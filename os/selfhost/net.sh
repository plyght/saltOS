set -euxo pipefail

OPENSSL_VER="3.4.0"
LIBNL_VER="3.11.0"
WPA_VER="2.11"
CURL_VER="8.11.1"

export PKG_CONFIG_PATH="$N/usr/lib/pkgconfig:$N/usr/lib64/pkgconfig:$N/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$N"
export CPPFLAGS="-I$N/usr/include"
export LDFLAGS="-L$N/usr/lib -L$N/usr/lib64 -Wl,-rpath-link,$N/usr/lib -Wl,-rpath-link,$N/usr/lib64"
export PATH="$N/usr/bin:$PATH"

mkdir -p "$N/.salt-done"

netdone() { [ -f "$N/.salt-done/$1" ]; }
netmark() { mkdir -p "$N/.salt-done"; touch "$N/.salt-done/$1"; }

cd "$SRC"

if ! netdone openssl; then
  echo "===== openssl ====="
  [ -f "openssl-${OPENSSL_VER}.tar.gz" ] || fetch \
    "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz" \
    "openssl-${OPENSSL_VER}.tar.gz"
  rm -rf "openssl-${OPENSSL_VER}"
  tar -xf "openssl-${OPENSSL_VER}.tar.gz"
  ( cd "openssl-${OPENSSL_VER}"
    ./Configure linux-x86_64 --prefix=/usr --libdir=lib --openssldir=/etc/ssl shared no-docs
    make -j"$JOBS"
    make DESTDIR="$N" install_sw install_ssldirs )
  netmark openssl
fi

if ! netdone libnl; then
  echo "===== libnl ====="
  [ -f "libnl-${LIBNL_VER}.tar.gz" ] || fetch \
    "https://github.com/thom311/libnl/releases/download/libnl$(echo "$LIBNL_VER" | tr . _)/libnl-${LIBNL_VER}.tar.gz" \
    "libnl-${LIBNL_VER}.tar.gz"
  rm -rf "libnl-${LIBNL_VER}"
  tar -xf "libnl-${LIBNL_VER}.tar.gz"
  ( cd "libnl-${LIBNL_VER}"
    ./configure --prefix=/usr --libdir=/usr/lib --disable-static --sysconfdir=/etc
    make -j"$JOBS"
    make DESTDIR="$N" install )
  netmark libnl
fi

if ! netdone curl; then
  echo "===== curl ====="
  [ -f "curl-${CURL_VER}.tar.xz" ] || fetch \
    "https://curl.se/download/curl-${CURL_VER}.tar.xz" "curl-${CURL_VER}.tar.xz"
  rm -rf "curl-${CURL_VER}"
  tar -xf "curl-${CURL_VER}.tar.xz"
  ( cd "curl-${CURL_VER}"
    ./configure --prefix=/usr --libdir=/usr/lib \
      --with-openssl="$N/usr" \
      --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
      --disable-ldap --disable-ldaps --disable-manual --without-libpsl
    make -j"$JOBS"
    make DESTDIR="$N" install )
  netmark curl
fi

if ! netdone wpa_supplicant; then
  echo "===== wpa_supplicant ====="
  [ -f "wpa_supplicant-${WPA_VER}.tar.gz" ] || fetch \
    "https://w1.fi/releases/wpa_supplicant-${WPA_VER}.tar.gz" "wpa_supplicant-${WPA_VER}.tar.gz"
  rm -rf "wpa_supplicant-${WPA_VER}"
  tar -xf "wpa_supplicant-${WPA_VER}.tar.gz"
  ( cd "wpa_supplicant-${WPA_VER}/wpa_supplicant"
    cat > .config <<'WPAEOF'
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL32=y
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_IEEE80211W=y
CONFIG_WPS=y
CONFIG_SAE=y
CONFIG_OWE=y
CONFIG_DPP=y
CONFIG_TLS=openssl
CONFIG_EAP_TLS=y
CONFIG_EAP_PEAP=y
CONFIG_EAP_TTLS=y
CONFIG_EAP_MD5=y
CONFIG_EAP_MSCHAPV2=y
CONFIG_EAP_GTC=y
CONFIG_EAP_LEAP=y
CONFIG_EAP_PWD=y
CONFIG_PKCS12=y
CONFIG_SMARTCARD=y
CONFIG_AP=y
CONFIG_P2P=y
CONFIG_MESH=y
CONFIG_DEBUG_FILE=y
WPAEOF
    # .config is included as a makefile, so append with += here. Passing CFLAGS=
    # on the make command line would override the tree's own -I../src include
    # paths and the build cannot then find includes.h.
    cat >> .config <<EOF
CFLAGS += -I$N/usr/include -I$N/usr/include/libnl3
LIBS += -L$N/usr/lib -L$N/usr/lib64 -Wl,-rpath-link,$N/usr/lib
LIBS_p += -L$N/usr/lib -L$N/usr/lib64
LIBS_c += -L$N/usr/lib -L$N/usr/lib64
EOF
    make -j"$JOBS"
    install -Dm755 wpa_supplicant "$N/usr/sbin/wpa_supplicant"
    install -Dm755 wpa_cli "$N/usr/sbin/wpa_cli"
    install -Dm755 wpa_passphrase "$N/usr/sbin/wpa_passphrase" )
  netmark wpa_supplicant
fi

if ! netdone cacerts; then
  echo "===== ca-certificates ====="
  fetch "https://curl.se/ca/cacert.pem" "$SRC/cacert.pem"
  install -Dm644 "$SRC/cacert.pem" "$N/etc/ssl/certs/ca-certificates.crt"
  install -Dm644 "$SRC/cacert.pem" "$N/etc/ssl/cert.pem"
  netmark cacerts
fi

find "$N" -name '*.la' -delete 2>/dev/null || true
