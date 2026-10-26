#include "pdf_renderer.h"

#include <cmath>
#include <cstring>
#include <mutex>

#pragma comment(lib, "Kernel32.lib")

namespace {

// The renderer keeps its own small helpers so that this translation unit
// stays independent of the rest of the application. That lets the PDFium
// loader be exercised on its own by the test program.

// Returns the directory that holds the running executable.
std::wstring GetExecutableDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::wstring();
        }
        if (length < buffer.size() - 1) {
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    std::wstring path(buffer.data());
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return std::wstring();
    }
    return path.substr(0, separator);
}

// Joins two path components with a backslash.
std::wstring CombinePath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    return left + L"\\" + right;
}

// Reports whether a file exists.
bool PathExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Formats the last Windows error as text.
std::wstring DescribeLastError() {
    const DWORD code = ::GetLastError();
    if (code == 0) {
        return std::wstring();
    }
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message;
    if (length > 0 && buffer != nullptr) {
        message.assign(buffer, length);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                    message.back() == L' ' || message.back() == L'.')) {
            message.pop_back();
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    if (message.empty()) {
        message = L"error " + std::to_wstring(static_cast<unsigned long>(code));
    }
    return message;
}

// PDFium handle types. They are opaque pointers in the C API.
using FpdfDocumentHandle = void*;
using FpdfPageHandle = void*;
using FpdfBitmapHandle = void*;

// The subset of the PDFium C API that the renderer uses. The signatures
// mirror the ones in fpdfview.h so that the exported functions can be called
// directly.
using FpdfInitLibraryFn = void(__stdcall*)();
using FpdfDestroyLibraryFn = void(__stdcall*)();
using FpdfLoadMemDocumentFn = FpdfDocumentHandle(__stdcall*)(const void*, int, const char*);
using FpdfCloseDocumentFn = void(__stdcall*)(FpdfDocumentHandle);
using FpdfGetPageCountFn = int(__stdcall*)(FpdfDocumentHandle);
using FpdfLoadPageFn = FpdfPageHandle(__stdcall*)(FpdfDocumentHandle, int);
using FpdfClosePageFn = void(__stdcall*)(FpdfPageHandle);
using FpdfGetPageWidthFn = double(__stdcall*)(FpdfPageHandle);
using FpdfGetPageHeightFn = double(__stdcall*)(FpdfPageHandle);
using FpdfRenderPageBitmapFn = void(__stdcall*)(FpdfBitmapHandle, FpdfPageHandle,
                                                 int, int, int, int, int, int);
using FpdfBitmapCreateExFn = FpdfBitmapHandle(__stdcall*)(int, int, int, void*, int);
using FpdfBitmapFillRectFn = int(__stdcall*)(FpdfBitmapHandle, int, int, int, int, unsigned long);
using FpdfBitmapDestroyFn = void(__stdcall*)(FpdfBitmapHandle);

// FPDFBitmap_BGRA from fpdfview.h.
constexpr int kBitmapFormatBgra = 4;

// The colour used to clear a page before rendering. White in BGRA order.
constexpr unsigned long kWhiteBgra = 0xFFFFFFFFUL;

// Resolution used when the caller does not ask for a specific one.
constexpr int kDefaultDpi = 300;

// The maximum edge length of a rendered page. A very large page at a high
// resolution can otherwise ask for an unreasonable amount of memory.
constexpr int kMaxEdgePixels = 20000;

// Per-page raster budget (256 MiB). Anything larger fails with a clear
// message instead of throwing std::bad_alloc in a detached thread.
constexpr unsigned long long kMaxPageBufferBytes = 256ULL * 1024ULL * 1024ULL;

