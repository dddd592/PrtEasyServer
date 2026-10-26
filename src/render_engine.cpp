#include "render_engine.h"

#include "pdf_renderer.h"

#include <objbase.h>
#include <objidl.h>

#include "gdi_plus_flat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#pragma comment(lib, "Gdiplus.lib")

// Step-level trace sink used to localise crashes inside the render path.
namespace {
std::mutex gTraceMutex;
RenderTraceSink gTraceSink;
}

void SetRenderTraceSink(RenderTraceSink sink) {
    std::lock_guard<std::mutex> lock(gTraceMutex);
    gTraceSink = std::move(sink);
}

namespace {
void TraceStep(const wchar_t* message) {
    std::lock_guard<std::mutex> lock(gTraceMutex);
    if (gTraceSink) {
        gTraceSink(message);
    }
}

// The resolution used when rasterising a PDF page. It matches the
// resolution a laser printer usually applies, so the page keeps its
// proportions and stays legible.
constexpr int kPdfRasterDpi = 300;

struct PaperNameEntry {
    DWORD id;
    const wchar_t* name;
};

// Paper identifiers from wingdi.h. Only the sizes that matter in practice are
// mapped; anything else is reported by its numeric identifier.
const PaperNameEntry kPaperNames[] = {
    {DMPAPER_LETTER, L"Letter"},
    {DMPAPER_LETTERSMALL, L"Letter Small"},
    {DMPAPER_TABLOID, L"Tabloid"},
    {DMPAPER_LEDGER, L"Ledger"},
    {DMPAPER_LEGAL, L"Legal"},
    {DMPAPER_STATEMENT, L"Statement"},
    {DMPAPER_EXECUTIVE, L"Executive"},
    {DMPAPER_A3, L"A3"},
    {DMPAPER_A4, L"A4"},
    {DMPAPER_A4SMALL, L"A4 Small"},
    {DMPAPER_A5, L"A5"},
    {DMPAPER_B4, L"B4"},
    {DMPAPER_B5, L"B5"},
    {DMPAPER_FOLIO, L"Folio"},
    {DMPAPER_QUARTO, L"Quarto"},
    {DMPAPER_ENV_INVITE, L"Env Invite"},
    {DMPAPER_ENV_C5, L"Env C5"},
    {DMPAPER_ENV_C4, L"Env C4"},
    {DMPAPER_ENV_DL, L"Env DL"},
    {DMPAPER_ENV_C3, L"Env C3"},
    {DMPAPER_ENV_C6, L"Env C6"},
};

std::wstring PaperIdToName(DWORD id) {
    for (const PaperNameEntry& entry : kPaperNames) {
        if (entry.id == id) {
            return entry.name;
        }
    }
    return L"Custom (" + std::to_wstring(static_cast<unsigned long>(id)) + L")";
}

bool PaperNameToId(const std::wstring& name, DWORD* id) {
    const std::wstring normalized = ToLowerCopy(Trim(name));
    if (normalized.empty()) {
        return false;
    }
    for (const PaperNameEntry& entry : kPaperNames) {
        if (ToLowerCopy(entry.name) == normalized) {
            *id = entry.id;
            return true;
        }
    }
    return false;
}

// Parses "WIDTHxHEIGHT" expressed in millimetres, for example "102x152" for a
// 4x6 inch label. Returns false when the text is not in that shape.
bool ParseCustomPaperSize(const std::wstring& value, long* widthMm, long* heightMm) {
    const std::wstring trimmed = Trim(value);
    const std::size_t separator = trimmed.find(L'x');
    if (separator == std::wstring::npos) {
        return false;
    }
    const std::wstring widthText = Trim(trimmed.substr(0, separator));
    const std::wstring heightText = Trim(trimmed.substr(separator + 1));
    if (widthText.empty() || heightText.empty()) {
        return false;
    }
    for (wchar_t ch : widthText) {
        if (ch < L'0' || ch > L'9') return false;
    }
    for (wchar_t ch : heightText) {
        if (ch < L'0' || ch > L'9') return false;
    }
    *widthMm = _wtoi(widthText.c_str());
    *heightMm = _wtoi(heightText.c_str());
    return *widthMm > 0 && *heightMm > 0;
}

bool StartsWithBytes(const std::vector<unsigned char>& data, const char* magic, std::size_t length) {
    if (data.size() < length) return false;
    return std::memcmp(data.data(), magic, length) == 0;
}

// Decodes UTF-8 / UTF-16 payloads, honouring a byte order mark when present.
std::wstring DecodeTextBytes(const std::vector<unsigned char>& data) {
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        std::wstring text;
        for (std::size_t i = 2; i + 1 < data.size(); i += 2) {
            const unsigned int code = static_cast<unsigned int>(data[i]) |
                                      (static_cast<unsigned int>(data[i + 1]) << 8);
            text.push_back(static_cast<wchar_t>(code));
        }
        return text;
    }
    if (data.size() >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        std::wstring text;
        for (std::size_t i = 2; i + 1 < data.size(); i += 2) {
            const unsigned int code = (static_cast<unsigned int>(data[i]) << 8) |
                                      static_cast<unsigned int>(data[i + 1]);
            text.push_back(static_cast<wchar_t>(code));
        }
        return text;
    }
    return Utf8ToWide(std::string(data.begin(), data.end()));
}

