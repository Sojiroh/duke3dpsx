#!/bin/bash
# Configure + build the PS1 executable and CD image.
# Usage: ./build.sh [iso|clean]      env: BUILD_DIR=build-xyz (default build), DUKE_GRP=path, DUKE_SF2=file.sf2 (music
#   instrument bank; default: built-in synthesis), DUKE_VIEWER=ON (engine viewer), EXTRA_CFLAGS="-DFOO=1".
#   DUKE_GRP / DUKE_SF2 / DUKE_VIEWER / EXTRA_CFLAGS are only passed to CMake when set, so a configured build dir
#   keeps its cached values; set EXTRA_CFLAGS="" (set but empty) to clear the flags of a cached dir.
set -e -o pipefail
P="$(cd "$(dirname "$0")" && pwd)"
SDK=$P/tools/sdk/PSn00bSDK-0.24-Linux
B=$P/${BUILD_DIR:-build}
export PATH=$P/tools/gcc/bin:$SDK/bin:$PATH
if [ "$1" = clean ]; then rm -rf "$B"; exit 0; fi
cmake -S "$P" -B "$B" -DCMAKE_TOOLCHAIN_FILE=$SDK/lib/libpsn00b/cmake/sdk.cmake \
      -DPSN00BSDK_TC=$P/tools/gcc -DCMAKE_BUILD_TYPE=Release ${DUKE_GRP:+"-DDUKE_GRP=$DUKE_GRP"} \
      ${DUKE_SF2+"-DDUKE_SF2=$DUKE_SF2"} ${DUKE_VIEWER:+"-DDUKE_VIEWER=$DUKE_VIEWER"} \
      ${EXTRA_CFLAGS+"-DEXTRA_CFLAGS=$EXTRA_CFLAGS"} > /dev/null
cmake --build "$B" -j8 --target ${1:-duke3d}
# RAM budget: the stack grows down from 0x801FFFF0; keep 32 KB between the end of .bss (_end) and the stack
# top. That reserve also holds the SDK heap (InitHeap at _end, a few KB used by CdSearchFile) and the stack.
END=$(mipsel-none-elf-nm "$B/duke3d.elf" | awk '$3=="_end"{print substr($1,length($1)-7)}')
[ -n "$END" ] || { echo "error: could not read _end from $B/duke3d.elf" >&2; exit 1; }
if [ $((16#$END)) -gt $((16#801F8000)) ]; then
  echo "error: RAM budget exceeded: _end=0x$END > 0x801F8000 (need 32 KB stack)" >&2; exit 1
fi
echo "RAM: _end=0x$END, stack reserve $(( (16#801FFFF0 - 16#$END) / 1024 )) KB"
