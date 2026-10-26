@echo off
setlocal EnableExtensions
cd /d "%~dp0"

REM Build from an x64 Native Tools Command Prompt for Visual Studio 2019/2022.
where cl.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: cl.exe was not found.
  echo Open "x64 Native Tools Command Prompt for VS 2022" and run this script again.
  exit /b 2
)
where rc.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: rc.exe was not found. Install the Windows 10/11 SDK in Visual Studio Installer.
  exit /b 3
)

if not exist build mkdir build
if not exist build\obj mkdir build\obj
pushd src
rc.exe /nologo /fo ..\build\obj\resources.res resources.rc
if errorlevel 1 goto :failed_src
popd

cl.exe /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /MD /O2 /utf-8 ^
  src\main.cpp src\main_window.cpp src\printer_server.cpp src\localization.cpp src\config.cpp src\common.cpp src\render_engine.cpp src\print_api.cpp src\mdns_broadcaster.cpp src\lpd_server.cpp src\pdf_renderer.cpp src\gdi_plus_flat.cpp src\driver_probe.cpp build\obj\resources.res ^
  /Febuild\PrtEasyServer-Custom.exe ^
  /Fobuild\obj\ ^
  /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT Comctl32.lib Comdlg32.lib Advapi32.lib User32.lib Gdi32.lib Gdiplus.lib Shell32.lib Shlwapi.lib Winspool.lib Ws2_32.lib Iphlpapi.lib Ole32.lib XpsPrint.lib
if errorlevel 1 goto :failed

echo.
echo Build succeeded: build\PrtEasyServer-Custom.exe
echo The application creates its built-in language files and logs folder beside the EXE on first run.
echo.
echo PDF support is optional. To rasterise PDF documents, install PDFium:
echo   bash tools\fetch_pdfium.sh
echo and copy third_party\pdfium\pdfium.dll next to the EXE.
exit /b 0

:failed_src
set ERR=%ERRORLEVEL%
popd
echo Resource compilation failed with error %ERR%.
exit /b %ERR%

:failed
set ERR=%ERRORLEVEL%
echo Build failed with error %ERR%.
exit /b %ERR%
