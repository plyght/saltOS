#!/bin/bash
set -euo pipefail

ARCH="${1:-x86_64}"
WORK="${WORK:-$PWD/omakase-work}"
OUT="${OUT:-$PWD/out-omakase}"
REPO="${REPO_DIR:-$PWD}"
VERSION="${VERSION:-0.1.0}"
MIRROR_DIR="${MIRROR_DIR:-$OUT/mirror/arch}"
VENDOR_DIR="${VENDOR_DIR:-$OUT/vendor}"
WALLPAPER_DIR="${WALLPAPER_DIR:-$OUT/wallpapers}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

case "$ARCH" in
  x86_64) SERIAL=ttyS0 ;;
  aarch64) SERIAL=ttyAMA0 ;;
  *) echo "iso: unsupported arch $ARCH" >&2; exit 1 ;;
esac

[ -x "$VENDOR_DIR/gum-$ARCH" ] || { echo "iso: run os/omakase/build/vendor.sh $ARCH first (missing $VENDOR_DIR/gum-$ARCH)" >&2; exit 1; }
[ -d "$VENDOR_DIR/vicinae-$ARCH" ] || { echo "iso: missing $VENDOR_DIR/vicinae-$ARCH" >&2; exit 1; }
[ -d "$VENDOR_DIR/helium-$ARCH" ] || { echo "iso: missing $VENDOR_DIR/helium-$ARCH" >&2; exit 1; }
[ -f "$WALLPAPER_DIR/CREDITS" ] || { echo "iso: run os/omakase/build/wallpapers.sh first (missing $WALLPAPER_DIR/CREDITS)" >&2; exit 1; }
if [ "${OMAKASE_OFFLINE:-1}" = 1 ]; then
  [ -f "$MIRROR_DIR/offline.db" ] || { echo "iso: run os/omakase/build/arch-mirror.sh $ARCH first (missing $MIRROR_DIR/offline.db)" >&2; exit 1; }
  [ -s "$MIRROR_DIR/bootstrap.sha256" ] || { echo "iso: missing $MIRROR_DIR/bootstrap.sha256" >&2; exit 1; }
fi

mkdir -p "$OUT"
if [ "${OMAKASE_REUSE_ROOTFS:-0}" = 1 ] && [ -x "$WORK/rootfs/usr/bin/salt" ]; then
  echo "==> reusing base rootfs in $WORK/rootfs"
  install -Dm755 "${SALT_BIN:-$REPO/build/src/salt/salt}" "$WORK/rootfs/usr/bin/salt"
  install -Dm755 "${SALTSETUP_BIN:-$REPO/build/src/setup/salt-setup}" "$WORK/rootfs/usr/bin/salt-setup"
else
  env EDITION=base WORK="$WORK" OUT="$WORK/base-out" REPO_DIR="$REPO" VERSION="$VERSION" \
    SALT_BIN="${SALT_BIN:-$REPO/build/src/salt/salt}" SALTSETUP_BIN="${SALTSETUP_BIN:-$REPO/build/src/setup/salt-setup}" \
    bash "$REPO/os/iso/live-build.sh" "$ARCH"
fi

ROOTFS="$WORK/rootfs"
ISODIR="$WORK/iso"
rm -f "$WORK/base-out"/*.iso

echo "==> adding omakase host packages"
saved_resolv=""
if [ -e "$ROOTFS/etc/resolv.conf" ] || [ -L "$ROOTFS/etc/resolv.conf" ]; then
  saved_resolv="$WORK/resolv.conf.saved"
  cp -a "$ROOTFS/etc/resolv.conf" "$saved_resolv"
  rm -f "$ROOTFS/etc/resolv.conf"
fi
cp -L /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
mount -t proc proc "$ROOTFS/proc"
chroot "$ROOTFS" sh -c 'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && apt-get install -y --no-install-recommends \
  jq console-data cryptsetup cryptsetup-initramfs seatd bluez polkitd rfkill fonts-terminus kbd \
  && apt-get clean && rm -rf /var/lib/apt/lists/*'
umount "$ROOTFS/proc"
rm -f "$ROOTFS/etc/resolv.conf"
[ -n "$saved_resolv" ] && cp -a "$saved_resolv" "$ROOTFS/etc/resolv.conf"

SHARE="$ROOTFS/usr/share/saltos-omakase"
rm -rf "$SHARE"
mkdir -p "$SHARE" "$ROOTFS/usr/local/bin"
for d in lib live packages target themes; do
  cp -a "$HERE/$d" "$SHARE/$d"
done
chmod 0755 "$SHARE"/live/saltos-* "$SHARE"/target/bin/* "$SHARE"/target/sv/*/run
chown -R 0:0 "$SHARE"
for f in "$SHARE"/live/saltos-*; do
  ln -sf "/usr/share/saltos-omakase/live/$(basename "$f")" "$ROOTFS/usr/local/bin/$(basename "$f")"
done
install -m 0755 "$VENDOR_DIR/gum-$ARCH" "$ROOTFS/usr/local/bin/gum"