std::wstring TrimTrailingNewlines(std::wstring value) {
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n')) {
        value.pop_back();
    }
    return value;
}

// Splits text on form feed characters. A form feed is the classic page
// separator, so it maps directly onto printer pages.
void SplitTextOnFormFeed(const std::wstring& text, std::vector<std::wstring>* sections) {
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L'\f') {
            sections->push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    sections->push_back(current);
}

// Owns a GDI+ object and releases it on scope exit.
struct BitmapDeleter {
    void operator()(void* pointer) const { GdiPlusFlat::GetApi().disposeImage(pointer); }
};
struct GraphicsDeleter {
    void operator()(void* pointer) const { GdiPlusFlat::GetApi().deleteGraphics(pointer); }
};
struct FontDeleter {
    void operator()(void* pointer) const { GdiPlusFlat::GetApi().deleteFont(pointer); }
};
struct StringFormatDeleter {
    void operator()(void* pointer) const { GdiPlusFlat::GetApi().deleteStringFormat(pointer); }
};
struct BrushDeleter {
    void operator()(void* pointer) const { GdiPlusFlat::GetApi().deleteBrush(pointer); }
};
struct FontFamilyDeleter {
    void operator()(void* pointer) const { GdiPlusFlat::GetApi().deleteFontFamily(pointer); }
};

// The template parameter is the handle type itself. The GDI+ handle aliases
// are already pointer types, so no extra indirection is added here.
template <typename Deleter>
class ScopedGdi {
public:
    explicit ScopedGdi(void* pointer = nullptr) : pointer_(pointer) {}
    ~ScopedGdi() { if (pointer_ != nullptr) Deleter{}(pointer_); }
    ScopedGdi(const ScopedGdi&) = delete;
    ScopedGdi& operator=(const ScopedGdi&) = delete;

    // Takes ownership of a handle, releasing any previous one.
    void Reset(void* pointer) {
        if (pointer_ != nullptr) {
            Deleter{}(pointer_);
        }
        pointer_ = pointer;
    }

    // Releases ownership without deleting.
    void* Release() {
        void* pointer = pointer_;
        pointer_ = nullptr;
        return pointer;
    }

    void* Get() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }

private:
    void* pointer_ = nullptr;
};

// Measures a single line without wrapping it.
float MeasureLineWidth(void* graphics,
                       void* font,
                       void* format,
                       const std::wstring& text) {
    if (text.empty() || graphics == nullptr || font == nullptr || format == nullptr) {
        return 0.0f;
    }
    float layout[4] = {0.0f, 0.0f, 100000.0f, 100000.0f};
    float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    INT chars = 0;
    INT lines = 0;
    if (GdiPlusFlat::GetApi().measureString(graphics, text.c_str(), -1, font,
                                             layout, format,
                                             bounds, &chars, &lines) != GdiPlusFlat::kStatusOk) {
        return 0.0f;
    }
    return bounds[2];
}

// Greedy word wrap. Wide characters are allowed to break between characters so
// that CJK text wraps correctly without needing a dictionary.
std::vector<std::wstring> WrapTextLines(void* graphics,
                                         void* font,
                                         void* format,
                                         const std::wstring& text,
                                         float maxWidth) {
    std::vector<std::wstring> lines;
    if (text.empty()) {
        lines.push_back(std::wstring());
        return lines;
    }

    std::wstring current;
    std::wstring word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        const std::wstring candidate = current + word;
        if (!current.empty() && MeasureLineWidth(graphics, font, format, candidate) > maxWidth) {
            lines.push_back(current);
            current = word;
        } else {
            current = candidate;
        }
        word.clear();
    };

    for (wchar_t ch : text) {
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            flushWord();
            lines.push_back(current);
            current.clear();
            continue;
        }
        if (ch == L' ' || ch == L'\t') {
            flushWord();
            current.push_back(L' ');
            continue;
        }
        const bool isWide = (ch >= 0x2E80 && ch <= 0x9FFF) || (ch >= 0xAC00 && ch <= 0xD7AF) ||
                            (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0xFF00 && ch <= 0xFF60);
        if (isWide) {
            flushWord();
            const std::wstring candidate = current + ch;
            if (!current.empty() && MeasureLineWidth(graphics, font, format, candidate) > maxWidth) {
                lines.push_back(current);
                current.clear();
            }
            current.push_back(ch);
            continue;
        }
        word.push_back(ch);
    }
    flushWord();
    lines.push_back(current);
    return lines;
}

