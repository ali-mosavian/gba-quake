#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
src="$root/work/mgba-0.10.5"
build="$src/build-sdl"
if [ ! -d "$src/.git" ]; then
    mkdir -p "$root/work"
    git clone --depth 1 --branch 0.10.5 https://github.com/mgba-emu/mgba.git "$src"
fi
cmake -S "$src" -B "$build" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_QT=OFF -DBUILD_SDL=ON -DBUILD_GL=OFF \
    -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DUSE_EPOXY=OFF \
    -DCMAKE_PREFIX_PATH="$(brew --prefix sdl2-compat);$(brew --prefix libzip)"
cmake --build "$build" --target mgba-sdl -j 4
printf '%s\n' "$build/sdl/mgba"
