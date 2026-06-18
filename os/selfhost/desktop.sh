set -euxo pipefail

XR="https://www.x.org/releases/individual"
export PKG_CONFIG_PATH="$X/usr/lib/pkgconfig:$X/usr/share/pkgconfig"
export ACLOCAL_PATH="$X/usr/share/aclocal"
export PATH="$X/usr/bin:$PATH"
export CPPFLAGS="-I$X/usr/include"
export LDFLAGS="-L$X/usr/lib -Wl,-rpath-link,$X/usr/lib"
export FONTROOTDIR="/usr/share/fonts/X11"

xbuild() {
  local url="$1"; shift
  local tarball dir
  tarball="$(basename "$url")"
  cd "$SRC"
  [ -f "$tarball" ] || fetch "$url" "$tarball"
  set +o pipefail
  dir="$(tar tf "$tarball" 2>/dev/null | sed -e 's@/.*@@' | sort -u | grep . | head -1)"
  set -o pipefail
  rm -rf "$dir"
  tar -xf "$tarball"
  cd "$dir"
  if [ -f configure ]; then
    ./configure --prefix=/usr "$@"
    make -j"$JOBS"
    make DESTDIR="$X" install
  elif [ -f meson.build ]; then
    meson setup _b --prefix=/usr --buildtype=release "$@"
    ninja -C _b -j"$JOBS"
    DESTDIR="$X" ninja -C _b install
  else
    echo "no build system for $dir"; exit 1
  fi
  cd "$SRC"
}

echo "===== X build system + protocol ====="
xbuild "$XR/util/util-macros-1.20.1.tar.xz"
xbuild "$XR/proto/xorgproto-2024.1.tar.xz"

echo "===== base libs ====="
xbuild "$XR/lib/libXau-1.0.11.tar.xz"
xbuild "$XR/lib/libpthread-stubs-0.5.tar.xz"
xbuild "https://xorg.freedesktop.org/archive/individual/proto/xcb-proto-1.17.0.tar.xz"
xbuild "$XR/lib/libxcb-1.17.0.tar.xz"
xbuild "$XR/lib/xtrans-1.5.2.tar.xz"
xbuild "$XR/lib/libX11-1.8.10.tar.xz"
xbuild "$XR/lib/libXext-1.3.6.tar.xz"
xbuild "$XR/lib/libXfixes-6.0.1.tar.xz"
xbuild "$XR/lib/libXrender-0.9.11.tar.xz"
xbuild "$XR/lib/libXrandr-1.5.4.tar.xz"
xbuild "$XR/lib/libXi-1.8.2.tar.xz"
xbuild "$XR/lib/libXdamage-1.1.6.tar.xz"
xbuild "$XR/lib/libICE-1.1.2.tar.xz"
xbuild "$XR/lib/libSM-1.2.5.tar.xz"
xbuild "$XR/lib/libXt-1.3.1.tar.xz"
xbuild "$XR/lib/libXmu-1.2.1.tar.xz"
xbuild "$XR/lib/libXcursor-1.2.3.tar.xz"
xbuild "$XR/lib/libxkbfile-1.1.3.tar.xz"

echo "===== fonts + rendering deps ====="
xbuild "https://zlib.net/fossils/zlib-1.3.1.tar.gz"
xbuild "https://downloads.sourceforge.net/libpng/libpng-1.6.43.tar.xz"
xbuild "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz" --enable-freetype-config
xbuild "https://github.com/libexpat/libexpat/releases/download/R_2_6_3/expat-2.6.3.tar.xz"
xbuild "https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.xz" --disable-docs
xbuild "$XR/lib/libXft-2.3.8.tar.xz"
xbuild "https://www.cairographics.org/releases/pixman-0.43.4.tar.gz"
xbuild "$XR/lib/libfontenc-1.1.8.tar.xz"
xbuild "$XR/lib/libXfont2-2.0.7.tar.xz"
xbuild "$XR/font/font-util-1.4.1.tar.xz"
xbuild "$XR/app/mkfontscale-1.2.3.tar.xz"
xbuild "$XR/app/bdftopcf-1.1.1.tar.xz"
xbuild "$XR/font/font-misc-misc-1.1.3.tar.xz"

echo "===== keyboard ====="
xbuild "$XR/app/xkbcomp-1.4.7.tar.xz"
xbuild "https://www.x.org/releases/individual/data/xkeyboard-config/xkeyboard-config-2.41.tar.xz"

echo "===== X server (fbdev) + input/video drivers ====="
xbuild "$XR/xserver/xorg-server-21.1.13.tar.xz" \
  --disable-glx --disable-dri --disable-dri2 --disable-dri3 --disable-glamor \
  --disable-xvfb --disable-xnest --disable-xephyr --disable-dmx --disable-xwin \
  --disable-docs --disable-devel-docs --disable-unit-tests \
  --disable-systemd-logind --disable-libunwind --without-dtrace \
  --enable-xorg --with-fontrootdir="$FONTROOTDIR"
xbuild "$XR/driver/xf86-video-fbdev-0.5.0.tar.xz"
xbuild "$XR/driver/xf86-input-keyboard-2.0.0.tar.xz"
xbuild "$XR/driver/xf86-input-mouse-1.9.5.tar.xz"

echo "===== X apps + WM + terminal ====="
xbuild "$XR/app/xauth-1.1.3.tar.xz"
xbuild "$XR/app/xinit-1.4.4.tar.xz"
xbuild "$XR/app/xsetroot-1.1.3.tar.xz"
xbuild "$XR/app/xclock-1.1.1.tar.xz"
xbuild "$XR/app/twm-1.0.12.tar.xz"
xbuild "https://invisible-island.net/datafiles/release/ncurses.tar.gz"
xbuild "$XR/app/xterm-393.tgz" --disable-freetype
