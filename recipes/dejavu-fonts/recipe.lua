local version = "2.37"

return {
  name = "dejavu-fonts",
  version = version,
  release = 1,
  summary = "DejaVu TrueType fonts",
  license = "Bitstream-Vera",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://downloads.sourceforge.net/dejavu/dejavu-fonts-ttf-" .. version .. ".tar.bz2",
    sha256 = "fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7",
  },
  build = {
    system = "custom",
    deps = { "coreutils" },
    script = [[
#!/bin/sh
install -d "$SALT_DEST/usr/share/fonts/dejavu" "$SALT_DEST/etc/fonts/conf.d"
install -m644 ttf/*.ttf "$SALT_DEST/usr/share/fonts/dejavu/"
install -m644 fontconfig/*.conf "$SALT_DEST/etc/fonts/conf.d/"
]],
  },
  package = {
    deps = {},
  },
  reproducibility = {
    status = "verified",
  },
}
