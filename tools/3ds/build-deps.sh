#!/bin/sh
# Builds the libraries DSDA-Doom needs for the 3DS into deps-3ds/ (devkitPro
# has none of them for the 3DS):
# - SDL2 and SDL2_mixer (SDL supports the 3DS officially since 2.24), with
#   the patches in tools/3ds/patches. SDL2_mixer without external decoders:
#   the music is MIDI, played by DSDA-Doom's own OPL emulation.
# - libzip (zip and pk3 mods), with devkitPro's zlib only.
# - libsndfile (sounds in WAV and other formats), without external codecs.
# - The OpenGL headers (Mesa, MIT; Khronos): only to compile. The 3DS draws
#   in software and links no OpenGL code.
#
# Run from the repository root inside the devkitpro/devkitarm image.
set -eu

SDL_VERSION=2.30.9
MIXER_VERSION=2.8.0
LIBZIP_VERSION=1.11.2
SNDFILE_VERSION=1.2.2
PREFIX="$PWD/deps-3ds"
WORK="$PWD/deps-3ds/src"
PATCHES="$PWD/tools/3ds/patches"
TOOLCHAIN="$DEVKITPRO/cmake/3DS.cmake"

[ -f "$PREFIX/lib/libsndfile.a" ] && { echo "deps-3ds: already built"; exit 0; }

mkdir -p "$WORK" "$PREFIX/include"
cp -r tools/3ds/include/GL tools/3ds/include/KHR "$PREFIX/include/"

clone() {
    [ -d "$WORK/$1" ] || git clone -q --depth 1 --branch "$2" "$3" "$WORK/$1"
}

clone SDL "release-$SDL_VERSION" https://github.com/libsdl-org/SDL.git
clone SDL_mixer "release-$MIXER_VERSION" https://github.com/libsdl-org/SDL_mixer.git
clone libzip "v$LIBZIP_VERSION" https://github.com/nih-at/libzip.git
clone libsndfile "$SNDFILE_VERSION" https://github.com/libsndfile/libsndfile.git

for patch in "$PATCHES"/sdl-*.patch; do
    git -C "$WORK/SDL" apply --check "$patch" 2>/dev/null \
        && git -C "$WORK/SDL" apply "$patch" \
        || echo "$(basename "$patch"): already applied"
done

build() {
    name=$1
    shift
    cmake -S "$WORK/$name" -B "$WORK/$name/build" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DBUILD_SHARED_LIBS=OFF \
        "$@"
    cmake --build "$WORK/$name/build" -j"$(nproc)"
    cmake --install "$WORK/$name/build"
}

build SDL -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF

build SDL_mixer \
    -DSDL2MIXER_SAMPLES=OFF \
    -DSDL2MIXER_CMD=OFF \
    -DSDL2MIXER_FLAC=OFF -DSDL2MIXER_GME=OFF -DSDL2MIXER_MOD=OFF \
    -DSDL2MIXER_MP3=OFF -DSDL2MIXER_OPUS=OFF -DSDL2MIXER_VORBIS=OFF \
    -DSDL2MIXER_WAVPACK=OFF -DSDL2MIXER_MIDI=OFF -DSDL2MIXER_WAVE=ON

build libzip \
    -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF \
    -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF \
    -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF \
    -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF

# Its ALAC code passes int32_t (long on ARM) for int: an error in GCC 14.
build libsndfile \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -Wno-error=incompatible-pointer-types" \
    -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
    -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DINSTALL_MANPAGES=OFF \
    -DENABLE_CPACK=OFF
