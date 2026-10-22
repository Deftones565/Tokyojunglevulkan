#!/usr/bin/env bash
# Tokyo Jungle: start here.
#
#   ./tokyojungle.sh             the launcher, which sets everything up and plays
#   ./tokyojungle.sh --setup     the same without a window (see --help)
#
# The port is built on this computer from your own copy of the game (installed
# and decrypted in RPCS3: docs/SETUP_GUIDE.md). This script makes sure the
# build tools are here -- installing the missing ones with the system's package
# manager, which asks for your password -- fetches the recompiler (the
# ps3recomp submodule) if it is missing, builds the launcher, and starts it.
# The launcher does the rest (launcher/launcher.cpp).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

say() { printf '[tokyojungle] %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
header() { local d; for d in /usr/include /usr/local/include; do [ -f "$d/$1" ] && return 0; done; return 1; }
cmake_pkg() { compgen -G "/usr/lib*/cmake/$1/*onfig.cmake" >/dev/null || compgen -G "/usr/lib/*/cmake/$1/*onfig.cmake" >/dev/null \
              || compgen -G "/usr/share/cmake/$1/*onfig.cmake" >/dev/null || compgen -G "/usr/local/lib*/cmake/$1/*onfig.cmake" >/dev/null; }

# What the build needs (CMakeLists.txt and third_party/ps3recomp/CMakeLists.txt):
# clang (the lifted SPU code needs its musttail), CMake 3.20+, Ninja, Python 3.11+
# with venv (the lifter), git (the submodule), and the Vulkan, SDL2, FFmpeg and
# glslang development files (glslang runs the game's own shaders on Vulkan).
missing=()
have clang && have clang++ || missing+=(clang)
have cmake || missing+=(cmake)
have ninja || missing+=(ninja)
have git || missing+=(git)
if ! have python3 || ! python3 -c 'import sys, venv, ensurepip; sys.exit(sys.version_info < (3, 11))' 2>/dev/null; then
    missing+=(python)
fi
header vulkan/vulkan.h || missing+=(vulkan)
header SDL2/SDL.h || missing+=(sdl2)
header libavcodec/avcodec.h || header ffmpeg/libavcodec/avcodec.h || missing+=(ffmpeg)
cmake_pkg glslang || missing+=(glslang)

if [ ${#missing[@]} -gt 0 ]; then
    . /etc/os-release 2>/dev/null || true
    os=" ${ID:-} ${ID_LIKE:-} "
    declare -A arch=([clang]=clang [cmake]=cmake [ninja]=ninja [git]=git [python]=python
                     [vulkan]="vulkan-headers vulkan-icd-loader" [sdl2]=sdl2 [ffmpeg]=ffmpeg [glslang]=glslang)
    declare -A deb=([clang]=clang [cmake]=cmake [ninja]=ninja-build [git]=git [python]="python3 python3-venv"
                    [vulkan]="libvulkan-dev" [sdl2]=libsdl2-dev [ffmpeg]="libavcodec-dev libavutil-dev"
                    [glslang]="glslang-dev glslang-tools spirv-tools")
    declare -A fed=([clang]=clang [cmake]=cmake [ninja]=ninja-build [git]=git [python]=python3
                    [vulkan]="vulkan-headers vulkan-loader" [sdl2]=SDL2-devel [ffmpeg]=ffmpeg-free-devel [glslang]=glslang-devel)
    pkgs=()
    case "$os" in
    *" steamos "*)
        say "Missing build tools: ${missing[*]}"
        say "SteamOS's system is read-only, so they can't be installed on it. Build in a distrobox"
        say "(an Arch one: distrobox create -i archlinux), or on another Linux PC."
        exit 1 ;;
    *" arch "*)   for m in "${missing[@]}"; do pkgs+=(${arch[$m]}); done; cmd=(pacman -S --needed --noconfirm) ;;
    *" debian "*|*" ubuntu "*)
                  for m in "${missing[@]}"; do pkgs+=(${deb[$m]}); done; cmd=(apt-get install -y) ;;
    *" fedora "*) for m in "${missing[@]}"; do pkgs+=(${fed[$m]}); done; cmd=(dnf install -y) ;;
    *)
        say "Missing build tools: ${missing[*]}"
        say "This system (${PRETTY_NAME:-unknown}) isn't one the script knows how to install them on;"
        say "install them with its package manager and run this again."
        exit 1 ;;
    esac
    say "Missing build tools: ${missing[*]}"
    say "Installing: ${pkgs[*]}"
    if have pkexec; then pkexec "${cmd[@]}" "${pkgs[@]}"; else sudo "${cmd[@]}" "${pkgs[@]}"; fi
fi

# The recompiler, when this copy came without it.
if [ ! -f third_party/ps3recomp/CMakeLists.txt ]; then
    say "Fetching the recompiler (ps3recomp)"
    git submodule update --init third_party/ps3recomp
fi

# The launcher, built (or brought up to date) in build-launcher/.
if [ ! -f build-launcher/build.ninja ]; then
    cmake -S launcher -B build-launcher -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release >/dev/null
fi
ninja -C build-launcher >/dev/null
exec build-launcher/tokyojungle-launcher "$@"
