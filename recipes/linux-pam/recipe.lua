local version = "1.6.1"

return {
  name = "linux-pam",
  version = version,
  release = 1,
  summary = "Pluggable Authentication Modules (used by SDDM; shadow's tools do not use PAM)",
  license = "BSD-3-Clause OR GPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/linux-pam/linux-pam/releases/download/v" .. version .. "/Linux-PAM-" .. version .. ".tar.xz",
    sha256 = "f8923c740159052d719dbfc2a2f81942d68dd34fcaf61c706a02c9b80feeef8e",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "flex", "libxcrypt" },
    script = [[
#!/bin/sh
set -e
./configure --prefix=/usr --sbindir=/usr/sbin --sysconfdir=/etc --libdir=/usr/lib \
  --enable-securedir=/usr/lib/security --disable-static --disable-doc --disable-regenerate-docu \
  --disable-nis --disable-selinux --disable-audit --disable-econf --disable-logind --disable-nls
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
# unix_chkpwd must be setuid so pam_unix can verify the calling user's own password.
chmod 4755 "$SALT_DEST/usr/sbin/unix_chkpwd"
install -d "$SALT_DEST/etc/pam.d"
install -m644 "$SALT_FILES/other" "$SALT_FILES/system-auth" "$SALT_DEST/etc/pam.d/"
]],
  },
  package = {
    deps = { "glibc", "libxcrypt" },
  },
  reproducibility = {
    status = "verified",
  },
}
