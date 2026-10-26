#pragma once

#include "common.h"

// PDF rasterisation through PDFium.
//
// Windows has no built-in PDF renderer, so the render path could not
// produce paper from a PDF document. PDFium is Chromium's PDF engine and is
// distributed under the MIT licence, which keeps it compatible with this
// project's licence.
//
// The library is loaded at run time instead of being linked. That keeps the
// executable free of any PDF dependency: a deployment that never prints a
// PDF does not need pdfium.dll at all, and a deployment that does need one
// only has to drop the DLL next to the EXE.
//
// When the DLL is missing the renderer reports the failure and the caller
// falls back to forwarding the document to the spooler unchanged.

// Describes one rasterised PDF page.
struct PdfPageBitmap {
    // The page rendered as a top-down 32bpp BGRA buffer.
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
};

// Reports whether a usable PDFium library was found.
bool IsPdfRendererAvailable();

// Returns a human readable reason when the renderer is unavailable.
std::wstring GetPdfRendererStatus();

// Rasterises every page of a PDF document.
//
// The DPI decides the resolution of the resulting bitmaps, so a page is
// rendered at the same pixel size it would have at that print resolution.
// Returns false and fills errorText when the document cannot be handled.
bool RenderPdfPages(const std::vector<unsigned char>& pdfBytes,
                    int dpi,
                    std::vector<PdfPageBitmap>* pages,
                    std::wstring* errorText);