struct PageGeometry {
    float dpiX = 300.0f;
    float dpiY = 300.0f;
    float originX = 0.0f;
    float originY = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

PageGeometry QueryPageGeometry(HDC printerDc) {
    PageGeometry geometry;
    const int dpiX = ::GetDeviceCaps(printerDc, LOGPIXELSX);
    const int dpiY = ::GetDeviceCaps(printerDc, LOGPIXELSY);
    geometry.dpiX = dpiX > 0 ? static_cast<float>(dpiX) : 300.0f;
    geometry.dpiY = dpiY > 0 ? static_cast<float>(dpiY) : 300.0f;

    const int horizontal = ::GetDeviceCaps(printerDc, HORZRES);
    const int vertical = ::GetDeviceCaps(printerDc, VERTRES);
    const int offsetX = ::GetDeviceCaps(printerDc, PHYSICALOFFSETX);
    const int offsetY = ::GetDeviceCaps(printerDc, PHYSICALOFFSETY);

    geometry.originX = static_cast<float>(offsetX);
    geometry.originY = static_cast<float>(offsetY);
    geometry.width = static_cast<float>(horizontal - offsetX * 2);
    geometry.height = static_cast<float>(vertical - offsetY * 2);
    if (geometry.width <= 0.0f) {
        geometry.width = static_cast<float>(horizontal);
    }
    if (geometry.height <= 0.0f) {
        geometry.height = static_cast<float>(vertical);
    }
    return geometry;
}

// Enumerates the paper sizes a queue supports.
//
// DeviceCapabilitiesW is avoided here: its final parameter differs between
// the MSVC and MinGW headers (WORD versus DEVMODEW*), so the portable
// DocumentProperties route is used instead. The DEVMODE reports the paper
// the queue is currently set to; the commonly used sizes are offered as
// suggestions so a caller always has something to pick.
bool QuerySupportedPaperIds(const std::wstring& printerName, HANDLE printerHandle,
                             std::vector<DWORD>* ids) {
    ids->clear();

    DEVMODEW current{};
    current.dmSize = sizeof(DEVMODEW);
    const LONG required = ::DocumentPropertiesW(nullptr, printerHandle,
                                                 const_cast<LPWSTR>(printerName.c_str()),
                                                 &current, nullptr, DM_OUT_BUFFER);
    if (required == IDOK) {
        ids->push_back(static_cast<DWORD>(current.dmPaperSize));
    }

    const DWORD commonSizes[] = {DMPAPER_LETTER, DMPAPER_LEGAL, DMPAPER_EXECUTIVE,
                                 DMPAPER_A3, DMPAPER_A4, DMPAPER_A5, DMPAPER_B5,
                                 DMPAPER_ENV_C5, DMPAPER_ENV_DL};
    for (DWORD size : commonSizes) {
        if (std::find(ids->begin(), ids->end(), size) == ids->end()) {
            ids->push_back(size);
        }
    }
    return true;
}

// Loads an image from memory through GDI+.
ScopedGdi<BitmapDeleter> LoadBitmapFromBytes(const std::vector<unsigned char>& data) {
    if (data.empty()) {
        return ScopedGdi<BitmapDeleter>(nullptr);
    }
    HGLOBAL global = ::GlobalAlloc(GHND, data.size());
    if (global == nullptr) {
        return ScopedGdi<BitmapDeleter>(nullptr);
    }
    void* destination = ::GlobalLock(global);
    if (destination == nullptr) {
        ::GlobalFree(global);
        return ScopedGdi<BitmapDeleter>(nullptr);
    }
    std::memcpy(destination, data.data(), data.size());
    ::GlobalUnlock(global);

    IStream* stream = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(global, TRUE, &stream)) || stream == nullptr) {
        ::GlobalFree(global);
        return ScopedGdi<BitmapDeleter>(nullptr);
    }

    void* bitmap = nullptr;
    const int status = GdiPlusFlat::GetApi().createBitmapFromStream(stream, &bitmap);
    stream->Release();
    if (status != GdiPlusFlat::kStatusOk || bitmap == nullptr) {
        return ScopedGdi<BitmapDeleter>(nullptr);
    }
    return ScopedGdi<BitmapDeleter>(bitmap);
}

