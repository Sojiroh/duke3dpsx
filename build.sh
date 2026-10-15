#!/bin/bash
# Configure + build the PS1 executable and CD image.
# Usage: ./build.sh [iso|clean]      env: BUILD_DIR=build-xyz (default build), DUKE_GRP=path, DUKE_SF2=file.sf2 (music
#   instrument bank; default: built-in synthesis), DUKE_VIEWER=ON (engine viewer), EXTRA_CFLAGS="-DFOO=1",
#   DUKE_GAME_OPT=-Os (optimisation flag for src/game/*.c; CMake default -Os, "" = the SDK's -O2),
#   DUKE_GAME_KEEP="actors;player" (game sources, base names, that keep the SDK -O2; default "actors"; ps1_control;ps1_platform always do),
#   DUKE_PYTHON=/path/to/python3 (interpreter for tools/mkdata.py; default: found by CMake).
#   DUKE_GRP / DUKE_SF2 / DUKE_VIEWER / DUKE_GAME_OPT / DUKE_GAME_KEEP / DUKE_PYTHON / EXTRA_CFLAGS are only passed to CMake when set, so a configured build dir
#   keeps its cached values; set EXTRA_CFLAGS="" (set but empty) to clear the flags of a cached dir.
#   Default target: the executable plus the RAM budget check (CMake target `ramcheck`, prints the "RAM: _end=..." line
#   and fails when less than 32 KB of stack reserve is left); `iso` additionally builds DUKE.DAT and the CD image.
set -e -o pipefail
P="$(cd "$(dirname "$0")" && pwd)"
SDK=$P/tools/sdk/PSn00bSDK-0.24-Linux
B=$P/${BUILD_DIR:-build}
export PATH=$P/tools/gcc/bin:$SDK/bin:$PATH
if [ "$1" = clean ]; then rm -rf "$B"; exit 0; fi
cmake -S "$P" -B "$B" -DCMAKE_TOOLCHAIN_FILE=$SDK/lib/libpsn00b/cmake/sdk.cmake \
      -DPSN00BSDK_TC=$P/tools/gcc -DCMAKE_BUILD_TYPE=Release ${DUKE_GRP:+"-DDUKE_GRP=$DUKE_GRP"} \
      ${DUKE_SF2+"-DDUKE_SF2=$DUKE_SF2"} ${DUKE_VIEWER:+"-DDUKE_VIEWER=$DUKE_VIEWER"} ${DUKE_PYTHON:+"-DPython3_EXECUTABLE=$DUKE_PYTHON"} \
      ${DUKE_GAME_OPT+"-DDUKE_GAME_OPT=$DUKE_GAME_OPT"} ${DUKE_GAME_KEEP+"-DDUKE_GAME_KEEP=$DUKE_GAME_KEEP"} ${EXTRA_CFLAGS+"-DEXTRA_CFLAGS=$EXTRA_CFLAGS"} > /dev/null
cmake --build "$B" -j8 --target ${1:-ramcheck}
