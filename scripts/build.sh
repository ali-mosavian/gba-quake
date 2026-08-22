#!/bin/sh
# Build every ROM, with devkitARM if it is installed and in a container if not.
set -eu
cd "$(dirname "$0")/.."
# Arguments are make goals; with none, build everything.
[ $# -eq 0 ] && set -- native
if [ -x /opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc ]; then
    exec make "$@"
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "Need devkitARM at /opt/devkitpro or Docker." >&2
    exit 1
fi
# The BSP and the Quake pak live outside the repo, so bind mount their
# directories at the same absolute paths the Makefile names. Without this the
# container cannot regenerate src/generated/bsp_wireframe_map.h, and it will
# try to: the map settings are stamped into a file the header depends on, and
# a container that did not inherit BSP_MERGE and friends stamps different
# values and invalidates a header the host had just built correctly. Passing
# them through as environment variables is what keeps the two makes agreeing --
# every BSP_* variable is declared with ?= for exactly that reason.
# Headers whose generators need host-only Python packages are built here
# before the container runs; the map header regenerates fine in either place.
make -s src/generated/bsp_wireframe_map.h src/generated/maps_index.h src/generated/beam_frame.h \
    src/generated/floor_plan.h >/dev/null
mounts=""
for path in "$(make -s print-BSP_MAP)" "$(make -s print-BSP_PAK)"; do
    directory=$(dirname "$path")
    [ -r "$directory" ] && mounts="$mounts -v $directory:$directory:ro"
done
# shellcheck disable=SC2086
exec docker run --rm -v "$PWD:/work" $mounts -w /work \
    -e BSP_MAP -e BSP_PAK -e BSP_MIP -e BSP_LMSCALE -e BSP_LMGAIN -e BSP_MERGE \
    devkitpro/devkitarm:latest make "$@"
