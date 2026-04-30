#!/usr/bin/env bash
# Re-syncs ../reedboard-app.html → firmware/data/index.html
# Run before each `pio run -t uploadfs` (USB) or `pio run -e chainlink_ota -t uploadfs` (WiFi).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../reedboard-app.html"
DEST="$HERE/firmware/data/index.html"
if [[ ! -f "$SRC" ]]; then
  echo "ERROR: $SRC not found" >&2
  exit 1
fi
cp "$SRC" "$DEST"
echo "Synced UI: $(wc -c < "$DEST") bytes → firmware/data/index.html"
echo "Now: pio run -t uploadfs    (USB)"
echo "  or: pio run -e chainlink_ota -t uploadfs   (WiFi)"
