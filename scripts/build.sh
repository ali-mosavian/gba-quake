#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ -x /opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc ]; then
    exec make native
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "Need devkitARM at /opt/devkitpro or Docker." >&2
    exit 1
fi
exec docker run --rm -v "$PWD:/work" -w /work devkitpro/devkitarm:latest make native

