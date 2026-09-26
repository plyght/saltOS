local version = "9.8p1"

return {
  name = "openssh",
  version = version,
  release = 3,
  summary = "Secure shell client and server",
  license = "BSD-2-Clause",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-" .. version .. ".tar.gz",
    sha256 = "dd8bd002a379b5d499dfb050dd1fa9af8029e80461f4bb6c523c49973f5a39f3",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "openssl", "zlib", "libxcrypt" },
    script = [[
#!/bin/sh
./configure --prefix=/usr \
    --sysconfdir=/etc/ssh \
    --with-privsep-path=/var/lib/sshd \
    --with-ssl-engine \
    --with-zlib
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install-nokeys
]],
  },
  package = {
    deps = { "glibc", "openssl", "zlib", "libxcrypt", "shadow" },
  },
  hooks = {
    post_install = [[
getent group sshd >/dev/null || groupadd -r sshd
getent passwd sshd >/dev/null || useradd -r -g sshd -d /var/lib/sshd -s /sbin/nologin -c "sshd privilege separation" sshd
mkdir -p /var/lib/sshd
chmod 0755 /var/lib/sshd
]],
  },
  reproducibility = {
    status = "verified",
  },
}
