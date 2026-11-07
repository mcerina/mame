#!/bin/bash
# Build the netplay emulator on macOS, Apple Silicon or Intel: the result is ./np
# in the mame folder. Needs the Xcode command line tools and SDL2 from Homebrew.
set -e
cd "$(dirname "$0")/.."
command -v clang >/dev/null || { echo "Missing the Xcode command line tools: run  xcode-select --install"; exit 1; }
command -v sdl2-config >/dev/null || { echo "Missing SDL2: install Homebrew (https://brew.sh), then run  brew install sdl2"; exit 1; }
SRCS=$(grep -v '^#' netplay/sources.txt | paste -sd, -)
# -ffp-contract=off: no fused multiply-add, so floating point rounds as on x86
make -j"$(sysctl -n hw.ncpu)" SUBTARGET=np SOURCES="$SRCS" USE_LIBSDL=1 USE_QTDEBUG=0 \
  NEW_GIT_VERSION="$(netplay/version.sh)" ARCHOPTS="-ffp-contract=off" "$@"
echo "Done: $(pwd)/np, version $(netplay/version.sh)"