// Draws one image page, honouring scale, fit-to-page and monochrome.
//
// The page is either an encoded image file or an already rasterised buffer
// with the pixels stored top-down in 32bpp BGRA order.
bool DrawImagePage(void* graphics,
                   const RenderPage& page,
                   const RenderJobOptions& options,
                   const PageGeometry& geometry) {
    ScopedGdi<BitmapDeleter> bitmap;
    UINT imageWidth = 0;
    UINT imageHeight = 0;
    float imageDpiX = 96.0f;
    float imageDpiY = 96.0f;

    if (page.hasRawPixels) {
        // Wrap the rasterised pixels in a GDI+ bitmap so the rest of the
        // drawing code can treat them like any other image.
        const int stride = page.pixelWidth * 4;
        if (page.pixelWidth <= 0 || page.pixelHeight <= 0 ||
            page.rawPixels.size() < static_cast<std::size_t>(stride) * static_cast<std::size_t>(page.pixelHeight)) {
            return false;
        }
        // The rasteriser hands over the rows in top-down order. A negative
        // stride tells GDI+ that the buffer is stored that way, so the page
        // does not have to be flipped before it is drawn.
        //
        // With a negative stride the buffer pointer has to address the first
        // row of the image, which is the last row in storage order.
        void* rawBitmap = nullptr;
        unsigned char* pixelBuffer = const_cast<unsigned char*>(page.rawPixels.data());
        unsigned char* topDownStart = pixelBuffer +
            static_cast<std::size_t>(stride) * static_cast<std::size_t>(page.pixelHeight - 1);
        if (GdiPlusFlat::GetApi().createBitmapFromScan0(page.pixelWidth, page.pixelHeight, -stride,
                                               GdiPlusFlat::kPixelFormat32bppARGB,
                                               topDownStart, &rawBitmap) != GdiPlusFlat::kStatusOk ||
            rawBitmap == nullptr) {
            return false;
        }
        bitmap.Reset(rawBitmap);
        imageWidth = static_cast<UINT>(page.pixelWidth);
        imageHeight = static_cast<UINT>(page.pixelHeight);
        // A rasterised page has no resolution of its own, so it is treated
        // as being rendered at the resolution it was rasterised at.
        imageDpiX = static_cast<float>(kPdfRasterDpi);
        imageDpiY = static_cast<float>(kPdfRasterDpi);
    } else {
        ScopedGdi<BitmapDeleter> decoded = LoadBitmapFromBytes(page.imageBytes);
        if (!decoded) {
            return false;
        }
        bitmap.Reset(decoded.Release());
        GdiPlusFlat::GetApi().getImageWidth(static_cast<void**>(bitmap.Get()), &imageWidth);
        GdiPlusFlat::GetApi().getImageHeight(static_cast<void**>(bitmap.Get()), &imageHeight);
        if (imageWidth == 0 || imageHeight == 0) {
            return false;
        }
    }

    // Physical size is derived from the image resolution so a 300 dpi photo
    // prints at its true physical size instead of being scaled to the page.
    if (!page.hasRawPixels) {
        GdiPlusFlat::GetApi().getImageHorizontalResolution(static_cast<void**>(bitmap.Get()), &imageDpiX);
        GdiPlusFlat::GetApi().getImageVerticalResolution(static_cast<void**>(bitmap.Get()), &imageDpiY);
    }
    if (imageDpiX <= 0.0f) imageDpiX = 96.0f;
    if (imageDpiY <= 0.0f) imageDpiY = 96.0f;

    float targetWidth = static_cast<float>(imageWidth) / imageDpiX * geometry.dpiX;
    float targetHeight = static_cast<float>(imageHeight) / imageDpiY * geometry.dpiY;

    const float scale = static_cast<float>(options.scalePercent) / 100.0f;
    if (scale > 0.0f && std::fabs(scale - 1.0f) > 0.001f) {
        targetWidth *= scale;
        targetHeight *= scale;
    }

    if (options.fitToPage) {
        if (targetWidth > geometry.width && geometry.width > 0.0f) {
            const float ratio = geometry.width / targetWidth;
            targetWidth = geometry.width;
            targetHeight *= ratio;
        }
        if (targetHeight > geometry.height && geometry.height > 0.0f) {
            const float ratio = geometry.height / targetHeight;
            targetHeight = geometry.height;
            targetWidth *= ratio;
        }
    }

    const float drawWidth = std::max(1.0f, targetWidth);
    const float drawHeight = std::max(1.0f, targetHeight);

    const float margin = static_cast<float>(options.marginInches) * geometry.dpiX;
    float drawX = geometry.originX + margin + std::max(0.0f, (geometry.width - drawWidth) / 2.0f);
    float drawY = geometry.originY + margin;
    if (drawX + drawWidth > geometry.originX + geometry.width) {
        drawX = geometry.originX + margin;
    }
    if (drawY + drawHeight > geometry.originY + geometry.height) {
        drawY = geometry.originY + margin;
    }

    if (options.monochrome) {
        // Convert to a one-bit-per-pixel copy so the driver receives a clean
        // bilevel image, which is what thermal and label printers expect.
        // The conversion happens on an off-screen bitmap so nothing is drawn
        // onto the page during this step.
        void* rawMono = nullptr;
        // The bilevel format is requested with the value that GDI+ itself
        // uses on Windows, which the resolver header defines.
        if (GdiPlusFlat::GetApi().createBitmapFromScan0(static_cast<INT>(imageWidth), static_cast<INT>(imageHeight), 0,
                                               GdiPlusFlat::kPixelFormat1bppIndexed,
                                               nullptr, &rawMono) != GdiPlusFlat::kStatusOk) {
            return false;
        }
        ScopedGdi<BitmapDeleter> mono(rawMono);

        // Draw the source image into the bilevel bitmap. The bitmap is
        // selected into an off-screen device context so nothing reaches the
        // page during this step.
        HBITMAP monoHandle = nullptr;
        if (GdiPlusFlat::GetApi().createHBITMAPFromBitmap(mono.Get(), &monoHandle, 0) != GdiPlusFlat::kStatusOk || monoHandle == nullptr) {
            return false;
        }
        HDC memoryDc = ::CreateCompatibleDC(nullptr);
        if (memoryDc == nullptr) {
            ::DeleteObject(monoHandle);
            return false;
        }
        HGDIOBJ selected = ::SelectObject(memoryDc, monoHandle);
        void* rawMonoGraphics = nullptr;
        bool ok = false;
        if (GdiPlusFlat::GetApi().createFromHDC(memoryDc, &rawMonoGraphics) == GdiPlusFlat::kStatusOk && rawMonoGraphics != nullptr) {
            ScopedGdi<GraphicsDeleter> monoGraphics(rawMonoGraphics);
            GdiPlusFlat::GetApi().setInterpolationMode(monoGraphics.Get(), GdiPlusFlat::kInterpolationModeNearestNeighbor);
            ok = GdiPlusFlat::GetApi().drawImageRectI(monoGraphics.Get(), bitmap.Get(),
                                             0, 0, static_cast<INT>(imageWidth),
                                             static_cast<INT>(imageHeight)) == GdiPlusFlat::kStatusOk;
            GdiPlusFlat::GetApi().flush(monoGraphics.Get(), GdiPlusFlat::kFlushIntentionSync);
        }
        if (selected != nullptr) {
            ::SelectObject(memoryDc, selected);
        }
        ::DeleteDC(memoryDc);
        ::DeleteObject(monoHandle);
        if (!ok) {
            return false;
        }

        GdiPlusFlat::GetApi().setInterpolationMode(graphics, GdiPlusFlat::kInterpolationModeNearestNeighbor);
        if (GdiPlusFlat::GetApi().drawImageRectI(graphics, mono.Get(),
                                         static_cast<INT>(drawX), static_cast<INT>(drawY),
                                         static_cast<INT>(drawWidth), static_cast<INT>(drawHeight)) != GdiPlusFlat::kStatusOk) {
            return false;
        }
        return true;
    }

    GdiPlusFlat::GetApi().setInterpolationMode(graphics, GdiPlusFlat::kInterpolationModeHighQualityBicubic);
    if (GdiPlusFlat::GetApi().drawImageRectI(graphics, bitmap.Get(),
                                     static_cast<INT>(drawX), static_cast<INT>(drawY),
                                     static_cast<INT>(drawWidth), static_cast<INT>(drawHeight)) != GdiPlusFlat::kStatusOk) {
        return false;
    }
    return true;
}

