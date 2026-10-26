# Windows 10 build guide (custom branch)

## Prerequisites
- Windows 10 x64
- Visual Studio 2022 (or 2019) with **Desktop development with C++**
- Windows 10 SDK selected in Visual Studio Installer
- Printer driver installed on the server; XPS printing requires the target printer/driver to support the Windows XPS Print API path

## Build
1. Extract this source archive to a writable folder.
2. Open **x64 Native Tools Command Prompt for VS 2022** from the Start menu.
3. `cd` to the extracted folder.
4. Run `build-windows10.cmd`.
5. The output path is `build\PrtEasyServer-Custom.exe`. The application generates built-in `language\zh-cn.txt`, `tw.txt` and `en.txt` files beside the EXE on first run.

The script uses the MSVC x64 compiler, Windows SDK resource compiler and links `XpsPrint.lib`. This environment is not Windows, so this archive has not been compiled or runtime-tested here.

## Changes in this custom source
- Simplified Chinese (`zh-cn`) added as a built-in language; Chinese Simplified/Singapore locale defaults to it. Traditional Chinese (`tw`) and English (`en`) remain selectable.
- Persistent UTF-8 logs written to `logs\printer-server.log`; at about 5 MiB the previous file rotates to `.1`.
- A received byte stream is treated as XPS only if ZIP/XPS package markers are found; it is then submitted using `StartXpsPrintJob` / `IXpsPrintJobStream`. Other traffic stays on the existing RAW path.

## XPS caveats
- This is a first-pass integration, not a claim of compatibility with every XPSDrv vendor driver. The client must send a complete XPS/OpenXPS package as one TCP connection, and the server's installed queue must accept XPS Print API jobs.
- RAW 9100 clients often send printer-ready PCL/PS rather than an XPS package; those continue through the original RAW path.
- Test with a non-critical printer first. Windows spooler acceptance/completion does not prove paper physically emerged.


## Per-printer RAW / XPS mode selection

The printer editor includes a **Print mode** selector for each configured printer:

- **RAW** (default): forwards the received bytes through the existing Windows RAW spooler path. This preserves existing behavior.
- **XPS**: treats the received payload as a complete XPS/OpenXPS package and submits it using the Windows XPS Print API. Non-XPS payloads are rejected and logged rather than silently sent as RAW.

The selected mode is saved as `mode: "raw"` or `mode: "xps"` in `config.json`. Existing configurations without a `mode` property default to RAW. Select a printer in the list to edit its mode, then click **Update** and **Save**.

Important: choosing XPS mode does not convert PCL, PostScript, or arbitrary RAW bytes to XPS. The network client must send a complete XPS/OpenXPS document, and the selected Windows printer queue must support the XPS Print API. Test with a non-production queue first.

## v2.1.0 additions

The build now compiles four extra translation units and links against
`Gdiplus.lib`:

| File | Purpose |
|---|---|
| `src/render_engine.cpp` | Server side rendering through the queue's printer driver (RENDER mode) |
| `src/print_api.cpp` | HTTP print API request parsing |
| `src/mdns_broadcaster.cpp` | Multicast DNS advertisement of the queues |
| `src/lpd_server.cpp` | LPD / LPR listener (RFC 1179) |
| `src/pdf_renderer.cpp` | PDF rasterisation through PDFium |
| `src/gdi_plus_flat.cpp` | Runtime access to the GDI+ flat API |

`Gdiplus.lib` is part of the Windows SDK and needs no extra redistributable.

### Why the GDI+ flat API is resolved at run time

The GDI+ flat API is declared in `gdiplusflat.h`, but the SDK headers of the
supported toolchains expose it differently. MSVC does not pull the flat API
into `gdiplus.h` at all, other toolchains publish it inside a nested
namespace, and some exported names differ, for example the image release
function is `GdipDeleteBitmap` on MSVC and `GdipDisposeImage` elsewhere.

`src/gdi_plus_flat.cpp` therefore declares the entry points itself and
resolves them from `gdiplus.dll` with `GetProcAddress`. The renderer does
not depend on any SDK declaration of the flat API, so one implementation
works on every toolchain. The library is part of Windows and always present,
so this adds no deployment requirement.

The same approach is used for PDFium in `src/pdf_renderer.cpp`.

### Pixel format identifiers are bit fields, not an enumeration

The GDI+ pixel format values are **not** an enumeration, which makes them
easy to get wrong when the SDK headers are bypassed:

```c
/* gdipluspixelformats.h, MSVC */
#define PixelFormat32bppARGB  (10 | (32 << 8) | Alpha | GDI | Canonical)
#define PixelFormat1bppIndexed (1 | ( 1 << 8) | Indexed | GDI)
```

Evaluated, those are `0x0026200A` and `0x00030101`. Some other toolchains
publish a small enumeration for the same names with completely different
values (`PixelFormat32bppARGB == 5`, `PixelFormat1bppIndexed == 1`).