ARCH_RECIPE="$REPO/strata/arch.toml"
[ -f "$REPO/strata/arch-$ARCH.toml" ] && ARCH_RECIPE="$REPO/strata/arch-$ARCH.toml"
mkdir -p "$SHARE/strata"
install -m 0644 "$ARCH_RECIPE" "$SHARE/strata/arch.toml"
install -m 0644 "$ARCH_RECIPE" "$ROOTFS/etc/salt/strata/arch.toml"
if [ -f "$MIRROR_DIR/offline.db" ]; then
  echo "==> pinning the arch recipe to the offline bootstrap"
  read -r BOOTSTRAP_SHA BOOTSTRAP_FILE <"$MIRROR_DIR/bootstrap.sha256"
  [ -f "$MIRROR_DIR/$BOOTSTRAP_FILE" ] || { echo "iso: bootstrap $BOOTSTRAP_FILE missing from $MIRROR_DIR" >&2; exit 1; }
  awk -v url="file:///run/saltos-install/offline/$BOOTSTRAP_FILE" -v sha="$BOOTSTRAP_SHA" '
    /^\[/ { section = $0 }
    section == "[bootstrap]" && /^url = / { print "url = \"" url "\""; next }
    section == "[bootstrap]" && /^sha256 = / { print "sha256 = \"" sha "\""; next }
    { print }' "$ARCH_RECIPE" >"$ROOTFS/etc/salt/strata/arch.toml"
  grep -q "^sha256 = \"$BOOTSTRAP_SHA\"" "$ROOTFS/etc/salt/strata/arch.toml" || { echo "iso: recipe pinning failed" >&2; exit 1; }
fi

cat >"$ROOTFS/etc/os-release" <<EOF
NAME="saltOS"
PRETTY_NAME="saltOS $VERSION omakase (live)"
ID=saltos
ID_LIKE=debian
VERSION="$VERSION"
VERSION_ID="$VERSION"
VARIANT="omakase"
VARIANT_ID=omakase
HOME_URL="https://github.com/plyght/saltOS"
EOF

rm -f "$ROOTFS/home/salt/.bash_profile" "$ROOTFS/etc/runit/runsvdir/current/agetty-tty1"
mkdir -p "$ROOTFS/etc/runit/sv/saltos-live-install"
cat >"$ROOTFS/etc/runit/sv/saltos-live-install/run" <<'EOF'
#!/bin/sh
exec 2>&1
sleep 1
exec setsid -c env TERM=linux HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  /usr/local/bin/saltos-live-install </dev/tty1 >/dev/tty1 2>&1
EOF
chmod 0755 "$ROOTFS/etc/runit/sv/saltos-live-install/run"
ln -sf /etc/runit/sv/saltos-live-install "$ROOTFS/etc/runit/runsvdir/current/saltos-live-install"
install -Dm755 "$HERE/target/sv/udevd/run" "$ROOTFS/etc/runit/sv/udevd/run"
ln -sf /etc/runit/sv/udevd "$ROOTFS/etc/runit/runsvdir/current/udevd"

cat >"$ROOTFS/etc/issue" <<'EOF'

saltOS omakase \r (\l)

EOF

echo "==> staging offline mirror, vendor bundles and wallpapers"
rm -rf "$ISODIR/omakase"
mkdir -p "$ISODIR/omakase/vendor"
cp -a "$WALLPAPER_DIR" "$ISODIR/omakase/wallpapers"
if [ -f "$MIRROR_DIR/offline.db" ]; then
  mkdir -p "$ISODIR/omakase/mirror"
  cp -a "$MIRROR_DIR" "$ISODIR/omakase/mirror/arch"
fi
cp -a "$VENDOR_DIR/vicinae-$ARCH" "$VENDOR_DIR/helium-$ARCH" "$ISODIR/omakase/vendor/"
install -m 0755 "$VENDOR_DIR/gum-$ARCH" "$ISODIR/omakase/vendor/gum-$ARCH"

echo "==> rebuilding squashfs"
rm -f "$ISODIR/live/filesystem.squashfs"
mksquashfs "$ROOTFS" "$ISODIR/live/filesystem.squashfs" \
  -comp zstd -Xcompression-level 15 -noappend

cat >"$ISODIR/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=3
insmod all_video
menuentry "saltOS $VERSION omakase (install)" {
  linux /live/vmlinuz boot=live components init=/sbin/runit-init quiet console=tty0 console=$SERIAL,115200
  initrd /live/initrd
}
menuentry "saltOS $VERSION omakase (install, to RAM)" {
  linux /live/vmlinuz boot=live components toram init=/sbin/runit-init quiet console=tty0 console=$SERIAL,115200
  initrd /live/initrd
}
menuentry "saltOS $VERSION omakase (install, safe graphics)" {
  linux /live/vmlinuz boot=live components init=/sbin/runit-init nomodeset console=tty0 console=$SERIAL,115200
  initrd /live/initrd
}
EOF

ISO_PATH="$OUT/saltos-omakase-$ARCH.iso"
MKRESCUE_OPTS=()
if [ "$ARCH" = aarch64 ]; then
  if [ -f /usr/lib/grub/arm64-efi/modinfo.sh ]; then
    MKRESCUE_OPTS=(-d /usr/lib/grub/arm64-efi)
  elif [ -f "$ROOTFS/usr/lib/grub/arm64-efi/modinfo.sh" ]; then
    MKRESCUE_OPTS=(-d "$ROOTFS/usr/lib/grub/arm64-efi")
  else
    echo "iso: no arm64-efi GRUB modules on the host or in the rootfs; install grub-efi-arm64-bin" >&2
    exit 1
  fi
fi
grub-mkrescue "${MKRESCUE_OPTS[@]}" -o "$ISO_PATH" "$ISODIR" -- -volid "SALTOS_OMAKASE"
du -h "$ISO_PATH"
echo "wrote $ISO_PATH"
