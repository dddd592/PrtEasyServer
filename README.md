# PrtEasyServer

Native C++ Windows RAW 9100 Network Print Server and TCP/IP Printer Server for local USB printers and Windows-installed printers.

[![Releases](https://img.shields.io/badge/Releases-v2.1.0.0-blue)](https://github.com/dddd592/PrtEasyServer/releases) | [Upstream](https://github.com/Terence0816/Windows-PrtEasyServer) | [MIT License](./LICENSE)

English | [繁體中文](#zh-tw) | [简体中文](#zh-cn)

PrtEasyServer is a native C++ rewrite of the original Python-packaged application. It turns a Windows PC into a lightweight RAW 9100 / TCP-IP printer server, so client PCs can install shared printers through a standard IP port without SMB printer sharing, Windows network neighborhood prompts, or account/password dialogs.

This repository (`dddd592/PrtEasyServer`) is an **enhanced fork** of [Terence0816/Windows-PrtEasyServer](https://github.com/Terence0816/Windows-PrtEasyServer) — see [About this fork](#about-this-fork) for what was added on top of upstream.

## <a id="about-this-fork"></a>About this fork

Based on upstream v2.1.0.0, this build adds:

- **AUTO print mode** — per-queue automatic RAW / XPS / RENDER selection backed by driver probing and payload sniffing (see below).
- **Android-friendly mDNS** — dual `_pdl-datastream._tcp` + `_printer._tcp` advertisements, RFC 6763 compliant FQDN names, multicast interface pinning, and an independent on/off switch in the settings page.
- **Hardened LPD listener** — real success/failure acknowledgements, payload limits, receive timeouts, an automatically created firewall rule, and a fix for the UI freeze when stopping the service.
- **Stability hardening** — structured-exception (SEH) guards around every client handler keep the server alive on driver faults; full driver DEVMODE buffers are preserved (fixes an access violation in `CreateDCW`); PDFium rendering is serialized with a mutex; 256 MiB payload/page budgets and 30 second receive timeouts on all listeners.
- **Self-healing installer BAT** — re-running the generated installer rebinds an existing queue/port to the current address and port numbers instead of skipping.
- **Simplified Chinese (`zh-cn`) as a built-in UI language**, persistent UTF-8 log file with 5 MiB rotation, and XPS/OpenXPS package submission through the Windows XPS Print API (from the earlier custom build line).

This is a source build, not a prebuilt EXE — see [BUILDING-WINDOWS10.md](./BUILDING-WINDOWS10.md) for the Windows 10 x64 build path.

## v2.1.0 — printing beyond PCL/PostScript printers

The RAW 9100 path forwards bytes verbatim, so it only works for printers whose
firmware natively understands the submitted page description language. Inkjets
that ship a GDI driver, label printers that speak ZPL/TSPL and receipt printers
that speak ESC/POS all fail on that path, and macOS / iOS cannot take part at
all because they cannot produce PCL.

This release adds four things that lift those limits. All of them keep the
existing "one port per printer" layout, so a setup that works today keeps
working.

### 1. RENDER mode — server side rendering through the printer driver

A new per-printer print mode. The server opens the target queue as a GDI
device context through the `WINSPDC` driver, draws the content itself, and the
queue's own driver performs the final conversion into whatever language the
printer speaks. That covers every printer Windows can drive, including
inkjets, label printers and thermal receipt printers.

* Text payloads are wrapped and paginated automatically. CJK text breaks
  between characters, so no dictionary is needed.
* Image payloads (PNG, JPEG, BMP, GIF, TIFF, EMF, WMF) are placed at their
  true physical size, derived from the image resolution and the printer's
  native DPI. This avoids the shrunken output that thermal label printers
  otherwise produce.
* `form feed` characters in a text payload start a new page.
* Paper size, orientation and copies are applied through DEVMODE, so the
  driver decides the layout.
* **PDF documents are rasterised** through PDFium, Chromium's PDF engine.
  See the section below.

### 2. AUTO mode — automatic RAW / XPS / RENDER selection

Each queue can be set to `AUTO` in the mode selector instead of a fixed mode.
On startup the server probes every queue's installed driver
(`GetPrinterDriver`, `GetPrinter`, `DeviceCapabilities`) and caches the
profile; at print time the payload itself decides the path:

* Native PDL payloads (PCL, PJL, PostScript, ZPL/TSPL, ESC/POS magic bytes)
  are always forwarded untouched through RAW, even when a GDI driver is
  present — content wins over driver labels.
* XPS packages go through the XPS Print API when the driver reports XPS
  compliance, and fall back to RENDER otherwise.
* Everything else (PDF, images, text, unknown bytes) goes through RENDER, so
  the queue's own driver produces whatever language the printer speaks.

The resolved mode is written to the log for every job (e.g. `AUTO->render`),
so it is always visible which path was taken.

### 3. HTTP print API — no driver on the client

```
POST http://<server>:80/api/print?printer=<name>&paper=A4&orientation=portrait
Content-Type: application/octet-stream

<document bytes>
```

The payload type is detected from the bytes, so an image, a text file or a PDF
can all be sent directly. Phones, macOS clients, ERP systems and shell
scripts can print without installing a printer driver.

Query parameters: `printer` (required), `copies`, `paper`, `orientation`,
`fit`, `scale`, `monochrome`, `fontsize`, `font`.

`GET /api/printers` returns the configured printer names as JSON.

### 4. mDNS discovery — the printers find themselves

Each configured printer is advertised over multicast DNS with its own
instance name and port:

* `_pdl-datastream._tcp` pointing at the RAW port (classic PDL data stream).
* `_printer._tcp` pointing at the LPD port when the LPD listener is enabled —
  this is the service type that Android print apps such as PrinterShare and
  the Mopria print service browse for.

Instance and host names are published as RFC 6763 compliant FQDNs
(`<printer> @ <host>._pdl-datastream._tcp.local`, `<host>.local`), which the
Android NSD / Mopria discovery stack requires; more lenient resolvers on
Windows and macOS accept them too. The multicast sender is pinned to the
primary LAN interface, so VPN virtual adapters cannot swallow the
announcements. Broadcasting can be switched off independently in the
settings page.

If UDP port 5353 is already taken by another responder the feature turns
itself off and the reason is written to the log; the printers stay reachable
by IP.

### 5. LPD listener (RFC 1179) — legacy clients

Unix hosts, embedded devices and older appliances usually speak LPD rather
than RAW. Enable the listener in the settings page and the queue name in an
LPD request selects the destination printer. The supported commands are
`0x01` (print with a control file), `0x02` (print data only), `0x03` (queue
status), `0x05` (remove jobs) and `0x06` (list queues).

Unix hosts, embedded devices and older appliances usually speak LPD rather
than RAW. Enable the listener in the settings page and the queue name in an
LPD request selects the destination printer. The supported commands are
`0x01` (print with a control file), `0x02` (print data only), `0x03` (queue
status), `0x05` (remove jobs) and `0x06` (list queues).

Jobs are only acknowledged (`0x00`) after the payload has actually been
handed to the destination queue — a failed handover returns an error code
instead of a silent fake success. A matching Windows firewall rule is created
automatically when the listener starts.

### 6. PDF support through PDFium

Windows has no built-in PDF renderer, so a PDF used to be forwarded to the
spooler unchanged, which only worked on queues whose driver accepts PDF
directly. PDFium, the PDF engine of Chromium, is now loaded at run time and
rasterises the document before it reaches the printer.

PDFium is distributed under the MIT licence, which is compatible with this
project. It is an **optional** dependency: the executable does not link
against it and keeps working without it.

```
# Download and install PDFium into third_party/pdfium
bash tools/fetch_pdfium.sh

# Copy it next to the executable, or keep the third_party/pdfium folder
copy third_party\pdfium\pdfium.dll .
```

The library is looked for in this order:

1. next to the executable
2. `<exe folder>/third_party/pdfium.dll`
3. `<exe folder>/third_party/pdfium/pdfium.dll`

Every page of the document is rasterised at 300 dpi, which matches the
resolution a laser printer usually applies. A multi page PDF becomes a multi
page print job.

When the DLL is missing the application still starts and every other feature
keeps working. A PDF job is then forwarded to the spooler unchanged, and the
reason is written to the log.

`bash tools/test_pdfium.sh` verifies the loader and the render path without
needing a printer.

#### Why not MuPDF

MuPDF is a capable alternative, but it is licensed under the AGPL, which
requires the whole application to be released under the AGPL together with
its complete source code. PDFium avoids that entirely.

## Version History

### v2.1.0.0 (this fork)

Beyond upstream v2.1.0.0:

- Added the `AUTO` print mode: per-queue driver probing on startup plus
  payload sniffing at print time, choosing RAW for native PDL bytes, the XPS
  Print API for compliant drivers, and RENDER for everything else.
- mDNS: added a second `_printer._tcp` advertisement (LPD) so Android print
  apps can discover the queues, published RFC 6763 compliant FQDN names,
  pinned the multicast sender to the primary LAN interface, and added an
  independent on/off switch in the settings page.
- LPD: real success/failure acknowledgements, payload size limits, receive
  timeouts, an automatic firewall rule, and a fix for the shutdown deadlock
  that froze the UI when stopping the service.
- Stability: SEH structured-exception guards around all client handlers (the
  server survives driver faults instead of disappearing), full DEVMODE
  driver buffers passed to `CreateDCW` (fixes an access violation), a mutex
  around PDFium rendering, and 256 MiB / 30 second resource limits on every
  listener and the HTTP API.
- The generated installer BAT is self-healing: re-running it rebinds an
  existing queue and port to the current address and port numbers.

### v2.1.0.0 (upstream)

- Added the `RENDER` print mode. The server renders the payload through the
  queue's own printer driver, which extends support to inkjets, label
  printers and thermal receipt printers that cannot consume PCL or
  PostScript.
- PDF documents are rasterised through PDFium, which is loaded at run time.
  The dependency is optional and does not change the licence of the project.
- Added the HTTP print API at `POST /api/print` plus `GET /api/printers`.
  Clients no longer need a printer driver of their own.
- Added mDNS advertisement so printers are discovered automatically.
- Added an optional LPD listener (RFC 1179) for legacy clients.
- Existing configurations keep working: `config.json` files without the new
  properties default to RAW mode with the LPD listener switched off.

### v2.0.0.0

- Major architecture update: the main application was rewritten from the original Python packaged version into a native C++ version.
- Faster startup and improved responsiveness.
- Main executable size reduced dramatically, from about 25 MB in the earliest version to about 728 KB.
- Removed the Python Runtime dependency for a cleaner and lighter runtime environment.
- Improved Windows 7 compatibility compared with the previous Python packaged version.
- Continued support for Windows 7 / 10 / 11.
- Added support for custom `.txt` language files.
- Built-in `tw` and `en` language templates are generated automatically, and users can add files such as `jp.txt` to switch the UI to additional languages.

### v1.2.0.0

- Added Windows 7 server-side support for printer hosting and driver package export.
- Added Windows 7 client-side support for installer BAT execution and automatic driver package installation.
- Reduced packaged file size significantly in the final Python-packaged generation.

### v1.1.1.0

- Replaced Base64 PowerShell launch flow with a readable UTF-8 embedded installer script.

### v1.1.0.0

- Added the built-in web page for installer BAT and driver package download.

### v1.0.0.0

- Initial public release of PrtEasyServer.

## Highlights

- Native C++ x64 main application
- RAW 9100 printer sharing over TCP/IP
- Per-printer print modes: RAW / XPS / RENDER / AUTO
- PDF, image and text rendering through the queue's own driver (PDFium + GDI+)
- HTTP print API (`POST /api/print`) for driverless clients
- mDNS (Bonjour) discovery with `_pdl-datastream._tcp` + `_printer._tcp`
- Optional LPD listener (RFC 1179) for legacy clients
- Built-in setup web page
- Automatic installer BAT generation (self-healing on re-run)
- Driver package download and installation flow
- Windows 7 / 10 / 11 support
- No Python Runtime dependency in the main application
- Custom language file support through external `.txt` files

## Repository Layout

- `src/`: main C++ Win32 source code
- `assets/printer.ico`: application icon used by the native resources
- `assets/screenshots/`: README screenshots
- `third_party/pdfium/`: runtime PDFium DLL and licence texts
- `tools/`: helper scripts and standalone test programs
- `build-windows10.cmd` + `BUILDING-WINDOWS10.md`: Windows 10 x64 build path

## English Screenshots

### English Main UI

![English Main UI](./assets/screenshots/ui-en-main-v2.png)

### English Web Page

![English Web Page](./assets/screenshots/web-en-v2.png)

### English Settings

![English Settings](./assets/screenshots/settings-en-v2.png)

## Custom Language Example: Japanese via `jp.txt`

The screenshots below show a Japanese UI created by manually adding a custom `jp.txt` language file. This means you can create your own `.txt` language file and use it to switch PrtEasyServer into another language without rebuilding the main program.

### Japanese Main UI via `jp.txt`

![Japanese Main UI via jp.txt](./assets/screenshots/ui-ja-custom-jp-txt-v2.png)

### Japanese Settings via `jp.txt`

![Japanese Settings via jp.txt](./assets/screenshots/settings-ja-custom-jp-txt-v2.png)

### Japanese Web Page via `jp.txt`

![Japanese Web Page via jp.txt](./assets/screenshots/web-ja-custom-jp-txt-v2.png)

## Download

- This fork: build from source with `build-windows10.cmd` (see [BUILDING-WINDOWS10.md](./BUILDING-WINDOWS10.md)).
- Upstream releases: [Terence0816/Windows-PrtEasyServer Releases](https://github.com/Terence0816/Windows-PrtEasyServer/releases)

### Deploying to another PC

The build statically links the C runtime (`/MT`), so `PrtEasyServer-Custom.exe`
runs on any Windows 7+ x64 machine without installing the VC++ Redistributable.
To keep PDF printing working, copy `pdfium.dll` next to the executable:

```
PrtEasyServer\
├── PrtEasyServer-Custom.exe
└── pdfium.dll
```

`pdfium.dll` is an optional dependency: without it the server starts normally
and RAW/PCL/PostScript printing keeps working, only PDF rendering is disabled
(the reason is written to the log).

## Search Keywords

Windows RAW 9100 print server, native C++ print server, TCP/IP printer server, Windows network print server, local USB printer sharing, IP printer setup, no SMB printer sharing, AUTO print mode, PDF printing PDFium, Bonjour mDNS printer discovery, LPD RFC 1179, HTTP print API, custom language txt, Win32 print server

## Credits

Based on PrinterOne by xtieume.  
Original project: https://github.com/xtieume/PrinterOne  
Original Copyright (c) 2025 xtieume@gmail.com

## License

This repository is released under the MIT License. See [LICENSE](./LICENSE).

---

<a id="zh-tw"></a>

# 繁體中文

PrtEasyServer 現在已改為原生 C++ 版本，是一個 Windows RAW 9100 網路印表機伺服器，可把本機 USB 印表機或已安裝在 Windows 上的印表機轉成 TCP/IP 網路印表機。使用者可透過標準 IP 連接埠安裝印表機，不需要使用 Windows 網芳或 SMB 印表機分享，也不需要碰到帳號密碼提示。

## 關於本儲存庫（強化分支）

本儲存庫是 [Terence0816/Windows-PrtEasyServer](https://github.com/Terence0816/Windows-PrtEasyServer)（v2.1.0.0）的強化分支，在上游基礎上新增：

- **AUTO 自動列印模式**：啟動時探測每個佇列的驅動程式，列印時依內容嗅探，自動在 RAW / XPS / RENDER 之間選擇（原生 PDL 位元流一律 RAW，其餘走 RENDER 由驅動程式轉換）。
- **安卓友善的 mDNS**：同時公告 `_pdl-datastream._tcp` 與 `_printer._tcp` 兩種服務，名稱符合 RFC 6763 規範（Android NSD / Mopria 必需），組播鎖定實體網卡避免被 VPN 虛擬網卡吞掉，並可在設定頁獨立開關。
- **強化的 LPD 監聽**：真實的成功/失敗回執、負載上限、接收逾時、自動建立防火牆規則，並修復停止服務時介面凍結的問題。
- **穩定性強化**：所有用戶端處理常式加上 SEH 結構化異常防護（驅動程式故障時伺服器不再消失）、保留完整 DEVMODE 驅動緩衝區（修復 `CreateDCW` 存取違規）、PDFium 渲染互斥鎖、256 MiB / 30 秒資源上限。
- **自癒式安裝 BAT**：重複執行安裝腳本會自動把既有佇列與連接埠重新綁定到目前的位址與埠號。
- **內建簡體中文介面**、UTF-8 記錄檔輪轉、XPS/OpenXPS 封包透過 Windows XPS 列印 API 提交。

## 版本更新紀錄

### v2.1.0.0（本分支）

在上游 v2.1.0.0 基礎上新增：

- `AUTO` 自動列印模式：啟動時驅動探測 + 列印時內容嗅探，原生 PDL 走 RAW、合規驅動走 XPS Print API、其餘走 RENDER。
- mDNS：新增 `_printer._tcp`（LPD）第二服務公告、RFC 6763 合規 FQDN 名稱、組播介面鎖定、設定頁獨立開關。
- LPD：真實回執、負載上限、接收逾時、自動防火牆規則、修復停止服務死鎖。
- 穩定性：SEH 異常防護、完整 DEVMODE 緩衝區、PDFium 渲染互斥鎖、各監聽器 256 MiB / 30 秒資源上限。
- 自癒式安裝 BAT：重複執行自動重綁佇列與連接埠。

### v2.1.0.0（上游）

- 新增 `RENDER` 列印模式，伺服器端透過佇列自身的驅動程式渲染，支援噴墨、標籤與熱感應收據印表機。
- PDF 文件透過 PDFium 於執行時載入並點陣化，該依賴為可選項。
- 新增 HTTP 列印 API（`POST /api/print` 與 `GET /api/printers`）。
- 新增 mDNS 自動探索公告。
- 新增可選的 LPD 監聽器（RFC 1179）。

### v2.0.0.0

- 本版本為重大架構更新，主程式已從原本的 Python 打包版本重寫為 C++ 原生版本。
- 主程式改以 C++ 重新編譯，啟動速度與操作反應更快。
- 主程式檔案大小大幅降低，從最初版本約 25MB 降至約 728KB。
- 移除對 Python Runtime 的依賴，執行環境更乾淨。
- 提升 Windows 7 相容性，較原本 Python 打包版本更不容易受到缺少元件或 Runtime 影響。
- 維持 Windows 7 / 10 / 11 支援。
- 支援自定義 `.txt` 語言檔。
- 內建 `tw` 與 `en` 語言模板會自動產生，使用者也可自行新增 `jp.txt` 等外部語言檔來切換介面語言。

### v1.2.0.0

- 支援 WIN7 伺服器端架設與打包印表機驅動程式。
- 支援 WIN7 連接端自動執行設定檔與驅動包安裝。
- Python 打包版本末期已大幅減少主程式檔案體積。

### v1.1.1.0

- 安裝 BAT 改為可讀的 UTF-8 明文嵌入式腳本流程。

### v1.1.0.0

- 新增內建網頁，可下載安裝 BAT 與驅動程式。

### v1.0.0.0

- PrtEasyServer 首次公開版本。

## 功能特色

- 原生 C++ x64 主程式
- RAW 9100 TCP/IP 印表機分享
- 每台印表機獨立列印模式：RAW / XPS / RENDER / AUTO
- PDF、圖片與文字經佇列自身驅動程式渲染（PDFium + GDI+）
- HTTP 列印 API（`POST /api/print`），用戶端免驅動程式
- mDNS (Bonjour) 自動探索，含 `_pdl-datastream._tcp` 與 `_printer._tcp`
- 可選 LPD 監聽器（RFC 1179）
- 內建安裝設定網頁
- 自動產生安裝 BAT（重複執行自動重綁）
- 驅動程式下載與安裝流程
- 支援 Windows 7 / 10 / 11
- 主程式不再依賴 Python Runtime
- 支援外部 `.txt` 自定義語言檔

## 儲存庫結構

- `src/`：主要 C++ Win32 原始碼
- `assets/printer.ico`：原生資源檔使用的程式圖示
- `assets/screenshots/`：README 畫面截圖
- `third_party/pdfium/`：執行時載入的 PDFium DLL 與授權文件
- `tools/`：輔助腳本與獨立測試程式
- `build-windows10.cmd` + `BUILDING-WINDOWS10.md`：Windows 10 x64 編譯路徑

## 中文畫面

### 中文主介面

![中文主介面](./assets/screenshots/ui-zh-tw-main-v2.png)

### 中文網頁

![中文網頁](./assets/screenshots/web-zh-tw-v2.png)

### 繁體設定頁

![繁體設定頁](./assets/screenshots/settings-zh-tw-v2.png)

## 自定義語言範例：手動新增 `jp.txt`

以下畫面示範的是手動新增 `jp.txt` 後切換出的日文介面。也就是說，你可以自行建立其他 `.txt` 語言檔，配合 PrtEasyServer 做額外語言切換，不需要重新編譯主程式。

### 日文主介面（手動建立 `jp.txt`）

![日文主介面（手動建立 jp.txt）](./assets/screenshots/ui-ja-custom-jp-txt-v2.png)

### 日文設定頁（手動建立 `jp.txt`）

![日文設定頁（手動建立 jp.txt）](./assets/screenshots/settings-ja-custom-jp-txt-v2.png)

### 日文網頁（手動建立 `jp.txt`）

![日文網頁（手動建立 jp.txt）](./assets/screenshots/web-ja-custom-jp-txt-v2.png)

## 下載

- 本分支：以 `build-windows10.cmd` 自原始碼編譯（見 [BUILDING-WINDOWS10.md](./BUILDING-WINDOWS10.md)）。
- 上游發行版：[Terence0816/Windows-PrtEasyServer Releases](https://github.com/Terence0816/Windows-PrtEasyServer/releases)

### 部署到其他電腦

建置採用 C 執行階段靜態連結（`/MT`），`PrtEasyServer-Custom.exe` 可直接在
任何 Windows 7 以上 x64 電腦執行，無需安裝 VC++ Redistributable。若需保留
PDF 列印功能，請將 `pdfium.dll` 一併複製到主程式旁：

```
PrtEasyServer\
├── PrtEasyServer-Custom.exe
└── pdfium.dll
```

`pdfium.dll` 為可選依賴：缺少它時伺服器仍可正常啟動，RAW/PCL/PostScript
列印不受影響，只有 PDF 轉譯功能停用（原因會寫入記錄檔）。

## 搜尋關鍵字

Windows RAW 9100 印表機伺服器、C++ 印表機伺服器、TCP/IP 印表機伺服器、Windows 網路印表機伺服器、USB 印表機分享、IP 印表機安裝、AUTO 自動列印模式、PDF 列印 PDFium、Bonjour mDNS 印表機探索、LPD RFC 1179、HTTP 列印 API、自定義語言 txt、無 SMB 分享、Win32 印表機分享

## 原作與致謝

本專案基於 PrinterOne 修改而成。  
原始專案：https://github.com/xtieume/PrinterOne  
Original Copyright (c) 2025 xtieume@gmail.com

## 授權

本儲存庫目前使用 MIT License，詳見 [LICENSE](./LICENSE)。

---

<a id="zh-cn"></a>

# 简体中文

PrtEasyServer 现已改为原生 C++ 版本，是一个 Windows RAW 9100 网络打印机服务器，可把本机 USB 打印机或已安装在 Windows 上的打印机转成 TCP/IP 网络打印机。用户可通过标准 IP 端口安装打印机，不需要使用 Windows 网上邻居或 SMB 打印机共享，也不会碰到账号密码提示。

## 关于本仓库（强化分支）

本仓库是 [Terence0816/Windows-PrtEasyServer](https://github.com/Terence0816/Windows-PrtEasyServer)（v2.1.0.0）的强化分支，在上游基础上新增：

- **AUTO 自动打印模式**：启动时探测每个队列的驱动程序，打印时按内容嗅探，自动在 RAW / XPS / RENDER 之间选择（原生 PDL 字节流一律 RAW，其余走 RENDER 由驱动程序转换）。
- **安卓友好的 mDNS**：同时公告 `_pdl-datastream._tcp` 与 `_printer._tcp` 两种服务，名称符合 RFC 6763 规范（Android NSD / Mopria 必需），组播锁定物理网卡避免被 VPN 虚拟网卡吞掉，并可在设置页独立开关。
- **强化的 LPD 监听**：真实的成功/失败回执、负载上限、接收超时、自动创建防火墙规则，并修复停止服务时界面冻结的问题。
- **稳定性强化**：所有客户端处理例程加上 SEH 结构化异常防护（驱动程序故障时服务器不再消失）、保留完整 DEVMODE 驱动缓冲区（修复 `CreateDCW` 访问冲突）、PDFium 渲染互斥锁、256 MiB / 30 秒资源上限。
- **自愈式安装 BAT**：重复执行安装脚本会自动把既有队列与端口重新绑定到当前的地址与端口号。
- **内置简体中文界面**、UTF-8 日志文件轮转、XPS/OpenXPS 数据包通过 Windows XPS 打印 API 提交。

## 版本更新记录

### v2.1.0.0（本分支）

在上游 v2.1.0.0 基础上新增：

- `AUTO` 自动打印模式：启动时驱动探测 + 打印时内容嗅探，原生 PDL 走 RAW、合规驱动走 XPS Print API、其余走 RENDER。
- mDNS：新增 `_printer._tcp`（LPD）第二服务公告、RFC 6763 合规 FQDN 名称、组播接口锁定、设置页独立开关。
- LPD：真实回执、负载上限、接收超时、自动防火墙规则、修复停止服务死锁。
- 稳定性：SEH 异常防护、完整 DEVMODE 缓冲区、PDFium 渲染互斥锁、各监听器 256 MiB / 30 秒资源上限。
- 自愈式安装 BAT：重复执行自动重绑队列与端口。

### v2.1.0.0（上游）

- 新增 `RENDER` 打印模式，服务器端通过队列自身的驱动程序渲染，支持喷墨、标签与热敏收据打印机。
- PDF 文档通过 PDFium 于运行时加载并栅格化，该依赖为可选项。
- 新增 HTTP 打印 API（`POST /api/print` 与 `GET /api/printers`）。
- 新增 mDNS 自动发现公告。
- 新增可选的 LPD 监听器（RFC 1179）。

### v2.0.0.0

- 本版本为重大架构更新，主程序已从原本的 Python 打包版本重写为 C++ 原生版本。
- 主程序改以 C++ 重新编译，启动速度与操作反应更快。
- 主程序文件体积大幅降低，从最初版本约 25MB 降至约 728KB。
- 移除对 Python Runtime 的依赖，运行环境更干净。
- 提升 Windows 7 兼容性，较原本 Python 打包版本更不容易受到缺少组件或 Runtime 影响。
- 维持 Windows 7 / 10 / 11 支持。
- 支持自定义 `.txt` 语言文件。
- 内置 `tw` 与 `en` 语言模板会自动生成，用户也可自行新增 `jp.txt` 等外部语言文件来切换界面语言。

### v1.2.0.0

- 支持 WIN7 服务器端搭建与打包打印机驱动程序。
- 支持 WIN7 客户端自动执行配置文件与驱动包安装。
- Python 打包版本末期已大幅减少主程序文件体积。

### v1.1.1.0

- 安装 BAT 改为可读的 UTF-8 明文嵌入式脚本流程。

### v1.1.0.0

- 新增内置网页，可下载安装 BAT 与驱动程序。

### v1.0.0.0

- PrtEasyServer 首次公开版本。

## 功能特色

- 原生 C++ x64 主程序
- RAW 9100 TCP/IP 打印机共享
- 每台打印机独立打印模式：RAW / XPS / RENDER / AUTO
- PDF、图片与文字经队列自身驱动程序渲染（PDFium + GDI+）
- HTTP 打印 API（`POST /api/print`），客户端免驱动程序
- mDNS (Bonjour) 自动发现，含 `_pdl-datastream._tcp` 与 `_printer._tcp`
- 可选 LPD 监听器（RFC 1179）
- 内置安装设置网页
- 自动生成安装 BAT（重复执行自动重绑）
- 驱动程序下载与安装流程
- 支持 Windows 7 / 10 / 11
- 主程序不再依赖 Python Runtime
- 支持外部 `.txt` 自定义语言文件

## 仓库结构

- `src/`：主要 C++ Win32 源代码
- `assets/printer.ico`：原生资源文件使用的程序图标
- `assets/screenshots/`：README 界面截图
- `third_party/pdfium/`：运行时加载的 PDFium DLL 与授权文件
- `tools/`：辅助脚本与独立测试程序
- `build-windows10.cmd` + `BUILDING-WINDOWS10.md`：Windows 10 x64 编译路径

## 中文界面截图

### 中文主界面

![中文主界面](./assets/screenshots/ui-zh-tw-main-v2.png)

### 中文网页

![中文网页](./assets/screenshots/web-zh-tw-v2.png)

### 设置页

![设置页](./assets/screenshots/settings-zh-tw-v2.png)

## 自定义语言示例：手动新增 `jp.txt`

以下界面演示的是手动新增 `jp.txt` 后切换出的日文界面。也就是说，你可以自行创建其他 `.txt` 语言文件，配合 PrtEasyServer 做额外语言切换，不需要重新编译主程序。

### 日文主界面（手动创建 `jp.txt`）

![日文主界面（手动创建 jp.txt）](./assets/screenshots/ui-ja-custom-jp-txt-v2.png)

### 日文设置页（手动创建 `jp.txt`）

![日文设置页（手动创建 jp.txt）](./assets/screenshots/settings-ja-custom-jp-txt-v2.png)

### 日文网页（手动创建 `jp.txt`）

![日文网页（手动创建 jp.txt）](./assets/screenshots/web-ja-custom-jp-txt-v2.png)

## 下载

- 本分支：以 `build-windows10.cmd` 从源代码编译（见 [BUILDING-WINDOWS10.md](./BUILDING-WINDOWS10.md)）。
- 上游发行版：[Terence0816/Windows-PrtEasyServer Releases](https://github.com/Terence0816/Windows-PrtEasyServer/releases)

### 部署到其他电脑

构建采用 C 运行库静态链接（`/MT`），`PrtEasyServer-Custom.exe` 可直接在
任何 Windows 7 及以上 x64 电脑运行，无需安装 VC++ Redistributable。如需保留
PDF 打印功能，请将 `pdfium.dll` 一并复制到主程序同目录：

```
PrtEasyServer\
├── PrtEasyServer-Custom.exe
└── pdfium.dll
```

`pdfium.dll` 为可选依赖：缺少它时服务器仍可正常启动，RAW/PCL/PostScript
打印不受影响，仅 PDF 渲染功能停用（原因会写入日志）。

## 搜索关键词

Windows RAW 9100 打印机服务器、C++ 打印机服务器、TCP/IP 打印机服务器、Windows 网络打印机服务器、USB 打印机共享、IP 打印机安装、AUTO 自动打印模式、PDF 打印 PDFium、Bonjour mDNS 打印机发现、LPD RFC 1179、HTTP 打印 API、自定义语言 txt、无 SMB 共享、Win32 打印机共享

## 原作与致谢

本项目基于 PrinterOne 修改而成。  
原始项目：https://github.com/xtieume/PrinterOne  
Original Copyright (c) 2025 xtieume@gmail.com

## 授权

本仓库目前使用 MIT License，详见 [LICENSE](./LICENSE)。