// Holds the resolved PDFium entry points.
struct PdfiumApi {
    FpdfInitLibraryFn initLibrary = nullptr;
    FpdfDestroyLibraryFn destroyLibrary = nullptr;
    FpdfLoadMemDocumentFn loadMemDocument = nullptr;
    FpdfCloseDocumentFn closeDocument = nullptr;
    FpdfGetPageCountFn getPageCount = nullptr;
    FpdfLoadPageFn loadPage = nullptr;
    FpdfClosePageFn closePage = nullptr;
    FpdfGetPageWidthFn getPageWidth = nullptr;
    FpdfGetPageHeightFn getPageHeight = nullptr;
    FpdfRenderPageBitmapFn renderPageBitmap = nullptr;
    FpdfBitmapCreateExFn bitmapCreateEx = nullptr;
    FpdfBitmapFillRectFn bitmapFillRect = nullptr;
    FpdfBitmapDestroyFn bitmapDestroy = nullptr;

    bool complete = false;
};

std::mutex gMutex;
HMODULE gModule = nullptr;
PdfiumApi gApi;
bool gInitialised = false;
bool gAttempted = false;
std::wstring gStatus;
// Serialises all rendering work. PDFium without FPDF_SetPlatformCallbacks
// is not thread-safe, and two concurrent RENDER jobs would otherwise race
// inside the library and crash the process.
std::mutex gRenderMutex;

// Resolves one exported symbol.
//
// The conversion from FARPROC to a specific function pointer is what the
// GetProcAddress contract requires. The intermediate step through a void
// pointer keeps the cast free of the incompatible function type warning
// that some compilers raise for a direct conversion.
template <typename T>
bool Resolve(HMODULE module, const char* name, T* target) {
    FARPROC address = ::GetProcAddress(module, name);
    if (address == nullptr) {
        return false;
    }
    void* raw = reinterpret_cast<void*>(address);
    T function = nullptr;
    std::memcpy(&function, &raw, sizeof(function));
    *target = function;
    return true;
}

// Searches the usual locations for pdfium.dll: next to the EXE, in a
// third_party folder next to the EXE and through the normal DLL search path.
std::wstring FindPdfiumLibrary() {
    const std::wstring moduleDirectory = GetExecutableDirectory();

    const std::wstring candidates[] = {
        CombinePath(moduleDirectory, L"pdfium.dll"),
        CombinePath(CombinePath(moduleDirectory, L"third_party"), L"pdfium.dll"),
        CombinePath(CombinePath(moduleDirectory, L"third_party"), L"pdfium\\pdfium.dll"),
    };
    for (const std::wstring& candidate : candidates) {
        if (PathExists(candidate)) {
            return candidate;
        }
    }
    return std::wstring();
}

// Loads the library and resolves the entry points. Safe to call repeatedly.
bool EnsurePdfiumLoaded(std::wstring* status) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gAttempted) {
        if (status) *status = gStatus;
        return gInitialised;
    }
    gAttempted = true;

    const std::wstring libraryPath = FindPdfiumLibrary();
    if (libraryPath.empty()) {
        gStatus = L"pdfium.dll was not found. Place it next to the executable, or in the third_party folder, to enable PDF printing.";
        if (status) *status = gStatus;
        return false;
    }

    gModule = ::LoadLibraryW(libraryPath.c_str());
    if (gModule == nullptr) {
        gStatus = L"pdfium.dll could not be loaded: " + DescribeLastError();
        if (status) *status = gStatus;
        return false;
    }

    bool resolved = true;
    resolved &= Resolve(gModule, "FPDF_InitLibrary", &gApi.initLibrary);
    resolved &= Resolve(gModule, "FPDF_DestroyLibrary", &gApi.destroyLibrary);
    resolved &= Resolve(gModule, "FPDF_LoadMemDocument", &gApi.loadMemDocument);
    resolved &= Resolve(gModule, "FPDF_CloseDocument", &gApi.closeDocument);
    resolved &= Resolve(gModule, "FPDF_GetPageCount", &gApi.getPageCount);
    resolved &= Resolve(gModule, "FPDF_LoadPage", &gApi.loadPage);
    resolved &= Resolve(gModule, "FPDF_ClosePage", &gApi.closePage);
    resolved &= Resolve(gModule, "FPDF_GetPageWidth", &gApi.getPageWidth);
    resolved &= Resolve(gModule, "FPDF_GetPageHeight", &gApi.getPageHeight);
    resolved &= Resolve(gModule, "FPDF_RenderPageBitmap", &gApi.renderPageBitmap);
    resolved &= Resolve(gModule, "FPDFBitmap_CreateEx", &gApi.bitmapCreateEx);
    resolved &= Resolve(gModule, "FPDFBitmap_FillRect", &gApi.bitmapFillRect);
    resolved &= Resolve(gModule, "FPDFBitmap_Destroy", &gApi.bitmapDestroy);

    if (!resolved) {
        // FPDF_GetPageWidth and FPDF_GetPageHeight are the only optional
        // entries; the rest are required for rendering.
        const bool optionalMissing = (gApi.getPageWidth == nullptr || gApi.getPageHeight == nullptr);
        if (!optionalMissing || gApi.renderPageBitmap == nullptr || gApi.loadMemDocument == nullptr) {
            ::FreeLibrary(gModule);
            gModule = nullptr;
            gStatus = L"pdfium.dll does not export the expected PDFium API. A different PDFium build may be required.";
            if (status) *status = gStatus;
            return false;
        }
    }

    gApi.complete = true;
    gApi.initLibrary();
    gInitialised = true;
    gStatus = L"PDFium is available for PDF printing.";
    if (status) *status = gStatus;
    return true;
}

