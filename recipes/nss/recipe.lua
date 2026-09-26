local version = "3.101"

return {
  name = "nss",
  version = version,
  release = 2,
  summary = "Network Security Services libraries",
  license = "MPL-2.0",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://archive.mozilla.org/pub/security/nss/releases/NSS_3_101_RTM/src/nss-" .. version .. ".tar.gz",
    sha256 = "859748f0b4b7bb51e7e600ae5a88ef4d71f93e6964b1beed2727784dd9ed85e7",
  },
  build = {
    system = "custom",
    deps = { "gcc", "make", "nspr", "perl", "pkgconf", "sqlite", "zlib" },
    script = [[
#!/bin/sh
set -e
# Build against the system nspr: the plain nss tarball has no nspr/ tree, so
# the nss_build_all target (which builds nspr first) cannot be used.
cd nss
make BUILD_OPT=1 USE_64=1 NSPR_INCLUDE_DIR=/usr/include/nspr NSPR_LIB_DIR=/usr/lib \
  USE_SYSTEM_ZLIB=1 ZLIB_LIBS=-lz NSS_USE_SYSTEM_SQLITE=1 NSS_ENABLE_WERROR=0 all
cd ../dist
obj=$(ls -d Linux*_OPT.OBJ | head -1)
install -d "$SALT_DEST/usr/lib/pkgconfig" "$SALT_DEST/usr/include/nss" "$SALT_DEST/usr/bin"
for f in "$obj"/lib/*.so; do
  case "${f##*/}" in libnssckbi-testlib.so|libpkcs11testmodule.so) continue ;; esac
  install -m755 "$f" "$SALT_DEST/usr/lib/"
done
install -m644 "$obj"/lib/*.chk "$obj"/lib/libcrmf.a "$SALT_DEST/usr/lib/"
cp -RL public/nss/* private/nss/* "$SALT_DEST/usr/include/nss/"
install -m755 "$obj"/bin/certutil "$obj"/bin/pk12util "$SALT_DEST/usr/bin/"
# The make build does not generate nss.pc / nss-config; write them.
nspr_ver=$(pkg-config --modversion nspr)
nss_ver=$(sed -n 's/^#define NSS_VERSION "\([0-9.]*\).*/\1/p' public/nss/nss.h)
cat > "$SALT_DEST/usr/lib/pkgconfig/nss.pc" <<PC
prefix=/usr
exec_prefix=/usr
libdir=/usr/lib
includedir=/usr/include/nss

Name: NSS
Description: Network Security Services
Version: $nss_ver
Requires: nspr >= $nspr_ver
Libs: -L\${libdir} -lssl3 -lsmime3 -lnss3 -lnssutil3
Cflags: -I\${includedir}
PC
cat > "$SALT_DEST/usr/bin/nss-config" <<'SH'
#!/bin/sh
for a in "$@"; do
  case "$a" in
    --version) pkg-config --modversion nss ;;
    --libs) pkg-config --libs nss ;;
    --cflags) pkg-config --cflags nss ;;
    --prefix|--exec-prefix) echo /usr ;;
    --includedir) echo /usr/include/nss ;;
    --libdir) echo /usr/lib ;;
  esac
done
SH
chmod 755 "$SALT_DEST/usr/bin/nss-config"
]],
  },
  package = {
    deps = { "glibc", "nspr", "sqlite", "zlib" },
  },
  reproducibility = {
    status = "verified",
  },
}
