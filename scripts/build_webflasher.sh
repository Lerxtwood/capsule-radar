#!/usr/bin/env bash
# Prepare web/flash/ for local browser-installer testing.
#
# The published GitHub Pages workflow copies web/flash/ and mirrors these files
# from the latest GitHub Release. This helper does the same thing locally so the
# manifest can be served from localhost:
#
#   ./scripts/build_webflasher.sh
#   python3 -m http.server -d web/flash 8000
#   # open http://localhost:8000
#
# Web Serial works from localhost or HTTPS only.
set -euo pipefail
cd "$(dirname "$0")/.."

REPO="${GITHUB_REPOSITORY:-Lerxtwood/capsule-radar}"
BASE_URL="https://github.com/${REPO}/releases/latest/download"
ASSETS=(
  bootloader.bin
  partition-table.bin
  ota_data_initial.bin
  CapsuleRadar-ota.bin
  PrintSphere-ota.bin
  sprites.pak
)

mkdir -p web/flash

for asset in "${ASSETS[@]}"; do
  echo "==> Downloading ${asset}"
  curl -L --fail -o "web/flash/${asset}" "${BASE_URL}/${asset}"
done

echo "==> Done. Serve with: python3 -m http.server -d web/flash 8000"