// RAII wrapper around a PDFium document handle.
class ScopedPdfDocument {
public:
    explicit ScopedPdfDocument(FpdfDocumentHandle handle) : handle_(handle) {}
    ~ScopedPdfDocument() { reset(); }
    ScopedPdfDocument(const ScopedPdfDocument&) = delete;
    ScopedPdfDocument& operator=(const ScopedPdfDocument&) = delete;

    void reset() {
        if (handle_ != nullptr && gApi.closeDocument != nullptr) {
            gApi.closeDocument(handle_);
        }
        handle_ = nullptr;
    }

    FpdfDocumentHandle get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    FpdfDocumentHandle handle_ = nullptr;
};

// RAII wrapper around a PDFium page handle.
class ScopedPdfPage {
public:
    explicit ScopedPdfPage(FpdfPageHandle handle) : handle_(handle) {}
    ~ScopedPdfPage() { reset(); }
    ScopedPdfPage(const ScopedPdfPage&) = delete;
    ScopedPdfPage& operator=(const ScopedPdfPage&) = delete;

    void reset() {
        if (handle_ != nullptr && gApi.closePage != nullptr) {
            gApi.closePage(handle_);
        }
        handle_ = nullptr;
    }

    FpdfPageHandle get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    FpdfPageHandle handle_ = nullptr;
};

// RAII wrapper around a PDFium bitmap handle.
class ScopedPdfBitmap {
public:
    explicit ScopedPdfBitmap(FpdfBitmapHandle handle) : handle_(handle) {}
    ~ScopedPdfBitmap() { reset(); }
    ScopedPdfBitmap(const ScopedPdfBitmap&) = delete;
    ScopedPdfBitmap& operator=(const ScopedPdfBitmap&) = delete;

    void reset() {
        if (handle_ != nullptr && gApi.bitmapDestroy != nullptr) {
            gApi.bitmapDestroy(handle_);
        }
        handle_ = nullptr;
    }

    FpdfBitmapHandle get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    FpdfBitmapHandle handle_ = nullptr;
};

}  // namespace

bool IsPdfRendererAvailable() {
    std::wstring status;
    return EnsurePdfiumLoaded(&status);
}

std::wstring GetPdfRendererStatus() {
    std::wstring status;
    EnsurePdfiumLoaded(&status);
    return status;
}

