#!/usr/bin/env bash
# Downloads the PDFium build that the render path uses and installs it into
# the project.
#
# PDFium is the PDF engine of Chromium and is distributed under the MIT
# licence, which is compatible with this project's licence. It is loaded at
# run time, so it is an optional dependency: a deployment that never prints
# a PDF does not need it.
#
# Usage:
#   bash tools/fetch_pdfium.sh                 # install into third_party/pdfium
#   bash tools/fetch_pdfium.sh --version X     # pick a specific release
#   bash tools/fetch_pdfium.sh --verify        # only check what is installed
#
# The script needs curl and tar. Nothing is installed system wide.

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_DIR="$ROOT/third_party/pdfium"
DLL_NAME="pdfium.dll"
RELEASE_REPO="bblanchon/pdfium-binaries"
DEFAULT_VERSION="chromium/8111"

VERSION="$DEFAULT_VERSION"
VERIFY_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --version)
      VERSION="$2"
      shift 2
      ;;
    --verify)
      VERIFY_ONLY=1
      shift
      ;;
    -h|--help)
      sed -n '2,20p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      exit 2
      ;;
  esac
done

report_installed() {
  if [ -f "$TARGET_DIR/$DLL_NAME" ]; then
    size=$(wc -c < "$TARGET_DIR/$DLL_NAME" | tr -d ' ')
    echo "PDFium is installed:"
    echo "  $TARGET_DIR/$DLL_NAME ($size bytes)"
    if [ -f "$TARGET_DIR/LICENSE" ]; then
      echo "  $TARGET_DIR/LICENSE"
    fi
    return 0
  fi
  return 1
}

if [ "$VERIFY_ONLY" = "1" ]; then
  if report_installed; then
    exit 0
  fi
  echo "PDFium is not installed. Run tools/fetch_pdfium.sh to install it." >&2
  exit 1
fi

if report_installed >/dev/null 2>&1; then
  echo "PDFium is already present:"
  report_installed
  echo "Remove $TARGET_DIR first if you want to install a different build."
  exit 0
fi

command -v curl >/dev/null 2>&1 || { echo "curl is required" >&2; exit 3; }
command -v tar  >/dev/null 2>&1 || { echo "tar is required" >&2; exit 3; }

# Resolve the release asset for the requested tag.
asset_url="https://github.com/$RELEASE_REPO/releases/download/${VERSION//\//%2F}/pdfium-win-x64.tgz"

echo "PDFium $VERSION"
echo "  source: $asset_url"
echo

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "Downloading..."
if ! curl -fsSL -o "$WORK/pdfium.tgz" "$asset_url"; then
  echo "The download failed. Check the release tag with:" >&2
  echo "  https://github.com/$RELEASE_REPO/releases" >&2
  exit 4
fi

echo "Extracting..."
# The --force-local flag keeps tar from treating a drive letter as a remote
# host name when it runs under Git Bash on Windows.
tar --force-local -xzf "$WORK/pdfium.tgz" -C "$WORK"

if [ ! -f "$WORK/bin/$DLL_NAME" ]; then
  echo "The archive does not contain bin/$DLL_NAME" >&2
  exit 5
fi

mkdir -p "$TARGET_DIR"
cp "$WORK/bin/$DLL_NAME" "$TARGET_DIR/$DLL_NAME"

# The licence has to travel with the binary.
if [ -f "$WORK/LICENSE" ]; then
  cp "$WORK/LICENSE" "$TARGET_DIR/LICENSE"
fi
if [ -d "$WORK/licenses" ]; then
  cp -r "$WORK/licenses" "$TARGET_DIR/licenses" 2>/dev/null || true
fi

echo
report_installed
echo
echo "The DLL is found automatically in these locations:"
echo "  1. next to PrtEasyServer-Custom.exe"
echo "  2. <exe folder>/third_party/pdfium.dll"
echo "  3. <exe folder>/third_party/pdfium/pdfium.dll"
echo
echo "Copy $TARGET_DIR/pdfium.dll next to the executable, or copy the"
echo "whole third_party/pdfium folder into the deployment folder."
echo
echo "Without the DLL the application still runs, but PDF documents are"
echo "forwarded to the spooler unchanged instead of being rasterised."
