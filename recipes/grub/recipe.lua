local version = "2.12"

return {
  name = "grub",
  version = version,
  release = 2,
  summary = "GRand Unified Bootloader with Btrfs snapshot boot support",
  license = "GPL-3.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://ftp.gnu.org/gnu/grub/grub-" .. version .. ".tar.xz",
    sha256 = "f3c97391f7c4eaa677a78e090c7e97e6dc47b16f655f04683ebd37bef7fe0faa",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "bison", "flex", "gawk", "python", "freetype", "fontconfig" },
    script = [[
#!/bin/sh
if [ "$SALT_ARCH" = "aarch64" ]; then
  platform=efi
  target=arm64-efi
else
  platform=efi
  target=x86_64-efi
fi
echo depends bli part_gpt > grub-core/extra_deps.lst
./configure --prefix=/usr --sysconfdir=/etc --disable-werror --with-platform="$platform"
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
]],
  },
  package = {
    deps = { "glibc", "freetype" },
  },
  reproducibility = {
    status = "unverified",
    reason = "embedded build timestamps and toolchain paths in generated boot images are not yet normalized",
  },
}
