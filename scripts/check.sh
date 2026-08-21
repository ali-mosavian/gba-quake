#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
./scripts/build.sh
for rom in build/*.gba; do
    test -s "$rom"
    size=$(wc -c < "$rom" | tr -d ' ')
    printf '%-24s %s bytes\n' "$rom" "$size"
done
command -v mgba >/dev/null 2>&1 && mgba --version | head -1 || true