// Draws one text page, wrapping and paginating as needed.
int DrawTextPage(void* graphics,
                 const std::wstring& text,
                 const RenderJobOptions& options,
                 const PageGeometry& geometry,
                 const std::function<void()>& beginPage,
                 const std::function<void()>& endPage) {
    const float margin = static_cast<float>(options.marginInches) * geometry.dpiX;
    const float usableWidth = std::max(1.0f, geometry.width - margin * 2.0f);
    const float usableHeight = std::max(1.0f, geometry.height - margin * 2.0f);
    const float pageBottom = geometry.originY + margin + usableHeight;

    std::wstring face = Trim(options.fontFace);
    if (face.empty()) {
        face = L"Microsoft YaHei";
    }
    const float fontSizePixels = std::max(4.0f, static_cast<float>(options.fontSizePoints) * geometry.dpiY / 72.0f);

    void* family = nullptr;
    if (GdiPlusFlat::GetApi().createFontFamilyFromName(face.c_str(), nullptr, &family) != GdiPlusFlat::kStatusOk || family == nullptr) {
        return 0;
    }
    ScopedGdi<FontFamilyDeleter> familyGuard(family);

    void* rawFont = nullptr;
    if (GdiPlusFlat::GetApi().createFont(family, fontSizePixels, GdiPlusFlat::kFontStyleRegular, GdiPlusFlat::kUnitPixel, &rawFont) != GdiPlusFlat::kStatusOk) {
        return 0;
    }
    ScopedGdi<FontDeleter> font(rawFont);

    void* rawFormat = nullptr;
    if (GdiPlusFlat::GetApi().createStringFormat(0, static_cast<LANGID>(0x0804), &rawFormat) != GdiPlusFlat::kStatusOk) {
        return 0;
    }
    ScopedGdi<StringFormatDeleter> format(rawFormat);
    GdiPlusFlat::GetApi().setStringFormatFlags(format.Get(), GdiPlusFlat::kStringFormatFlagsMeasureTrailingSpaces |
                                          GdiPlusFlat::kStringFormatFlagsNoWrap);

    void* rawSolidBrush = nullptr;
    if (GdiPlusFlat::GetApi().createSolidFill(0xFF000000, &rawSolidBrush) != GdiPlusFlat::kStatusOk ||
        rawSolidBrush == nullptr) {
        return 0;
    }
    void* rawBrush = rawSolidBrush;
    ScopedGdi<BrushDeleter> brush(rawBrush);

    // Line height is derived from the font metrics of the printer device.
    float emHeight = 0.0f;
    GdiPlusFlat::GetApi().getFontHeight(font.Get(), graphics, &emHeight);
    if (emHeight <= 0.0f) {
        emHeight = fontSizePixels;
    }
    const float lineHeight = std::max(1.0f, emHeight * 1.2f);

    std::vector<std::wstring> lines = WrapTextLines(graphics, font.Get(), format.Get(), text, usableWidth);
    int pageCount = 0;
    float y = geometry.originY + margin;
    bool pageOpen = false;

    for (const std::wstring& line : lines) {
        if (!pageOpen) {
            beginPage();
            pageOpen = true;
            ++pageCount;
            y = geometry.originY + margin;
        }
        if (y + lineHeight > pageBottom) {
            endPage();
            beginPage();
            ++pageCount;
            y = geometry.originY + margin;
        }
        float target[4] = {geometry.originX + margin, y, usableWidth, lineHeight};
        GdiPlusFlat::GetApi().drawString(graphics, line.c_str(), -1, font.Get(),
                                         target, format.Get(), brush.Get());
        y += lineHeight;
    }

    if (!pageOpen) {
        beginPage();
        pageCount = 1;
    }
    return pageCount;
}

}  // namespace

