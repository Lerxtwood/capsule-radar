#!/usr/bin/env bash
# Build the firmware and produce a single merged binary for local browser-flasher
# testing. The published GitHub Pages installer normally points at the latest
# GitHub Release binary instead of committing this generated .bin.
#
#   ./scripts/build_webflasher.sh
#   then serve it over HTTPS/localhost, e.g.:  python3 -m http.server -d web/flash 8000
#   and open http://localhost:8000  (Web Serial works on localhost & HTTPS only)
set -euo pipefail
cd "$(dirname "$0")/.."

ENV=esp32-s3-amoled-175
B=".pio/build/$ENV"
OUT="web/flash/CapsuleRadar-esp32s3.bin"

echo "==> Building firmware ($ENV)"
pio run -e "$ENV"

echo "==> Locating tools"
BOOT_APP0="$(find "$HOME/.platformio/packages" -name boot_app0.bin -path '*framework-arduinoespressif32*' 2>/dev/null | head -1)"
ESPTOOL="$(find "$HOME/.platformio/packages" -name esptool.py -path '*tool-esptoolpy*' 2>/dev/null | head -1)"
PY="$HOME/.platformio/penv/bin/python"
[ -x "$PY" ] || PY=python3

echo "==> Merging bootloader + partitions + app -> $OUT"
mkdir -p web/flash
"$PY" "$ESPTOOL" --chip esp32s3 merge_bin -o "$OUT" \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0     "$B/bootloader.bin" \
  0x8000  "$B/partitions.bin" \
  0xe000  "$BOOT_APP0" \
  0x10000 "$B/firmware.bin"

echo "==> Done: $OUT ($(du -h "$OUT" | cut -f1))"
echo "==> Note: published web/flash/manifest.json uses the latest GitHub Release asset."
