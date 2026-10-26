# Third party components

## PDFium

PDFium is the PDF engine of Chromium. It is used to rasterise PDF documents
before they are sent to a printer.

* Project: https://pdfium.googlesource.com/pdfium/
* Prebuilt Windows binaries: https://github.com/bblanchon/pdfium-binaries
* Licence: MIT (see `third_party/pdfium/LICENSE` after running
  `tools/fetch_pdfium.sh`)

PDFium is an **optional** dependency. It is loaded at run time with
`LoadLibrary`, so the executable does not link against it and the
application runs without it. The library is only needed when a PDF document
is printed in RENDER mode.

When the library is redistributed, its `LICENSE` file has to be
redistributed with it.

## MuPDF

MuPDF was evaluated as an alternative PDF engine but was not adopted. It is
licensed under the GNU Affero General Public License, which would require
this whole application to be released under the AGPL together with its
complete source code. PDFium avoids that requirement.
