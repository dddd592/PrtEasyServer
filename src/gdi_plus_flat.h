#pragma once

#include "common.h"

// GDI+ flat API access.
//
// The flat API lives in Gdiplusflat.h, but the headers of the supported
// toolchains expose it differently: MSVC does not pull the flat API into
// gdiplus.h at all, while other toolchains publish it inside a nested
// namespace. The exported names differ as well, for example the image
// release function is GdipDeleteBitmap on MSVC and GdipDisposeImage
// elsewhere.
//
// The renderer therefore declares the few entry points it needs itself and
// resolves them from gdiplus.dll at run time. That keeps one implementation
// working on every toolchain, and it also means a missing or unexpected
// library is reported instead of failing to link.
//
// The declarations follow the documented GDI+ flat API signatures. All GDI+
// handles are opaque pointers, so void* is used for them. The status code is
// an enum on every platform and is represented as int here.
//
// The rectangles are passed by pointer, and GDI+ expects the four floats of
// a RectF in the order x, y, width, height. A plain float array has exactly
// that layout.

namespace GdiPlusFlat {

// The GDI+ status code that means success.
constexpr int kStatusOk = 0;

// The GDI+ enumerations that this renderer needs. The values match the
// documented GDI+ values and are shared by every platform.
// The GDI+ pixel format identifiers.
//
// The SDK does not use an enumeration for these. The MSVC headers define
// them as bit field macros in gdipluspixelformats.h, while some other
// toolchains publish a small enumeration with different values. GDI+ itself
// expects the MSVC values, so those are used here.
//
//   PixelFormat32bppARGB   = 10 | (32 << 8) | Alpha | GDI | Canonical
//   PixelFormat1bppIndexed =  1 | ( 1 << 8) | Indexed | GDI
enum PixelFormatValue {
    kPixelFormat32bppARGB = 0x0026200A,
    kPixelFormat1bppIndexed = 0x00030101,
};

enum UnitValue {
    kUnitPixel = 2,
};

enum InterpolationModeValue {
    kInterpolationModeNearestNeighbor = 0,
    kInterpolationModeHighQualityBicubic = 7,
};

enum TextRenderingHintValue {
    kTextRenderingHintAntiAliasGridFit = 3,
};

enum SmoothingModeValue {
    kSmoothingModeHighQuality = 1,
};

enum FlushIntentionValue {
    kFlushIntentionSync = 0,
};

enum FontStyleValue {
    kFontStyleRegular = 0,
};

enum StringFormatFlagsValue {
    kStringFormatFlagsMeasureTrailingSpaces = 1,
    kStringFormatFlagsNoWrap = 4,
};

// The GDI+ start up structures.
//
// The field order and the field types have to match the SDK layout exactly,
// because GDI+ reads them directly. A layout that does not match makes
// GDI+ reject every later call with InvalidParameter.
struct StartupInputValue {
    UINT32 gdiplusVersion = 1;
    void* debugEventCallback = nullptr;
    BOOL suppressBackgroundThread = FALSE;
    BOOL suppressExternalCodecs = FALSE;
};

struct StartupOutputValue {
    void* notificationHook = nullptr;
    void* notificationUnhook = nullptr;
};

// The entry points that the renderer uses. Every GDI+ handle is an opaque
// pointer, and the functions that create an object receive a pointer to it.
struct Api {
    int(__stdcall* createBitmapFromStream)(IStream*, void**);
    int(__stdcall* createBitmapFromScan0)(int, int, int, int, unsigned char*, void**);
    int(__stdcall* createHBITMAPFromBitmap)(void*, HBITMAP*, unsigned long);
    int(__stdcall* createFromHDC)(HDC, void**);
    int(__stdcall* deleteGraphics)(void*);
    int(__stdcall* deleteFont)(void*);
    int(__stdcall* deleteFontFamily)(void*);
    int(__stdcall* deleteStringFormat)(void*);
    int(__stdcall* deleteBrush)(void*);
    int(__stdcall* disposeImage)(void*);
    int(__stdcall* getImageWidth)(void*, unsigned int*);
    int(__stdcall* getImageHeight)(void*, unsigned int*);
    int(__stdcall* getImageHorizontalResolution)(void*, float*);
    int(__stdcall* getImageVerticalResolution)(void*, float*);
    int(__stdcall* setInterpolationMode)(void*, int);
    int(__stdcall* drawImageRectI)(void*, void*, int, int, int, int);
    int(__stdcall* flush)(void*, int);
    int(__stdcall* createFontFamilyFromName)(const wchar_t*, void*, void**);
    int(__stdcall* createFont)(void*, float, int, int, void**);
    int(__stdcall* getFontHeight)(void*, void*, float*);
    int(__stdcall* createStringFormat)(int, unsigned short, void**);
    int(__stdcall* setStringFormatFlags)(void*, int);
    int(__stdcall* measureString)(void*, const wchar_t*, int, void*, const float*,
                                  const void*, float*, int*, int*);
    int(__stdcall* drawString)(void*, const wchar_t*, int, void*, const float*,
                               const void*, void*);
    int(__stdcall* createSolidFill)(unsigned long, void**);
    int(__stdcall* setTextRenderingHint)(void*, int);
    int(__stdcall* setSmoothingMode)(void*, int);

    int(__stdcall* startup)(ULONG_PTR*, const StartupInputValue*, StartupOutputValue*);
    void(__stdcall* shutdown)(ULONG_PTR);

    bool complete = false;
};

// Loads Gdiplus.dll and resolves the entry points. Returns false when the
// library is unusable; the reason is written to errorText.
bool EnsureApiLoaded(std::wstring* errorText);

// Returns the resolved entry points. The library is loaded on first use.
const Api& GetApi();

}  // namespace GdiPlusFlat