Passing the enumeration value to the real GDI+ is rejected with
`InvalidParameter`, so every rasterised page fails to render.
`src/gdi_plus_flat.h` therefore defines the bit field values that Windows
itself uses.

The same applies to the row order. A rasteriser hands over its rows top-down,
while GDI+ stores `PixelFormat32bppARGB` bottom-up. Passing a **negative**
stride tells GDI+ the buffer is top-down, which avoids copying the whole
page to flip it. With a negative stride the buffer pointer has to address
the first row of the image, which is the last row in storage order.

### Deprecation warnings

The XPS print API is marked deprecated by `xpsprint.h`, but it is the only
way to submit an XPS document on Windows 7, which this build still
supports.

The deprecation is declared like this in the SDK header:

```c
#pragma deprecated(StartXpsPrintJob,StartXpsPrintJob1,IXpsPrintJob,
                   IXpsPrintStream,XPS_JOB_STATUS,XPS_JOB_COMPLETION)
```

It is unconditional. There is no macro in the SDK that switches it off, so
`printer_server.cpp` suppresses the warning with a pragma around the single
function that uses the API:

```cpp
#pragma warning(push)
#pragma warning(disable : 4995)
bool PrintXpsDocument(...) { ... }
#pragma warning(pop)
```

The region deliberately covers only that function, so warnings elsewhere in
the file are still reported.

## PDFium, an optional runtime dependency

PDFium is loaded at run time with `LoadLibrary`, so it is **not** linked
against the executable and **not** required to build or to run the
application. It is only needed to rasterise PDF documents.

```
bash tools/fetch_pdfium.sh
```

The script downloads the prebuilt Windows x64 package from the
`bblanchon/pdfium-binaries` releases and installs it into
`third_party/pdfium/`:

```
third_party/pdfium/pdfium.dll    the library
third_party/pdfium/LICENSE        the licence, which has to travel with it
```

Copy `pdfium.dll` next to the executable, or keep the `third_party/pdfium`
folder inside the deployment folder. The loader searches these locations:

1. next to the executable
2. `<exe folder>/third_party/pdfium.dll`
3. `<exe folder>/third_party/pdfium/pdfium.dll`

### Licensing

PDFium is distributed under the MIT licence, which is compatible with the
licence of this project. The `LICENSE` file that comes with the download has
to be redistributed alongside the DLL.

MuPDF was considered as well, but it is licensed under the AGPL, which would
require this whole application to be released under the AGPL together with
its complete source code. PDFium avoids that requirement.

### Verifying the PDF path

```
bash tools/test_pdfium.sh
```

The script builds three checks with the MinGW toolchain and runs them:

1. the loader reports a clear error when the DLL is missing
2. the loader rasterises a page when the DLL is present
3. the application path turns a PDF into rasterised pages that contain
   actual rendered content

No printer is needed for these checks. The MSVC build remains the
authoritative one.

### Configuration format

`config.json` gained two optional properties and one new mode value:

```json
{
  "printers": [
    { "printer_name": "HP LaserJet MFP", "port": 9100, "mode": "render" }
  ],
  "web_port": 80,
  "language": "zh-cn",
  "minimize_to_tray": true,
  "enable_lpd": false,
  "lpd_port": 515
}
```

`mode` accepts `raw` (default), `xps` and `render`. A configuration file that
does not mention the new properties keeps its previous behaviour: every
printer stays in RAW mode and the LPD listener stays switched off.

### Testing the new paths

1. Start the server and confirm that a printer is configured in RENDER mode.
2. Run the smoke test against the server:

   ```
   bash tools/smoke_test.sh <server-ip> <web-port>
   ```

3. Confirm the paper output by hand. The smoke test only reports what the
   server answered, because a spooler accepting a job does not prove that
   the printer produced output.

Useful manual checks:

* Text job with automatic wrapping and pagination:
  `curl -X POST --data-binary @note.txt "http://<server>/api/print?printer=<name>"`
* Image job at true physical size:
  `curl -X POST --data-binary @label.png "http://<server>/api/print?printer=<name>&paper=102x152"`
  (the `paper` value `WIDTHxHEIGHT` is given in millimetres)
* Legacy client over LPD:
  `lp -h <server> -P <printer name> /path/to/file`

### Syntax check without Visual Studio

`tools/check_syntax.sh` compiles every translation unit with the MinGW
toolchain in syntax-only mode. It rewrites the two Microsoft extension
constructs that libstdc++ does not accept, in a scratch copy, so the shipped
sources are never modified. The script is a guard against syntax and type
errors; the MSVC build remains authoritative.

```
CXX=/path/to/g++ bash tools/check_syntax.sh
```

### Known limitations

* The render path rasterises images, text and PDF. PDF rasterisation needs
  the optional PDFium library; without it a PDF is forwarded to the spooler
  unchanged and only prints on queues whose driver accepts PDF directly.
* XPS payloads keep using the Windows XPS Print API, which is a better fit
  for vector documents than a raster round trip.
* mDNS advertisement uses `_pdl-datastream._tcp.local`. A full IPP and
  AirPrint implementation is not part of this release.