bool RenderPdfPages(const std::vector<unsigned char>& pdfBytes,
                    int dpi,
                    std::vector<PdfPageBitmap>* pages,
                    std::wstring* errorText) {
    if (pages == nullptr) {
        return false;
    }
    pages->clear();

    if (pdfBytes.empty()) {
        if (errorText) *errorText = L"The PDF job carried no data.";
        return false;
    }

    std::wstring status;
    if (!EnsurePdfiumLoaded(&status)) {
        if (errorText) *errorText = status;
        return false;
    }

    // One job at a time inside PDFium (see gRenderMutex above).
    std::lock_guard<std::mutex> renderLock(gRenderMutex);

    const int resolution = dpi > 0 ? dpi : kDefaultDpi;
    const double scale = static_cast<double>(resolution) / 72.0;

    // The document buffer has to stay alive for the lifetime of the handle.
    ScopedPdfDocument document(
        gApi.loadMemDocument(pdfBytes.data(), static_cast<int>(pdfBytes.size()), nullptr));
    if (!document) {
        if (errorText) {
            *errorText = L"The PDF document could not be opened. It may be damaged or password protected.";
        }
        return false;
    }

    const int pageCount = gApi.getPageCount(document.get());
    if (pageCount <= 0) {
        if (errorText) *errorText = L"The PDF document contains no pages.";
        return false;
    }

    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        ScopedPdfPage page(gApi.loadPage(document.get(), pageIndex));
        if (!page) {
            if (errorText) {
                *errorText = L"Page " + std::to_wstring(pageIndex + 1) + L" of the PDF could not be loaded.";
            }
            return false;
        }

        // PDFium reports the page size in points, where one point is 1/72
        // inch. Converting through the requested resolution gives the pixel
        // size the page would have at that print resolution.
        double pageWidthPoints = 0.0;
        double pageHeightPoints = 0.0;
        if (gApi.getPageWidth != nullptr && gApi.getPageHeight != nullptr) {
            pageWidthPoints = gApi.getPageWidth(page.get());
            pageHeightPoints = gApi.getPageHeight(page.get());
        }
        if (pageWidthPoints <= 0.0 || pageHeightPoints <= 0.0) {
            pageWidthPoints = 612.0;   // US Letter
            pageHeightPoints = 792.0;
        }

        int pixelWidth = static_cast<int>(std::lround(pageWidthPoints * scale));
        int pixelHeight = static_cast<int>(std::lround(pageHeightPoints * scale));
        if (pixelWidth <= 0) pixelWidth = 1;
        if (pixelHeight <= 0) pixelHeight = 1;
        if (pixelWidth > kMaxEdgePixels) pixelWidth = kMaxEdgePixels;
        if (pixelHeight > kMaxEdgePixels) pixelHeight = kMaxEdgePixels;

        // Reject pages whose raster buffer would exceed the per-page budget
        // instead of letting the allocation throw and terminate the process.
        const unsigned long long pageBytes =
            static_cast<unsigned long long>(pixelWidth) * 4ULL *
            static_cast<unsigned long long>(pixelHeight);
        if (pageBytes > kMaxPageBufferBytes) {
            if (errorText) {
                *errorText = L"Page " + std::to_wstring(pageIndex + 1) +
                             L" is too large to rasterise at the requested resolution. " +
                             L"Print at a lower resolution or use a printer whose driver supports the PDF directly.";
            }
            return false;
        }

        const int stride = pixelWidth * 4;
        std::vector<unsigned char> buffer(static_cast<std::size_t>(stride) * static_cast<std::size_t>(pixelHeight), 0);

        ScopedPdfBitmap bitmap(
            gApi.bitmapCreateEx(pixelWidth, pixelHeight, kBitmapFormatBgra, buffer.data(), stride));
        if (!bitmap) {
            if (errorText) {
                *errorText = L"A rendering surface for page " + std::to_wstring(pageIndex + 1) +
                             L" could not be created.";
            }
            return false;
        }

        // Clear the page so that transparent areas stay white.
        gApi.bitmapFillRect(bitmap.get(), 0, 0, pixelWidth, pixelHeight, kWhiteBgra);
        gApi.renderPageBitmap(bitmap.get(), page.get(), 0, 0, pixelWidth, pixelHeight, 0, 0);

        PdfPageBitmap output;
        output.width = pixelWidth;
        output.height = pixelHeight;
        output.pixels = std::move(buffer);
        pages->push_back(std::move(output));
    }

    return true;
}
