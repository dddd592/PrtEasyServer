#!/usr/bin/env bash
# PrtEasyServer feature smoke test.
#
# Exercises the new print paths against a running PrtEasyServer instance.
# Start the server first, then run this script with the server address:
#
#   bash tools/smoke_test.sh 192.168.1.50
#   bash tools/smoke_test.sh 192.168.1.50 8080
#
# The script never asserts that paper was produced. It only reports what the
# server answered, because the spooler accepting a job does not prove that
# the printer produced output.

set -u

HOST="${1:-127.0.0.1}"
PORT="${2:-80}"
BASE="http://${HOST}:${PORT}"

if command -v curl >/dev/null 2>&1; then
  HAVE_CURL=1
else
  HAVE_CURL=0
fi

if command -v python3 >/dev/null 2>&1; then
  HAVE_PY=1
else
  HAVE_PY=0
fi

pass=0
fail=0

report() {
  local label="$1"
  local status="$2"
  local detail="$3"
  if [ "$status" = "ok" ]; then
    pass=$((pass + 1))
    echo "PASS  ${label}"
  else
    fail=$((fail + 1))
    echo "FAIL  ${label} - ${detail}"
  fi
}

echo "PrtEasyServer smoke test against ${BASE}"
echo

# --- 1. The printer list endpoint must answer with JSON. ---
if [ "$HAVE_CURL" = "1" ]; then
  printers_json="$(curl -s --max-time 10 "${BASE}/api/printers")"
  if [ -n "$printers_json" ]; then
    report "GET /api/printers" ok ""
    echo "      ${printers_json}"
  else
    report "GET /api/printers" bad "no response"
  fi
else
  echo "SKIP  curl is not available"
fi

# --- 2. A print request without a printer name must be rejected. ---
if [ "$HAVE_CURL" = "1" ]; then
  code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
          -X POST --data-binary 'test' "${BASE}/api/print")"
  if [ "$code" = "400" ]; then
    report "POST /api/print without a printer is rejected" ok ""
  else
    report "POST /api/print without a printer is rejected" bad "expected 400, got ${code}"
  fi
fi

# --- 3. A GET on the print endpoint must be rejected. ---
if [ "$HAVE_CURL" = "1" ]; then
  code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "${BASE}/api/print")"
  if [ "$code" = "405" ]; then
    report "GET /api/print is rejected" ok ""
  else
    report "GET /api/print is rejected" bad "expected 405, got ${code}"
  fi
fi

# --- 4. Extract a printer name and send a real text job. ---
PRINTER_NAME=""
if [ "$HAVE_CURL" = "1" ] && [ "$HAVE_PY" = "1" ]; then
  printers_json="$(curl -s --max-time 10 "${BASE}/api/printers")"
  PRINTER_NAME="$(printf '%s' "$printers_json" | python3 -c "
import json, sys
try:
    data = json.load(sys.stdin)
    names = [item['name'] for item in data.get('printers', [])]
    print(names[0] if names else '')
except Exception:
    print('')
")"

  if [ -n "$PRINTER_NAME" ]; then
    echo
    echo "Sending a text job to printer: ${PRINTER_NAME}"
    code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 30 \
            -X POST --data-binary $'PrtEasyServer render test\nsecond line\n' \
            "${BASE}/api/print?printer=$(python3 -c "
import urllib.parse, sys
print(urllib.parse.quote(sys.argv[1]))
" "$PRINTER_NAME")")"
    if [ "$code" = "200" ]; then
      report "POST /api/print with a text document" ok ""
    else
      report "POST /api/print with a text document" bad "expected 200, got ${code}"
    fi

    # A job with a paper size exercises the DEVMODE path.
    code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 30 \
            -X POST --data-binary 'paper size test' \
            "${BASE}/api/print?printer=$(python3 -c "
import urllib.parse, sys
print(urllib.parse.quote(sys.argv[1]))
" "$PRINTER_NAME")&paper=A4&orientation=portrait&copies=1")"
    if [ "$code" = "200" ]; then
      report "POST /api/print with paper and orientation" ok ""
    else
      report "POST /api/print with paper and orientation" bad "expected 200, got ${code}"
    fi
  else
    echo "SKIP  no printer is configured, so no print job was sent"
  fi
fi

echo
echo "Passed: ${pass}   Failed: ${fail}"
echo
echo "Note: these checks only confirm what the server replied. Confirm the"
echo "paper output separately, and always test with a non-critical printer first."
exit $((fail > 0 ? 1 : 0))
