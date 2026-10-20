# Build environment for the Steam Deck / portable Linux release (tools/release/make_appimage.sh).
# Ubuntu 24.04 = glibc 2.39, older than SteamOS 3.8's 2.41, so the binary and every bundled
# library run there. Same compiler major (clang 22, apt.llvm.org) and FFmpeg major (9) as build/.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates wget gnupg lsb-release software-properties-common \
      build-essential cmake ninja-build python3 pkg-config nasm file patchelf \
      libsdl2-dev glslang-dev glslang-tools spirv-tools libspirv-cross-c-shared-dev \
      libvulkan-dev squashfs-tools desktop-file-utils xz-utils \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && bash /tmp/llvm.sh 22 \
 && rm -rf /var/lib/apt/lists/*
# Minimal FFmpeg 9 (the game decodes AC3 audio and H.264/MPEG-2 movies through it), no external deps.
RUN wget -qO /tmp/ff.tar.xz https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
 && mkdir /tmp/ff && tar -xf /tmp/ff.tar.xz -C /tmp/ff --strip-components=1 && cd /tmp/ff \
 && ./configure --prefix=/opt/ffmpeg --enable-shared --disable-static --disable-programs \
      --disable-doc --disable-avdevice --disable-avformat --disable-swscale --disable-swresample \
      --disable-avfilter --disable-network --disable-everything \
      --enable-decoder=ac3,h264,mpeg2video --enable-parser=ac3,h264,mpegvideo \
      --disable-autodetect \
 && make -j4 && make install && rm -rf /tmp/ff /tmp/ff.tar.xz
# SDL2 from source: its default build dlopens X11, Wayland, PulseAudio/PipeWire/ALSA, D-Bus and
# udev, so the bundled libSDL2 needs only glibc and the Deck's own display/audio stack is used.
# (Ubuntu's libsdl2 links them directly, which would drag them all into the AppImage.)
RUN apt-get update && apt-get remove -y libsdl2-dev libsdl2-2.0-0 && apt-get install -y --no-install-recommends \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
      libxrender-dev libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev libegl-dev \
      libpulse-dev libasound2-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libdrm-dev libgbm-dev \
 && rm -rf /var/lib/apt/lists/* \
 && wget -qO /tmp/sdl.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz \
 && mkdir /tmp/sdl && tar -xzf /tmp/sdl.tar.gz -C /tmp/sdl --strip-components=1 \
 && cmake -S /tmp/sdl -B /tmp/sdl/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DSDL_STATIC=OFF -DSDL_TEST=OFF \
 && cmake --build /tmp/sdl/b -j8 && cmake --install /tmp/sdl/b && rm -rf /tmp/sdl /tmp/sdl.tar.gz && ldconfig
ENV CC=clang-22 CXX=clang++-22
# appimagetool, unpacked (no FUSE in a container).
RUN wget -qO /tmp/ait https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage \
 && chmod +x /tmp/ait && cd /opt && /tmp/ait --appimage-extract >/dev/null && mv squashfs-root appimagetool \
 && chmod -R a+rX /opt/appimagetool && rm /tmp/ait
# OpenSSL headers for psnr's TLS (0120). Linked, not bundled: build_in_container.sh leaves
# libssl/libcrypto to the system, whose OpenSSL knows where that system keeps its CA certificates.
RUN apt-get update && apt-get install -y --no-install-recommends libssl-dev && rm -rf /var/lib/apt/lists/*
