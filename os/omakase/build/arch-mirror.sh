#!/bin/bash
set -euo pipefail

ARCH="${1:-x86_64}"
OUT="${OUT:-$PWD/out-omakase/mirror/arch}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BOOTSTRAP_URL="${ARCH_BOOTSTRAP_URL:-https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst}"

[ "$ARCH" = x86_64 ] || { echo "arch-mirror: the Arch stratum is x86_64 only (got $ARCH)" >&2; exit 1; }
command -v docker >/dev/null || { echo "arch-mirror: docker is required" >&2; exit 1; }

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

pkgs="$(tail -n +2 "$HERE/packages/packages.tsv" | cut -f2 | tr ' ' '\n' | grep -v '^-$' | grep -v '^$' | sort -u | tr '\n' ' ')"

echo "==> downloading Arch packages into $OUT"
docker run --rm -v "$OUT:/mirror" archlinux:latest bash -euo pipefail -c "
  pacman-key --init >/dev/null 2>&1
  pacman-key --populate archlinux >/dev/null 2>&1
  pacman -Sy --noconfirm archlinux-keyring >/dev/null
  mkdir -p /tmp/empty /tmp/db
  pacman -Syw --noconfirm --dbpath /tmp/empty --cachedir /mirror base linux-firmware-whence $pkgs
  rm -f /mirror/*.sig
  cd /mirror
  repo-add -q offline.db.tar.gz ./*.pkg.tar.zst
  pacman -Sy --noconfirm --dbpath /tmp/db --config /dev/stdin <<CONF >/dev/null
[options]
Architecture = auto
SigLevel = Never
[offline]
Server = file:///mirror
CONF
  pacman -Sp --noconfirm --dbpath /tmp/db --config /dev/stdin base linux-firmware-whence $pkgs <<CONF >/dev/null
[options]
Architecture = auto
SigLevel = Never
[offline]
Server = file:///mirror
CONF
"
rm -f "$OUT"/*.part

echo "==> fetching Arch bootstrap rootfs"
curl -fsSL "$BOOTSTRAP_URL" -o "$OUT/archlinux-bootstrap-x86_64.tar.zst"
curl -fsSL "${BOOTSTRAP_URL%/*}/sha256sums.txt" | grep 'archlinux-bootstrap-x86_64.tar.zst$' \
  | (cd "$OUT" && sha256sum -c -)

ls -1 "$OUT" | wc -l | xargs printf '==> %s files, '
du -sh "$OUT" | cut -f1
