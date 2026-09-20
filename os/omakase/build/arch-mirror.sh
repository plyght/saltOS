#!/bin/bash
set -euo pipefail

ARCH="${1:-x86_64}"
OUT="${OUT:-$PWD/out-omakase/mirror/arch}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

case "$ARCH" in
  x86_64)
    BOOTSTRAP_URL="${ARCH_BOOTSTRAP_URL:-https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst}"
    BOOTSTRAP_FILE="archlinux-bootstrap-x86_64.tar.zst"
    PACMAN_ARCH=auto
    PACMAN_REPOS=""
    KEYRING="pacman-key --init >/dev/null 2>&1; pacman-key --populate archlinux >/dev/null 2>&1; pacman -Sy --noconfirm archlinux-keyring >/dev/null"
    IMAGE=archlinux:latest
    PLATFORM=linux/amd64
    ;;
  aarch64)
    BOOTSTRAP_URL="${ARCH_BOOTSTRAP_URL:-http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz}"
    BOOTSTRAP_FILE="ArchLinuxARM-aarch64-latest.tar.gz"
    PACMAN_ARCH=aarch64
    PACMAN_REPOS="$(for r in core extra alarm aur; do printf '[%s]\nServer = http://mirror.archlinuxarm.org/$arch/$repo\n' "$r"; done)"
    KEYRING=":"
    IMAGE=saltos-omakase-alarm:latest
    PLATFORM=linux/arm64
    ;;
  *) echo "arch-mirror: unsupported arch $ARCH" >&2; exit 1 ;;
esac
command -v docker >/dev/null || { echo "arch-mirror: docker is required" >&2; exit 1; }

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

pkgs="$(tail -n +2 "$HERE/packages/packages.tsv" | cut -f2 | tr ' ' '\n' | grep -v '^-$' | grep -v '^$' | sort -u | tr '\n' ' ')"

echo "==> fetching Arch bootstrap rootfs"
curl -fsSL "$BOOTSTRAP_URL" -o "$OUT/$BOOTSTRAP_FILE"
case "$ARCH" in
  x86_64)
    curl -fsSL "${BOOTSTRAP_URL%/*}/sha256sums.txt" | grep "$BOOTSTRAP_FILE\$" | (cd "$OUT" && sha256sum -c -)
    ;;
  aarch64)
    curl -fsSL "$BOOTSTRAP_URL.md5" | sed "s|  .*|  $BOOTSTRAP_FILE|" | (cd "$OUT" && md5sum -c -)
    echo "==> importing the Arch Linux ARM rootfs as $IMAGE"
    docker import --platform "$PLATFORM" "$OUT/$BOOTSTRAP_FILE" "$IMAGE" >/dev/null
    ;;
esac
(cd "$OUT" && sha256sum "$BOOTSTRAP_FILE" >bootstrap.sha256)

echo "==> downloading Arch ($ARCH) packages into $OUT"
docker run --rm --platform "$PLATFORM" -v "$OUT:/mirror" -e "PACMAN_REPOS=$PACMAN_REPOS" "$IMAGE" bash -euo pipefail -c "
  $KEYRING
  mkdir -p /tmp/empty /tmp/db
  if [ -n \"\$PACMAN_REPOS\" ]; then
    printf '[options]\nArchitecture = $PACMAN_ARCH\nSigLevel = Never\n%s\n' \"\$PACMAN_REPOS\" >/tmp/upstream.conf
  else
    cp /etc/pacman.conf /tmp/upstream.conf
  fi
  pacman -Syw --noconfirm --config /tmp/upstream.conf --dbpath /tmp/empty --cachedir /mirror base linux-firmware-whence $pkgs
  rm -f /mirror/*.sig
  cd /mirror
  rm -f offline.db offline.db.tar.gz offline.files offline.files.tar.gz
  repo-add -q offline.db.tar.gz ./*.pkg.tar.*
  printf '[options]\nArchitecture = $PACMAN_ARCH\nSigLevel = Never\n[offline]\nServer = file:///mirror\n' >/tmp/offline.conf
  pacman -Sy --noconfirm --dbpath /tmp/db --config /tmp/offline.conf >/dev/null
  pacman -Sp --noconfirm --dbpath /tmp/db --config /tmp/offline.conf base linux-firmware-whence $pkgs >/dev/null
"
rm -f "$OUT"/*.part

ls -1 "$OUT" | wc -l | xargs printf '==> %s files, '
du -sh "$OUT" | cut -f1
