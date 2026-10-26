#!/usr/bin/env bash
# Builds and runs the PDFium related checks.
#
# The checks verify the part of the PDF path that can be tested without a
# printer:
#
#   1. the dynamic loader resolves the PDFium API and can rasterise a page
#   2. the application path turns a PDF into rasterised pages that contain
#      actual rendered content
#   3. the behaviour without the DLL is a clear, non fatal failure
#
# Usage:
#   bash tools/test_pdfium.sh
#
# The script uses the MinGW toolchain when it is available. The MSVC build
# remains authoritative; this is an extra safety net.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-/c/tools/mingw64/bin/g++.exe}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$CXX" ]; then
  echo "A C++ compiler was not found at $CXX." >&2
  echo "Set CXX to a working compiler, or run the checks from Visual Studio." >&2
  exit 2
fi

DLL_SOURCE="$ROOT/third_party/pdfium/pdfium.dll"
if [ ! -f "$DLL_SOURCE" ]; then
  echo "PDFium is not installed. Run tools/fetch_pdfium.sh first." >&2
  exit 3
fi

echo "Building the checks..."
mkdir -p "$WORK/src"
cp "$ROOT"/src/*.cpp "$ROOT"/src/*.h "$WORK/src/"
cp "$ROOT/tools/pdf_renderer_test.cpp" "$WORK/"
cp "$ROOT/tools/pdf_render_path_test.cpp" "$WORK/"
cp "$ROOT/tools/gdi_plus_flat_test.cpp" "$WORK/"

# The application sources use a Microsoft extension for wide string file
# streams. The scratch copy is rewritten so that the check can be built with
# any toolchain. The shipped sources are never modified.
python3 - "$WORK/src" <<'PY'
import os, re, sys

root = sys.argv[1]

def wrap_first_arg(source, pattern):
    out = []
    index = 0
    compiled = re.compile(pattern)
    while True:
        match = compiled.search(source, index)
        if match is None:
            out.append(source[index:])
            break
        out.append(source[index:match.end()])
        depth = 1
        cursor = match.end()
        while cursor < len(source) and depth > 0:
            char = source[cursor]
            if char == '(':
                depth += 1
            elif char == ')':
                depth -= 1
                if depth == 0:
                    break
            cursor += 1
        arguments = source[match.end():cursor]
        argument_depth = 0
        split_at = -1
        for position, char in enumerate(arguments):
            if char in '([{':
                argument_depth += 1
            elif char in ')]}':
                argument_depth -= 1
            elif char == ',' and argument_depth == 0:
                split_at = position
                break
        if split_at < 0:
            first, remainder = arguments, ''
        else:
            first, remainder = arguments[:split_at], arguments[split_at:]
        out.append('Narrow(')
        out.append(first)
        out.append(')')
        out.append(remainder)
        index = cursor
    return ''.join(out)

for name in os.listdir(root):
    if not name.endswith(('.cpp', '.h')):
        continue
    path = os.path.join(root, name)
    text = open(path, encoding='utf-8').read()
    original = text
    for pattern in (r'std::ifstream\s+\w+\(', r'std::ofstream\s+\w+\(',
                    r'\w+\.open\(', r'\w+->open\('):
        text = wrap_first_arg(text, pattern)
    if text != original:
        open(path, 'w', encoding='utf-8').write(text)

helper = ('#pragma once\n#include <string>\n#include <windows.h>\n'
          'inline std::string Narrow(const std::wstring& path){\n'
          '    if (path.empty()) return std::string();\n'
          '    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);\n'
          '    if (needed <= 0) return std::string();\n'
          '    std::string buffer(static_cast<std::size_t>(needed), \'\\0\');\n'
          '    ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, buffer.data(), needed, nullptr, nullptr);\n'
          '    if (!buffer.empty() && buffer.back() == \'\\0\') buffer.pop_back();\n'
          '    return buffer;\n'
          '}\n')
open(os.path.join(root, 'wide_narrow_helper.h'), 'w', encoding='utf-8').write(helper)

for name in os.listdir(root):
    if name.endswith('.cpp'):
        path = os.path.join(root, name)
        text = open(path, encoding='utf-8').read()
        open(path, 'w', encoding='utf-8').write('#include "wide_narrow_helper.h"\n' + text)
PY

FLAGS="-std=c++17 -O1 -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -I$WORK/src"
LIBS="-lgdiplus -lole32 -lwinspool -lgdi32 -lcomctl32 -lshell32 -lshlwapi -ladvapi32 -lws2_32 -liphlpapi"

if ! "$CXX" $FLAGS -o "$WORK/pdf_renderer_test.exe" \
        "$WORK/pdf_renderer_test.cpp" "$WORK/src/pdf_renderer.cpp" 2>&1 | grep -E "error:" ; then
  :
fi
if [ ! -f "$WORK/pdf_renderer_test.exe" ]; then
  echo "FAIL  the loader test did not build" >&2
  exit 4
fi

"$CXX" $FLAGS -o "$WORK/pdf_render_path_test.exe" \
    "$WORK/pdf_render_path_test.cpp" "$WORK/src/render_engine.cpp" \
    "$WORK/src/pdf_renderer.cpp" "$WORK/src/gdi_plus_flat.cpp" \
    "$WORK/src/common.cpp" $LIBS 2>&1 | grep -E "error:" >&2 || true
if [ ! -f "$WORK/pdf_render_path_test.exe" ]; then
  echo "FAIL  the render path test did not build" >&2
  exit 4
fi

"$CXX" $FLAGS -o "$WORK/gdi_plus_flat_test.exe" \
    "$WORK/gdi_plus_flat_test.cpp" "$WORK/src/gdi_plus_flat.cpp" -lole32 2>&1 | grep -E "error:" >&2 || true
if [ ! -f "$WORK/gdi_plus_flat_test.exe" ]; then
  echo "FAIL  the GDI+ resolver test did not build" >&2
  exit 4
fi

echo "The checks were built."
echo

cp "$DLL_SOURCE" "$WORK/"

status=0

echo "--- Check 1: the loader without the library ---"
mv "$WORK/pdfium.dll" "$WORK/pdfium.dll.hidden"
"$WORK/pdf_renderer_test.exe" > "$WORK/out1.txt" 2>&1
result=$?
if [ $result -ne 0 ] && grep -q "was not found" "$WORK/out1.txt"; then
  echo "PASS  the missing library is reported clearly"
else
  echo "FAIL  the missing library was not reported clearly"
  cat "$WORK/out1.txt"
  status=1
fi
mv "$WORK/pdfium.dll.hidden" "$WORK/pdfium.dll"
echo

echo "--- Check 2: the loader with the library ---"
if "$WORK/pdf_renderer_test.exe" > "$WORK/out2.txt" 2>&1; then
  sed 's/^/  /' "$WORK/out2.txt"
else
  echo "FAIL  the loader could not rasterise a page"
  cat "$WORK/out2.txt"
  status=1
fi
echo

echo "--- Check 3: the GDI+ resolver ---"
if "$WORK/gdi_plus_flat_test.exe" > "$WORK/out_gdi.txt" 2>&1; then
  sed 's/^/  /' "$WORK/out_gdi.txt"
else
  echo "FAIL  the GDI+ resolver did not work"
  cat "$WORK/out_gdi.txt"
  status=1
fi
echo

echo "--- Check 4: the application render path ---"
if "$WORK/pdf_render_path_test.exe" > "$WORK/out3.txt" 2>&1; then
  sed 's/^/  /' "$WORK/out3.txt"
else
  echo "FAIL  the render path did not produce printable pages"
  cat "$WORK/out3.txt"
  status=1
fi
echo

if [ $status -eq 0 ]; then
  echo "ALL PDF CHECKS PASSED"
else
  echo "SOME PDF CHECKS FAILED"
fi
exit $status
