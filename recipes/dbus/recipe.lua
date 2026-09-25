local version = "1.14.10"

return {
  name = "dbus",
  version = version,
  release = 3,
  summary = "Message bus system for interprocess communication",
  license = "GPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://dbus.freedesktop.org/releases/dbus/dbus-" .. version .. ".tar.xz",
    sha256 = "ba1f21d2bd9d339da2d4aa8780c09df32fea87998b73da24f49ab9df1e36a50f",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "pkgconf", "expat" },
    script = [[
#!/bin/sh
./configure --prefix=/usr \
    --sysconfdir=/etc \
    --localstatedir=/var \
    --runstatedir=/run \
    --disable-static \
    --disable-doxygen-docs \
    --disable-xml-docs \
    --disable-systemd \
    --without-x \
    --with-system-socket=/run/dbus/system_bus_socket \
    --with-system-pid-file=/run/dbus/pid
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "expat", "shadow" },
  },
  hooks = {
    post_install = [[
getent group messagebus >/dev/null || groupadd -r messagebus
getent passwd messagebus >/dev/null || useradd -r -g messagebus -d /run/dbus -s /sbin/nologin -c "D-Bus system bus" messagebus
]],
  },
  reproducibility = {
    status = "verified",
  },
}
