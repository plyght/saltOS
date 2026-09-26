local version = "0.51.0"

return {
  name = "intltool",
  version = version,
  release = 1,
  summary = "Tools to extract translatable strings from XML-style files",
  license = "GPL-2.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://launchpad.net/intltool/trunk/" .. version .. "/+download/intltool-" .. version .. ".tar.gz",
    sha256 = "67c74d94196b153b774ab9f89b2fa6c6ba79352407037c8c14d5aeb334e959cd",
  },
  build = {
    system = "autotools",
    deps = { "make", "perl", "perl-xml-parser", "gettext" },
    script = [[
#!/bin/sh
set -e
# Unescaped left brace in regex: a warning since perl 5.22, fatal later.
sed -i 's:\\\${:\\\$\\{:' intltool-update.in
./configure --prefix=/usr
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "perl", "perl-xml-parser" },
  },
  reproducibility = {
    status = "verified",
  },
}
