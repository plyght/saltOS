local version = "2.1.2"

return {
  name = "runit",
  version = version,
  release = 3,
  summary = "UNIX init scheme with service supervision",
  license = "BSD-3-Clause",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "http://smarden.org/runit/runit-" .. version .. ".tar.gz",
    sha256 = "6fd0160cb0cf1207de4e66754b6d39750cff14bb0aa66ab49490992c0c47ba18",
  },
  build = {
    system = "custom",
    deps = { "gcc", "make" },
    script = [[
#!/bin/sh
cd "$(find . -maxdepth 2 -type d -name runit-2.1.2 | head -n 1)"
sed -i 's/ -static$//' src/Makefile
echo "gcc -O2 -Wall -Wno-parentheses -Wno-incompatible-pointer-types -Wno-implicit-function-declaration -D_GNU_SOURCE" > src/conf-cc
echo "gcc -Wl,-z,relro" > src/conf-ld
cd src
make -j"$SALT_JOBS"
make check
cd ..
install -d "$SALT_DEST/usr/bin"
for bin in runit runit-init runsv runsvchdir runsvdir sv svlogd chpst utmpset; do
    install -m 0755 "src/$bin" "$SALT_DEST/usr/bin/$bin"
done
install -d "$SALT_DEST/usr/sbin"
ln -sf ../bin/runit-init "$SALT_DEST/usr/sbin/init"
ln -sf ../bin/runit "$SALT_DEST/usr/sbin/runit"
install -d "$SALT_DEST/etc/runit"
install -d "$SALT_DEST/etc/runit/runsvdir/default"
install -d "$SALT_DEST/var/service"
]],
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
