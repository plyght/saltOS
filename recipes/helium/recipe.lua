local version = "0.13.5.1"
local appimage = {
  x86_64 = { file = "x86_64", sha256 = "5409dc2fbf27c974513543d9d9cd10b9dc45adfc72eed5c4d8d14de2d79e7b39" },
  aarch64 = { file = "arm64", sha256 = "c449988e9523adb552cf7f5094beff7c9391bca5a75e435751790f02512fc048" },
}
local src = appimage[salt.arch] or appimage.x86_64

return {
  name = "helium",
  version = version,
  release = 2,
  summary = "Helium privacy-focused Chromium-derived web browser (prebuilt)",
  license = "BSD-3-Clause",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://github.com/imputnet/helium-linux/releases/download/" .. version .. "/helium-" .. version .. "-" .. src.file .. ".AppImage",
    sha256 = src.sha256,
  },
  build = {
    system = "custom",
    deps = { "coreutils", "tar", "zstd" },
    script = [[
#!/bin/sh
set -e
appimage=$(ls "$SALT_SRC"/helium-*.AppImage | head -n1)
chmod +x "$appimage"
"$appimage" --appimage-extract
install -d "$SALT_DEST/opt/helium"
cp -a squashfs-root/. "$SALT_DEST/opt/helium/"
install -d "$SALT_DEST/usr/bin"
ln -sf /opt/helium/AppRun "$SALT_DEST/usr/bin/helium"
install -d "$SALT_DEST/usr/share/applications"
if [ -f "$SALT_DEST/opt/helium/helium.desktop" ]; then
  cp "$SALT_DEST/opt/helium/helium.desktop" "$SALT_DEST/usr/share/applications/helium.desktop"
fi
]],
  },
  package = {
    deps = {
      "glibc",
      "glib",
      "nspr",
      "nss",
      "dbus",
      "expat",
      "at-spi2-core",
      "libcups",
      "cairo",
      "pango",
      "mesa",
      "fontconfig",
      "freetype",
      "alsa-lib",
      "eudev",
      "libx11",
      "libxext",
      "libxcb",
      "libxcomposite",
      "libxdamage",
      "libxfixes",
      "libxrandr",
      "libxkbcommon",
    },
  },
  reproducibility = {
    status = "unverified",
    reason = "Chromium-derived browser build currently not bit-for-bit reproducible",
  },
}
