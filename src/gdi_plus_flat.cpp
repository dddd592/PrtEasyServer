#include "gdi_plus_flat.h"

#include <cstring>
#include <mutex>

namespace {

// Formats the last Windows error as text. The resolver keeps its own helper
// so that this translation unit stays independent of the rest of the
// application.
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

}  // namespace

namespace GdiPlusFlat {

namespace {

std::mutex gMutex;
HMODULE gModule = nullptr;
Api gApi;
bool gAttempted = false;
bool gReady = false;
std::wstring gStatus;

// Resolves one exported symbol.
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

// Resolves a symbol that is known under more than one exported name. The
// image release function is called GdipDeleteBitmap on some toolchains and
// GdipDisposeImage on others.
template <typename T>
bool ResolveAny(HMODULE module, const char* first, const char* second, T* target) {
    if (Resolve(module, first, target)) {
        return true;
    }
    return Resolve(module, second, target);
}

}  // namespace

const Api& GetApi() {
    std::wstring ignored;
    EnsureApiLoaded(&ignored);
    return gApi;
}

bool EnsureApiLoaded(std::wstring* errorText) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gAttempted) {
        if (errorText) *errorText = gStatus;
        return gReady;
    }
    gAttempted = true;

    gModule = ::LoadLibraryW(L"gdiplus.dll");
    if (gModule == nullptr) {
        gStatus = L"gdiplus.dll could not be loaded: " + DescribeLastError();
        if (errorText) *errorText = gStatus;
        return false;
    }

    bool ok = true;
    ok &= Resolve(gModule, "GdipCreateBitmapFromStream", &gApi.createBitmapFromStream);
    ok &= Resolve(gModule, "GdipCreateBitmapFromScan0", &gApi.createBitmapFromScan0);
    ok &= Resolve(gModule, "GdipCreateHBITMAPFromBitmap", &gApi.createHBITMAPFromBitmap);
    ok &= Resolve(gModule, "GdipCreateFromHDC", &gApi.createFromHDC);
    ok &= Resolve(gModule, "GdipDeleteGraphics", &gApi.deleteGraphics);
    ok &= Resolve(gModule, "GdipDeleteFont", &gApi.deleteFont);
    ok &= Resolve(gModule, "GdipDeleteFontFamily", &gApi.deleteFontFamily);
    ok &= Resolve(gModule, "GdipDeleteStringFormat", &gApi.deleteStringFormat);
    ok &= Resolve(gModule, "GdipDeleteBrush", &gApi.deleteBrush);
    ok &= ResolveAny(gModule, "GdipDisposeImage", "GdipDeleteBitmap", &gApi.disposeImage);
    ok &= Resolve(gModule, "GdipGetImageWidth", &gApi.getImageWidth);
    ok &= Resolve(gModule, "GdipGetImageHeight", &gApi.getImageHeight);
    ok &= Resolve(gModule, "GdipGetImageHorizontalResolution", &gApi.getImageHorizontalResolution);
    ok &= Resolve(gModule, "GdipGetImageVerticalResolution", &gApi.getImageVerticalResolution);
    ok &= Resolve(gModule, "GdipSetInterpolationMode", &gApi.setInterpolationMode);
    ok &= Resolve(gModule, "GdipDrawImageRectI", &gApi.drawImageRectI);
    ok &= Resolve(gModule, "GdipFlush", &gApi.flush);
    ok &= Resolve(gModule, "GdipCreateFontFamilyFromName", &gApi.createFontFamilyFromName);
    ok &= Resolve(gModule, "GdipCreateFont", &gApi.createFont);
    ok &= Resolve(gModule, "GdipGetFontHeight", &gApi.getFontHeight);
    ok &= Resolve(gModule, "GdipCreateStringFormat", &gApi.createStringFormat);
    ok &= Resolve(gModule, "GdipSetStringFormatFlags", &gApi.setStringFormatFlags);
    ok &= Resolve(gModule, "GdipMeasureString", &gApi.measureString);
    ok &= Resolve(gModule, "GdipDrawString", &gApi.drawString);
    ok &= Resolve(gModule, "GdipCreateSolidFill", &gApi.createSolidFill);
    ok &= Resolve(gModule, "GdipSetTextRenderingHint", &gApi.setTextRenderingHint);
    ok &= Resolve(gModule, "GdipSetSmoothingMode", &gApi.setSmoothingMode);
    ok &= Resolve(gModule, "GdiplusStartup", &gApi.startup);
    ok &= Resolve(gModule, "GdiplusShutdown", &gApi.shutdown);

    if (!ok) {
        ::FreeLibrary(gModule);
        gModule = nullptr;
        gStatus = L"gdiplus.dll does not export the expected GDI+ flat API.";
        if (errorText) *errorText = gStatus;
        return false;
    }

    gApi.complete = true;
    gReady = true;
    gStatus = L"GDI+ is available.";
    if (errorText) *errorText = gStatus;
    return true;
}

}  // namespace GdiPlusFlat