RenderJobKind DetectRenderJobKind(const std::vector<unsigned char>& data) {
    if (data.empty()) {
        return RenderJobKind::Unknown;
    }
    if (StartsWithBytes(data, "\x89PNG", 4)) {
        return RenderJobKind::Image;
    }
    if (StartsWithBytes(data, "\xFF\xD8\xFF", 3)) {
        return RenderJobKind::Image;
    }
    if (StartsWithBytes(data, "GIF8", 4)) {
        return RenderJobKind::Image;
    }
    if (StartsWithBytes(data, "BM", 2)) {
        return RenderJobKind::Image;
    }
    if (StartsWithBytes(data, "II*\x00", 4) || StartsWithBytes(data, "MM\x00*", 4)) {
        return RenderJobKind::Image;
    }
    if (StartsWithBytes(data, "\x01\x00\x00\x00", 4) || StartsWithBytes(data, "\xD7\xCD\xC6\x9A", 4)) {
        return RenderJobKind::Image;
    }
    if (StartsWithBytes(data, "%PDF", 4)) {
        return RenderJobKind::Pdf;
    }
    if (data.size() >= 4 && data[0] == 'P' && data[1] == 'K') {
        return RenderJobKind::Xps;
    }
    return RenderJobKind::Text;
}

bool SplitRenderPages(const std::vector<unsigned char>& data,
                      RenderJobKind kind,
                      std::vector<RenderPage>* pages,
                      std::wstring* errorText) {
    if (pages == nullptr) {
        return false;
    }
    pages->clear();

    if (data.empty()) {
        if (errorText) *errorText = L"The print job carried no data.";
        return false;
    }

    if (kind == RenderJobKind::Image) {
        RenderPage page;
        page.isImage = true;
        page.imageBytes = data;
        pages->push_back(page);
        return true;
    }

    if (kind == RenderJobKind::Pdf) {
        // A PDF is rasterised with PDFium. The resolution follows the page
        // size so that a page ends up with the same pixel count it would
        // have at that print resolution.
        std::vector<PdfPageBitmap> pdfPages;
        std::wstring pdfError;
        if (!RenderPdfPages(data, kPdfRasterDpi, &pdfPages, &pdfError)) {
            if (errorText) *errorText = pdfError;
            return false;
        }
        for (const PdfPageBitmap& pdfPage : pdfPages) {
            RenderPage page;
            page.isImage = true;
            page.hasRawPixels = true;
            page.rawPixels = pdfPage.pixels;
            page.pixelWidth = pdfPage.width;
            page.pixelHeight = pdfPage.height;
            pages->push_back(std::move(page));
        }
        return true;
    }

    if (kind == RenderJobKind::Text) {
        const std::wstring text = DecodeTextBytes(data);
        std::vector<std::wstring> sections;
        SplitTextOnFormFeed(text, &sections);
        for (const std::wstring& section : sections) {
            RenderPage page;
            page.isImage = false;
            page.text = section;
            pages->push_back(page);
        }
        if (pages->empty()) {
            RenderPage page;
            page.isImage = false;
            page.text = text;
            pages->push_back(page);
        }
        return true;
    }

    if (errorText) {
        *errorText = L"Unsupported payload for the render path.";
    }
    return false;
}

bool ApplyRenderOptionsToDevMode(void* devMode,
                                  const RenderJobOptions& options,
                                  std::wstring* errorText) {
    if (devMode == nullptr) {
        if (errorText) *errorText = L"Missing DEVMODE block.";
        return false;
    }
    DEVMODEW* mode = reinterpret_cast<DEVMODEW*>(devMode);

    DWORD fields = 0;

    const std::wstring paper = Trim(options.paperSize);
    if (!paper.empty()) {
        DWORD paperId = 0;
        if (PaperNameToId(paper, &paperId)) {
            mode->dmPaperSize = static_cast<short>(paperId);
            fields |= DM_PAPERSIZE;
        } else {
            long widthMm = 0;
            long heightMm = 0;
            if (ParseCustomPaperSize(paper, &widthMm, &heightMm)) {
                mode->dmPaperSize = DMPAPER_USER;
                mode->dmPaperWidth = static_cast<short>(widthMm * 10);
                mode->dmPaperLength = static_cast<short>(heightMm * 10);
                fields |= DM_PAPERSIZE;
            } else {
                bool numeric = true;
                for (wchar_t ch : paper) {
                    if (ch < L'0' || ch > L'9') {
                        numeric = false;
                        break;
                    }
                }
                if (numeric) {
                    mode->dmPaperSize = static_cast<short>(_wtoi(paper.c_str()));
                    fields |= DM_PAPERSIZE;
                } else if (errorText) {
                    *errorText = L"Unknown paper size: " + paper;
                    return false;
                }
            }
        }
    }

    const std::wstring orientation = ToLowerCopy(Trim(options.orientation));
    if (orientation == L"portrait") {
        mode->dmOrientation = DMORIENT_PORTRAIT;
        fields |= DM_ORIENTATION;
    } else if (orientation == L"landscape") {
        mode->dmOrientation = DMORIENT_LANDSCAPE;
        fields |= DM_ORIENTATION;
    }

    if (options.copies > 0) {
        mode->dmCopies = static_cast<short>(options.copies);
        fields |= DM_COPIES;
    }

    mode->dmFields |= fields;
    return true;
}

