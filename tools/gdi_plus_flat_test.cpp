// Verifies the runtime GDI+ resolver.
//
// The renderer resolves the GDI+ flat API from gdiplus.dll at run time
// because the headers of the supported toolchains expose it differently.
// This check confirms that the resolver finds every entry point and that
// the library really works: a bitmap is decoded, measured and drawn.
//
// Usage:
//   gdi_plus_flat_test [image-file]

#include "gdi_plus_flat.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string NarrowForTest(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string buffer(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, buffer.data(), needed, nullptr, nullptr);
    if (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();
    return buffer;
}

// Builds a small PNG so the check does not depend on an external file.
std::vector<unsigned char> BuildSamplePng() {
    // A 2x2 PNG with one red pixel. The bytes are a fixed minimal PNG.
    static const unsigned char kPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
        0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };
    return std::vector<unsigned char>(kPng, kPng + sizeof(kPng));
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring status;
    if (!GdiPlusFlat::EnsureApiLoaded(&status)) {
        std::cout << "FAIL: " << NarrowForTest(status) << std::endl;
        return 1;
    }
    std::cout << "PASS: the GDI+ entry points were resolved" << std::endl;

    const GdiPlusFlat::Api& api = GdiPlusFlat::GetApi();
    if (!api.complete) {
        std::cout << "FAIL: the entry point table is incomplete" << std::endl;
        return 1;
    }

    // Start GDI+ up and draw into a memory device context.
    ULONG_PTR token = 0;
    GdiPlusFlat::StartupInputValue startupInput;
    GdiPlusFlat::StartupOutputValue startupOutput;
    if (api.startup(&token, &startupInput, &startupOutput) != GdiPlusFlat::kStatusOk) {
        std::cout << "FAIL: GDI+ could not be started" << std::endl;
        return 1;
    }
    std::cout << "PASS: GDI+ started" << std::endl;

    HDC screenDc = ::GetDC(nullptr);
    if (screenDc == nullptr) {
        api.shutdown(token);
        std::cout << "FAIL: no device context is available" << std::endl;
        return 1;
    }

    void* graphics = nullptr;
    if (api.createFromHDC(screenDc, &graphics) != GdiPlusFlat::kStatusOk || graphics == nullptr) {
        ::ReleaseDC(nullptr, screenDc);
        api.shutdown(token);
        std::cout << "FAIL: a graphics context could not be created" << std::endl;
        return 1;
    }
    std::cout << "PASS: a graphics context was created" << std::endl;

    // Decode a PNG from memory and read its size.
    std::vector<unsigned char> png = BuildSamplePng();
    HGLOBAL global = ::GlobalAlloc(GHND, png.size());
    void* buffer = ::GlobalLock(global);
    std::memcpy(buffer, png.data(), png.size());
    ::GlobalUnlock(global);

    IStream* stream = nullptr;
    void* bitmap = nullptr;
    bool decoded = false;
    if (SUCCEEDED(::CreateStreamOnHGlobal(global, TRUE, &stream)) && stream != nullptr) {
        if (api.createBitmapFromStream(stream, &bitmap) == GdiPlusFlat::kStatusOk && bitmap != nullptr) {
            decoded = true;
        }
        stream->Release();
    }

    unsigned int width = 0;
    unsigned int height = 0;
    if (decoded) {
        api.getImageWidth(bitmap, &width);
        api.getImageHeight(bitmap, &height);
        std::cout << "PASS: the image was decoded at " << width << "x" << height << " pixels" << std::endl;
        if (width == 0 || height == 0) {
            decoded = false;
        }
    }
    if (!decoded) {
        api.deleteGraphics(graphics);
        ::ReleaseDC(nullptr, screenDc);
        api.shutdown(token);
        std::cout << "FAIL: the image could not be decoded" << std::endl;
        return 1;
    }

    // Draw the bitmap and flush the result.
    api.setInterpolationMode(graphics, GdiPlusFlat::kInterpolationModeHighQualityBicubic);
    const int drawn = api.drawImageRectI(graphics, bitmap, 0, 0, static_cast<int>(width), static_cast<int>(height));
    api.flush(graphics, GdiPlusFlat::kFlushIntentionSync);
    if (drawn != GdiPlusFlat::kStatusOk) {
        api.disposeImage(bitmap);
        api.deleteGraphics(graphics);
        ::ReleaseDC(nullptr, screenDc);
        api.shutdown(token);
        std::cout << "FAIL: the image could not be drawn" << std::endl;
        return 1;
    }
    std::cout << "PASS: the image was drawn" << std::endl;

    // Measure a string so the text path is covered as well.
    void* family = nullptr;
    if (api.createFontFamilyFromName(L"Microsoft YaHei", nullptr, &family) == GdiPlusFlat::kStatusOk && family != nullptr) {
        void* font = nullptr;
        if (api.createFont(family, 16.0f, GdiPlusFlat::kFontStyleRegular, GdiPlusFlat::kUnitPixel, &font) == GdiPlusFlat::kStatusOk && font != nullptr) {
            void* format = nullptr;
            if (api.createStringFormat(0, 0x0804, &format) == GdiPlusFlat::kStatusOk && format != nullptr) {
                float layout[4] = {0.0f, 0.0f, 1000.0f, 1000.0f};
                float bounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                int chars = 0;
                int lines = 0;
                const wchar_t* text = L"PrtEasyServer";
                if (api.measureString(graphics, text, -1, font, layout, format, bounds, &chars, &lines) == GdiPlusFlat::kStatusOk) {
                    std::cout << "PASS: the text was measured, width " << bounds[2]
                              << " over " << lines << " line(s)" << std::endl;
                    if (bounds[2] <= 0.0f) {
                        api.deleteStringFormat(format);
                        api.deleteFont(font);
                        api.deleteFontFamily(family);
                        api.disposeImage(bitmap);
                        api.deleteGraphics(graphics);
                        ::ReleaseDC(nullptr, screenDc);
                        api.shutdown(token);
                        std::cout << "FAIL: the measured width is not positive" << std::endl;
                        return 1;
                    }
                } else {
                    std::cout << "FAIL: the text could not be measured" << std::endl;
                    api.deleteStringFormat(format);
                    api.deleteFont(font);
                    api.deleteFontFamily(family);
                    api.disposeImage(bitmap);
                    api.deleteGraphics(graphics);
                    ::ReleaseDC(nullptr, screenDc);
                    api.shutdown(token);
                    return 1;
                }
                api.deleteStringFormat(format);
            }
            api.deleteFont(font);
        }
        api.deleteFontFamily(family);
    }

    api.disposeImage(bitmap);
    api.deleteGraphics(graphics);
    ::ReleaseDC(nullptr, screenDc);
    api.shutdown(token);

    std::cout << "ALL CHECKS PASSED" << std::endl;
    return 0;
}
