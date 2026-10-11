#!/bin/sh
# Builds DSDA-Doom for the 3DS. Run inside the devkitpro/devkitarm image,
# from the repository root, after tools/3ds/build-deps.sh.
set -eu

DEPS="$PWD/deps-3ds"

cmake -S prboom2 -B build-3ds \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$DEPS" \
    -DCMAKE_FIND_ROOT_PATH="$DEPS" \
    -DWITH_IMAGE=OFF -DWITH_MAD=OFF -DWITH_FLUIDSYNTH=OFF \
    -DWITH_XMP=OFF -DWITH_VORBISFILE=OFF -DWITH_PORTMIDI=OFF \
    -DENABLE_PACKAGING=OFF
cmake --build build-3ds -j"$(nproc)" "$@"
