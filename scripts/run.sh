#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
name=${1:-baseline}
scale=${2:-1}
rom="build/$name.gba"
[ -f "$rom" ] || ./scripts/build.sh
if [ -x work/mgba-0.10.5/build-sdl/sdl/mgba ]; then
    exec work/mgba-0.10.5/build-sdl/sdl/mgba --scale "$scale" "$rom"
fi
if command -v mgba >/dev/null 2>&1; then
    exec mgba --scale "$scale" "$rom"
fi
if [ -x /opt/homebrew/opt/mgba/mGBA.app/Contents/MacOS/mGBA ]; then
    exec /opt/homebrew/opt/mgba/mGBA.app/Contents/MacOS/mGBA --scale "$scale" "$rom"
fi
echo "mGBA not found. Install it with: brew install mgba" >&2
exit 1
