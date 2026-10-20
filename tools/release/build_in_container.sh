#!/bin/bash
# Runs inside the tj-release container (tools/release/Dockerfile), started by make_appimage.sh.
# /src = this checkout (read-only), /out = the output dir. Builds the game the way build/ is
# built (RelWithDebInfo = -O2, the same options), then assembles AppDir and the AppImage.
set -euo pipefail
B=/out/.build; A=/out/.AppDir
cmake -S /src -B $B -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=clang-22 -DCMAKE_CXX_COMPILER=clang++-22 \
      -DPS3RECOMP_FFMPEG_INCLUDE_DIR=/opt/ffmpeg/include ${CMAKE_EXTRA:-}
nice -n 10 cmake --build $B -j${JOBS:-10}

rm -rf $A; mkdir -p $A/usr/bin $A/usr/lib
cp $B/tokyojungle $A/usr/bin/
strip --strip-debug $A/usr/bin/tokyojungle
patchelf --set-rpath '$ORIGIN/../lib' $A/usr/bin/tokyojungle

# Bundle every shared library it links except the base system ones (glibc,
# libstdc++/libgcc: SteamOS's are newer) and the display/audio/GPU stack, which
# must be the system's own (it matches the Deck's graphics driver), plus FFmpeg,
# which it dlopens.
ldd $B/tokyojungle | awk '/=> \//{print $3}' | while read -r l; do
    case "$(basename "$l")" in
        libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|ld-linux*|libstdc++.so*|libgcc_s.so*) ;;
        libX*|libxcb*|libwayland*|libdecor*|libxkbcommon*|libdrm*|libgbm*|libEGL*|libGL*|libvulkan*) ;;
        libasound*|libpulse*|libpipewire*|libdbus*|libsystemd*|libudev*) ;;
        *) cp -L "$l" $A/usr/lib/ ;;
    esac
done
cp -L /opt/ffmpeg/lib/libavcodec.so.[0-9]* /opt/ffmpeg/lib/libavutil.so.[0-9]* $A/usr/lib/
for l in $A/usr/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$l" 2>/dev/null || true; done

cp /src/tools/release/AppRun $A/AppRun; chmod +x $A/AppRun
cp /src/tools/release/tokyojungle.desktop $A/
cp /src/tools/release/tokyojungle.png $A/tokyojungle.png
ln -sf tokyojungle.png $A/.DirIcon

echo "--- newest glibc symbol each file needs:"
for f in $A/usr/bin/tokyojungle $A/usr/lib/*.so*; do
    printf '%-48s %s\n' "$(basename $f)" "$(objdump -T $f | grep -o 'GLIBC_2\.[0-9]*' | sort -uV | tail -1)"
done
ARCH=x86_64 /opt/appimagetool/AppRun --no-appstream $A /out/TokyoJungle-x86_64.AppImage
