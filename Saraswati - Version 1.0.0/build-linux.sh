#!/usr/bin/env bash
# Builds Saraswati on Linux (GCC or Clang) and copies it to App-Data/App/saraswati.
# Needs: git cmake ninja-build g++ and SDL3's X11/Wayland dev packages, e.g. on Ubuntu/Debian:
#   sudo apt install git cmake ninja-build g++ libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
#        libxi-dev libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev libwayland-dev \
#        wayland-protocols libegl-dev libdbus-1-dev libudev-dev libvulkan1
# For CPU Vulkan (--cpu-vulkan) also: mesa-vulkan-drivers
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/App-Data/Source"
bash fetch-deps.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
mkdir -p "$here/App-Data/App"
cp build/saraswati "$here/App-Data/App/saraswati"
echo "Done: App-Data/App/saraswati   (run: App-Data/App/saraswati, or with --cpu-vulkan)"
