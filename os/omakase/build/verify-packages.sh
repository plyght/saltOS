#!/bin/bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TSV="${1:-$HERE/../packages/packages.tsv}"
ONLY="${2:-}"
ARCH="${VERIFY_ARCH:-x86_64}"

case "$ARCH" in
  x86_64) PLATFORM=linux/amd64 ;;
  aarch64) PLATFORM=linux/arm64 ;;
  *) echo "verify-packages: unsupported arch $ARCH" >&2; exit 1 ;;
esac

image_for() {
  case "$1" in
    arch) [ "$ARCH" = aarch64 ] && echo "saltos-omakase-alarm:latest" || echo "archlinux:latest" ;;
    debian) echo "debian:trixie" ;;
    fedora) echo "fedora:40" ;;
    void) echo "ghcr.io/void-linux/void-glibc:latest" ;;
    alpine) echo "alpine:3.20" ;;
    opensuse) echo "opensuse/leap:15.6" ;;
  esac
}

probe_script() {
  case "$1" in
    arch) cat <<'EOF'
if [ "$(uname -m)" = aarch64 ]; then
  { printf '[options]\nArchitecture = aarch64\nSigLevel = Never\n'
    for r in core extra alarm aur; do printf '[%s]\nServer = http://mirror.archlinuxarm.org/$arch/$repo\n' "$r"; done
  } >/tmp/pacman.conf
else
  cp /etc/pacman.conf /tmp/pacman.conf
fi
pacman -Sy --config /tmp/pacman.conf >/dev/null 2>&1
for p in "$@"; do pacman -Si --config /tmp/pacman.conf "$p" >/dev/null 2>&1 && echo "ok $p" || echo "missing $p"; done
EOF
      ;;
    debian) cat <<'EOF'
apt-get update >/dev/null 2>&1
for p in "$@"; do apt-cache show "$p" >/dev/null 2>&1 && echo "ok $p" || echo "missing $p"; done
EOF
      ;;
    fedora) cat <<'EOF'
dnf -q makecache >/dev/null 2>&1
for p in "$@"; do [ -n "$(dnf -q repoquery "$p" 2>/dev/null)" ] && echo "ok $p" || echo "missing $p"; done
EOF
      ;;
    void) cat <<'EOF'
xbps-install -S >/dev/null 2>&1
for p in "$@"; do xbps-query -R "$p" >/dev/null 2>&1 && echo "ok $p" || echo "missing $p"; done
EOF
      ;;
    alpine) cat <<'EOF'
apk update >/dev/null 2>&1
for p in "$@"; do [ -n "$(apk search -x "$p" 2>/dev/null)" ] && echo "ok $p" || echo "missing $p"; done
EOF
      ;;
    opensuse) cat <<'EOF'
zypper -n -q ref >/dev/null 2>&1
for p in "$@"; do zypper -n -q info "$p" 2>/dev/null | grep -q '^Name' && echo "ok $p" || echo "missing $p"; done
EOF
      ;;
  esac
}

column_of() {
  head -1 "$TSV" | tr '\t' '\n' | grep -nx "$1" | cut -d: -f1
}

packages_for() {
  local col
  col="$(column_of "$1")"
  tail -n +2 "$TSV" | cut -f"$col" | tr ' ' '\n' | grep -v '^-$' | grep -v '^$' | sort -u
}

rc=0
for distro in arch debian fedora void alpine opensuse; do
  [ -n "$ONLY" ] && [ "$ONLY" != "$distro" ] && continue
  pkgs="$(packages_for "$distro" | tr '\n' ' ')"
  echo "== $distro ($(image_for "$distro"))"
  out="$(docker run --rm --platform "$PLATFORM" "$(image_for "$distro")" sh -c "$(probe_script "$distro")" -- $pkgs 2>&1 || true)"
  echo "$out" | grep '^missing' | sed 's/^/   /' || true
  n_missing=$(echo "$out" | grep -c '^missing' || true)
  n_ok=$(echo "$out" | grep -c '^ok' || true)
  echo "   $n_ok resolvable, $n_missing missing"
  [ "$n_missing" -eq 0 ] || rc=1
done
exit $rc