bool QueryPrinterPaperSizes(const std::wstring& printerName,
                            std::vector<std::wstring>* paperSizes,
                            std::wstring* currentPaper,
                            std::wstring* currentOrientation) {
    HANDLE printerHandle = nullptr;
    if (!::OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()), &printerHandle, nullptr)) {
        return false;
    }

    if (paperSizes) {
        paperSizes->clear();
        std::vector<DWORD> ids;
        QuerySupportedPaperIds(printerName, printerHandle, &ids);
        for (DWORD id : ids) {
            const std::wstring name = PaperIdToName(id);
            if (std::find(paperSizes->begin(), paperSizes->end(), name) == paperSizes->end()) {
                paperSizes->push_back(name);
            }
        }
        if (paperSizes->empty()) {
            paperSizes->push_back(L"Letter");
            paperSizes->push_back(L"A4");
            paperSizes->push_back(L"A5");
            paperSizes->push_back(L"Legal");
        }
    }

    const LONG size = ::DocumentPropertiesW(nullptr, printerHandle,
                                             const_cast<LPWSTR>(printerName.c_str()),
                                             nullptr, nullptr, 0);
    if (size > 0) {
        std::vector<BYTE> buffer(static_cast<std::size_t>(size));
        DEVMODEW* mode = reinterpret_cast<DEVMODEW*>(buffer.data());
        mode->dmSize = static_cast<WORD>(size);
        if (::DocumentPropertiesW(nullptr, printerHandle,
                                  const_cast<LPWSTR>(printerName.c_str()),
                                  mode, nullptr, DM_OUT_BUFFER) == IDOK) {
            if (currentPaper) {
                *currentPaper = PaperIdToName(static_cast<DWORD>(mode->dmPaperSize));
            }
            if (currentOrientation) {
                *currentOrientation = (mode->dmOrientation == DMORIENT_LANDSCAPE) ? L"landscape" : L"portrait";
            }
        }
    }

    ::ClosePrinter(printerHandle);
    return true;
}

