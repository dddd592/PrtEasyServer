#!/usr/bin/env bash
# Local-only syntax check for the PrtEasyServer sources.
#
# The upstream code uses two Microsoft extension syntaxes that libstdc++
# does not accept:
#   * std::ifstream / std::ofstream constructed from a std::wstring
#   * the same streams opened with a std::wstring path
#
# This script copies the sources into a scratch directory, rewrites only those
# extension lines into portable equivalents, and then runs a syntax-only
# compile over every file with the MinGW toolchain. The shipped sources are
# never modified.
#
# The MSVC build script (build-windows10.cmd) remains the authoritative
# build; this check only guards against syntax and type errors.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-/c/tools/mingw64/bin/g++.exe}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$CXX" ]; then
  echo "g++ not found at $CXX - set CXX to a working compiler." >&2
  exit 2
fi

cp -r "$ROOT/src" "$WORK/src"

# Rewrite the Microsoft extension stream constructions into portable ones.
python3 - "$WORK/src" <<'PY'
import os, re, sys
root = sys.argv[1]
for name in os.listdir(root):
    if not name.endswith(('.cpp', '.h')):
        continue
    path = os.path.join(root, name)
    with open(path, encoding='utf-8') as handle:
        text = handle.read()
    original = text

    def wrap_first_arg(source, pattern):
        """Wraps the first argument of a call so a wstring path becomes narrow."""
        out = []
        index = 0
        compiled = re.compile(pattern)
        while True:
            match = compiled.search(source, index)
            if match is None:
                out.append(source[index:])
                break
            out.append(source[index:match.end()])

            # Find the end of the argument list, tracking nesting.
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
            argument_list = source[match.end():cursor]

            # Split off the first argument at the top level comma.
            arg_depth = 0
            split_at = -1
            for position, char in enumerate(argument_list):
                if char in '([{':
                    arg_depth += 1
                elif char in ')]}':
                    arg_depth -= 1
                elif char == ',' and arg_depth == 0:
                    split_at = position
                    break

            if split_at < 0:
                first_argument = argument_list
                remainder = ''
            else:
                first_argument = argument_list[:split_at]
                remainder = argument_list[split_at:]

            out.append('Narrow(')
            out.append(first_argument)
            out.append(')')
            out.append(remainder)
            index = cursor
        return ''.join(out)

    # std::ifstream input(path, mode);  ->  std::ifstream input(Narrow(path), mode);
    text = wrap_first_arg(text, r'std::ifstream\s+\w+\(')
    text = wrap_first_arg(text, r'std::ofstream\s+\w+\(')
    text = wrap_first_arg(text, r'\w+\.open\(')
    text = wrap_first_arg(text, r'\w+->open\(')
    if text != original:
        with open(path, 'w', encoding='utf-8') as handle:
            handle.write(text)
PY

# Provide the Narrow() helper used above.
cat > "$WORK/src/wide_narrow_helper.h" <<'EOF'
#pragma once
#include <string>
#include <windows.h>
inline std::string Narrow(const std::wstring& path) {
    if (path.empty()) return std::string();
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string buffer(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, buffer.data(), needed, nullptr, nullptr);
    if (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();
    return buffer;
}
EOF

# Inject the helper into the translation units that need it.
for f in "$WORK"/src/*.cpp; do
  sed -i '1i #include "wide_narrow_helper.h"' "$f"
done

FLAGS="-std=c++17 -fsyntax-only -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DWINVER=0x0601"

status=0
for f in "$WORK"/src/*.cpp; do
  base="$(basename "$f")"
  out="$("$CXX" $FLAGS -I"$WORK/src" "$f" 2>&1 | grep -E "error:")"
  if [ -n "$out" ]; then
    echo "FAIL $base"
    echo "$out" | head -12
    status=1
  else
    echo "OK   $base"
  fi
done

exit $status
