#pragma once

#include "common.h"

#include <functional>

// Server-side rendering support.
//
// The RAW 9100 path forwards bytes verbatim, so it only works for printers
// whose firmware natively understands the submitted page description
// language (typically PCL or PostScript). Inkjets that only ship a GDI
// driver, label printers that speak ZPL/TSPL and receipt printers that
// speak ESC/POS all fail on that path.
//
// The render path closes that gap: the server owns the Windows print
// driver, so it can rasterize content itself and hand the result to any
// installed queue. That covers every printer Windows can drive, including
// inkjets, label printers and thermal receipt printers.
//
// Content is drawn with GDI+ onto a printer device context created through
// the "WINSPDC" driver, which means the queue's own driver performs the
// final conversion to the printer's native language. The server never has
// to know that language.

enum class RenderJobKind {
    Unknown,
    Text,
    Image,
    Pdf,
    Xps,
};

// Per-job render settings. Paper and orientation map onto DEVMODE fields so
// the driver itself decides how to lay the page out.
struct RenderJobOptions {
    int copies = 1;
    std::wstring paperSize;    // empty = keep the queue default
    std::wstring orientation;  // "portrait" / "landscape" / empty
    bool fitToPage = true;
    double scalePercent = 100.0;
    double marginInches = 0.0;
    bool monochrome = false;
    double fontSizePoints = 11.0;
    std::wstring fontFace;  // empty = built-in default
};

// A page of finished content handed to the renderer.
struct RenderPage {
    // Either raw bytes of an image file (PNG/JPEG/BMP/GIF/TIFF/EMF/WMF) ...
    std::vector<unsigned char> imageBytes;
    // ... or a text run.
    std::wstring text;
    bool isImage = false;

    // A ready made bitmap, used for rasterised PDF pages. The pixels are
    // stored top-down in 32bpp BGRA order.
    std::vector<unsigned char> rawPixels;
    int pixelWidth = 0;
    int pixelHeight = 0;
    bool hasRawPixels = false;
};

struct RenderResult {
    bool ok = false;
    std::wstring errorText;
    int pageCount = 0;
    std::wstring description;
};

// Detects what kind of payload arrived so the caller can pick a path.
RenderJobKind DetectRenderJobKind(const std::vector<unsigned char>& data);

// Splits a raw payload into renderable pages. Text is split on form feeds,
// images produce a single page.
bool SplitRenderPages(const std::vector<unsigned char>& data,
                      RenderJobKind kind,
                      std::vector<RenderPage>* pages,
                      std::wstring* errorText);

// Renders pages onto a Windows printer queue using that queue's own driver.
// Returns the number of pages that were submitted.
// Step-level trace hook used to localise crashes inside the render path.
// The server installs its log callback; every marker appears in the main log.
using RenderTraceSink = std::function<void(const std::wstring&)>;
void SetRenderTraceSink(RenderTraceSink sink);

RenderResult RenderPagesToPrinter(const std::vector<RenderPage>& pages,
                                   const std::wstring& printerName,
                                   const RenderJobOptions& options);

// Reports the queue's current paper size and orientation, plus the paper
// sizes the driver advertises, so callers can offer a sensible choice.
bool QueryPrinterPaperSizes(const std::wstring& printerName,
                            std::vector<std::wstring>* paperSizes,
                            std::wstring* currentPaper,
                            std::wstring* currentOrientation);

// Builds the paper size / orientation / copies fields for a DEVMODE block.
// The DEVMODE pointer is passed untyped so callers do not need the winspool
// headers in their own translation units.
bool ApplyRenderOptionsToDevMode(void* devMode,
                                  const RenderJobOptions& options,
                                  std::wstring* errorText);
