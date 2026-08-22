set -euxo pipefail

ALSA_VER="1.2.12"

export PKG_CONFIG_PATH="$A/usr/lib/pkgconfig:$A/usr/share/pkgconfig"
export CPPFLAGS="-I$A/usr/include"
export LDFLAGS="-L$A/usr/lib -Wl,-rpath-link,$A/usr/lib"
export PATH="$A/usr/bin:$PATH"

mkdir -p "$A/.salt-done"
audiodone() { [ -f "$A/.salt-done/$1" ]; }
audiomark() { mkdir -p "$A/.salt-done"; touch "$A/.salt-done/$1"; }

cd "$SRC"

if ! audiodone alsa-lib; then
  echo "===== alsa-lib ====="
  [ -f "alsa-lib-${ALSA_VER}.tar.bz2" ] || fetch \
    "https://www.alsa-project.org/files/pub/lib/alsa-lib-${ALSA_VER}.tar.bz2" \
    "alsa-lib-${ALSA_VER}.tar.bz2"
  rm -rf "alsa-lib-${ALSA_VER}"
  tar -xf "alsa-lib-${ALSA_VER}.tar.bz2"
  ( cd "alsa-lib-${ALSA_VER}"
    ./configure --prefix=/usr --libdir=/usr/lib --sysconfdir=/etc --disable-python
    make -j"$JOBS"
    make DESTDIR="$A" install )
  audiomark alsa-lib
fi

if ! audiodone alsa-utils; then
  echo "===== alsa-utils ====="
  [ -f "alsa-utils-${ALSA_VER}.tar.bz2" ] || fetch \
    "https://www.alsa-project.org/files/pub/utils/alsa-utils-${ALSA_VER}.tar.bz2" \
    "alsa-utils-${ALSA_VER}.tar.bz2"
  rm -rf "alsa-utils-${ALSA_VER}"
  tar -xf "alsa-utils-${ALSA_VER}.tar.bz2"
  ( cd "alsa-utils-${ALSA_VER}"
    ./configure --prefix=/usr --libdir=/usr/lib --sysconfdir=/etc \
      --with-alsa-prefix="$A/usr/lib" --with-alsa-inc-prefix="$A/usr/include" \
      --disable-alsamixer --disable-xmlto --disable-nls --disable-rst2man \
      --disable-alsaconf --disable-bat
    make -j"$JOBS"
    make DESTDIR="$A" install )
  audiomark alsa-utils
fi

find "$A" -name '*.la' -delete 2>/dev/null || true
