local version = "4.16.0"

return {
  name = "shadow",
  version = version,
  release = 1,
  summary = "Password and account management utilities",
  license = "BSD-3-Clause",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/shadow-maint/shadow/releases/download/" .. version .. "/shadow-" .. version .. ".tar.xz",
    sha256 = "b78e3921a95d53282a38e90628880624736bf6235e36eea50c50835f59a3530b",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "libxcrypt" },
    script = [[
#!/bin/sh
sed -i 's/groups$(EXEEXT) //' src/Makefile.in
find man -name Makefile.in -exec sed -i 's/groups\.1 / /' {} \;
./configure --sysconfdir=/etc \
    --prefix=/usr \
    --disable-static \
    --without-libpam \
    --without-libbsd
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "libxcrypt" },
  },
  reproducibility = {
    status = "verified",
  },
}