RenderResult RenderPagesToPrinter(const std::vector<RenderPage>& pages,
                                   const std::wstring& printerName,
                                   const RenderJobOptions& options) {
    RenderResult result;
    if (printerName.empty()) {
        result.errorText = L"No printer was selected for the render job.";
        return result;
    }
    if (pages.empty()) {
        result.errorText = L"The render job contained no pages.";
        return result;
    }

    HANDLE printerHandle = nullptr;
    if (!::OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()), &printerHandle, nullptr)) {
        result.errorText = L"OpenPrinter failed: " + FormatLastErrorMessage();
        return result;
    }
    TraceStep(L"[render-trace] printer opened");

    // Build the DEVMODE for this queue so paper, orientation and copies are
    // honoured by the driver itself. The driver's DEVMODE carries
    // dmDriverExtra private bytes; the full buffer has to stay alive and be
    // handed to CreateDCW verbatim. Copying it into a plain DEVMODEW keeps
    // the dmDriverExtra count but drops the storage, and CreateDCW then
    // reads past the end of the stack object (access violation).
    std::vector<BYTE> devModeBuffer;
    DEVMODEW* devModePtr = nullptr;
    DEVMODEW fallbackDevMode{};
    const LONG required = ::DocumentPropertiesW(nullptr, printerHandle,
                                                const_cast<LPWSTR>(printerName.c_str()),
                                                nullptr, nullptr, 0);
    if (required > 0) {
        devModeBuffer.assign(static_cast<std::size_t>(required), 0);
        DEVMODEW* mode = reinterpret_cast<DEVMODEW*>(devModeBuffer.data());
        mode->dmSize = static_cast<WORD>(required);
        if (::DocumentPropertiesW(nullptr, printerHandle,
                                  const_cast<LPWSTR>(printerName.c_str()),
                                  mode, nullptr, DM_OUT_BUFFER) == IDOK) {
            std::wstring ignored;
            ApplyRenderOptionsToDevMode(mode, options, &ignored);
            devModePtr = mode;
            TraceStep(L"[render-trace] devmode prepared");
        }
    }
    if (devModePtr == nullptr) {
        fallbackDevMode.dmSize = sizeof(DEVMODEW);
        fallbackDevMode.dmSpecVersion = DM_SPECVERSION;
        fallbackDevMode.dmDriverVersion = DM_SPECVERSION;
        fallbackDevMode.dmFields = DM_ORIENTATION | DM_PAPERSIZE | DM_COPIES;
        fallbackDevMode.dmOrientation = DMORIENT_PORTRAIT;
        fallbackDevMode.dmPaperSize = DMPAPER_A4;
        fallbackDevMode.dmCopies = 1;
        ApplyRenderOptionsToDevMode(&fallbackDevMode, options, nullptr);
        devModePtr = &fallbackDevMode;
    }

    // The "WINSPDC" driver turns the queue's own driver into a device
    // context, so GDI+ drawing is converted by the driver into whatever
    // language the printer actually speaks.
    HDC printerDc = ::CreateDCW(L"WINSPDC", printerName.c_str(), nullptr, devModePtr);
    if (printerDc == nullptr) {
        ::ClosePrinter(printerHandle);
        result.errorText = L"CreateDC failed for the printer device context: " + FormatLastErrorMessage();
        return result;
    }
    TraceStep(L"[render-trace] printer DC created");

    ULONG_PTR gdiplusToken = 0;
    GdiPlusFlat::StartupInputValue gdiplusInput;
    GdiPlusFlat::StartupOutputValue gdiplusOutput;
    if (GdiPlusFlat::GetApi().startup(&gdiplusToken, &gdiplusInput, &gdiplusOutput) != GdiPlusFlat::kStatusOk) {
        ::DeleteDC(printerDc);
        ::ClosePrinter(printerHandle);
        result.errorText = L"GDI+ could not be initialised.";
        return result;
    }
    TraceStep(L"[render-trace] GDI+ started");

    DOC_INFO_1W docInfo{};
    docInfo.pDocName = const_cast<LPWSTR>(L"PrtEasyServer Render Job");
    docInfo.pDatatype = nullptr;

    const DWORD jobId = ::StartDocPrinterW(printerHandle, 1, reinterpret_cast<LPBYTE>(&docInfo));
    if (jobId == 0) {
        GdiPlusFlat::GetApi().shutdown(gdiplusToken);
        ::DeleteDC(printerDc);
        ::ClosePrinter(printerHandle);
        result.errorText = L"StartDocPrinter failed: " + FormatLastErrorMessage();
        return result;
    }
    TraceStep(L"[render-trace] doc started");

    void* rawGraphics = nullptr;
    if (GdiPlusFlat::GetApi().createFromHDC(printerDc, &rawGraphics) != GdiPlusFlat::kStatusOk || rawGraphics == nullptr) {
        ::EndDocPrinter(printerHandle);
        GdiPlusFlat::GetApi().shutdown(gdiplusToken);
        ::DeleteDC(printerDc);
        ::ClosePrinter(printerHandle);
        result.errorText = L"A GDI+ graphics context could not be created for the printer.";
        return result;
    }
    ScopedGdi<GraphicsDeleter> graphics(rawGraphics);
    GdiPlusFlat::GetApi().setTextRenderingHint(graphics.Get(), GdiPlusFlat::kTextRenderingHintAntiAliasGridFit);
    GdiPlusFlat::GetApi().setSmoothingMode(graphics.Get(), GdiPlusFlat::kSmoothingModeHighQuality);
    TraceStep(L"[render-trace] graphics created");

    const PageGeometry geometry = QueryPageGeometry(printerDc);
    TraceStep(L"[render-trace] geometry queried");

    int pageCount = 0;
    bool failed = false;
    std::wstring failure;

    for (const RenderPage& page : pages) {
        TraceStep(page.isImage ? L"[render-trace] drawing image page"
                               : L"[render-trace] drawing text page");
        if (page.isImage) {
            if (!::StartPagePrinter(printerHandle)) {
                failed = true;
                failure = L"StartPagePrinter failed: " + FormatLastErrorMessage();
                break;
            }
            GdiPlusFlat::GetApi().flush(graphics.Get(), GdiPlusFlat::kFlushIntentionSync);
            if (!DrawImagePage(graphics.Get(), page, options, geometry)) {
                failed = true;
                failure = L"The image payload could not be decoded for rendering.";
                break;
            }
            GdiPlusFlat::GetApi().flush(graphics.Get(), GdiPlusFlat::kFlushIntentionSync);
            ::EndPagePrinter(printerHandle);
            ++pageCount;
        } else {
            bool pageStarted = false;
            const int drawn = DrawTextPage(
                graphics.Get(), TrimTrailingNewlines(page.text), options, geometry,
                [&]() {
                    ::StartPagePrinter(printerHandle);
                    pageStarted = true;
                },
                [&]() {
                    GdiPlusFlat::GetApi().flush(graphics.Get(), GdiPlusFlat::kFlushIntentionSync);
                    ::EndPagePrinter(printerHandle);
                    pageStarted = false;
                });
            if (drawn <= 0) {
                failed = true;
                failure = L"The text payload could not be rendered.";
                break;
            }
            if (pageStarted) {
                GdiPlusFlat::GetApi().flush(graphics.Get(), GdiPlusFlat::kFlushIntentionSync);
                ::EndPagePrinter(printerHandle);
            }
            pageCount += drawn;
        }
    }

    GdiPlusFlat::GetApi().flush(graphics.Get(), GdiPlusFlat::kFlushIntentionSync);
    ::EndDocPrinter(printerHandle);
    GdiPlusFlat::GetApi().shutdown(gdiplusToken);
    ::DeleteDC(printerDc);
    ::ClosePrinter(printerHandle);
    TraceStep(L"[render-trace] doc finished");

    if (failed) {
        result.errorText = failure;
        return result;
    }

    result.ok = true;
    result.pageCount = pageCount;
    result.description = FormatString(L"rendered %d page(s) through the printer driver", pageCount);
    return result;
}
