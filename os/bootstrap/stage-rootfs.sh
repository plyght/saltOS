#!/bin/sh
set -eu

ARCH="${ARCH:-x86_64}"
OUT="${OUT:-/var/tmp/saltos-build}"
REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SALT="${SALT:-salt}"
STAGES="${STAGES:-base desktop}"

case "$ARCH" in
  arm64) ARCH=aarch64 ;;
  amd64) ARCH=x86_64 ;;
esac

WORK="$OUT/$ARCH"
PKGDIR="$WORK/packages"
ROOTFS="${ROOTFS:-$WORK/rootfs}"
ORDER="$REPO_ROOT/os/bootstrap/build-order.toml"
RUNIT_SRC="$REPO_ROOT/os/runit/sv"

log() { printf '[stage-rootfs %s] %s\n' "$ARCH" "$*"; }

mkdir -p "$ROOTFS"

for d in dev proc sys run tmp \
         boot home root opt mnt media srv \
         etc etc/runit etc/runit/runsvdir/current etc/salt etc/ssh etc/runit/sv \
         usr usr/bin usr/sbin usr/lib usr/share \
         var var/lib var/lib/salt var/log var/tmp var/cache; do
  mkdir -p "$ROOTFS/$d"
done
chmod 1777 "$ROOTFS/tmp" "$ROOTFS/var/tmp"

stage_packages() {
  awk -v stage="[$1]" '
    $0 == stage { inblock=1; next }
    /^\[/ { inblock=0 }
    inblock && /"/ {
      line=$0; gsub(/[",]/, "", line); gsub(/^[ \t]+|[ \t]+$/, "", line)
      if (line != "packages = [" && line != "" && line != "]") print line
    }
  ' "$ORDER"
}

log "installing $STAGES packages into $ROOTFS"
for stage in $STAGES; do
  stage_packages "$stage" | while IFS= read -r pkg; do
    [ -n "$pkg" ] || continue
    p=$(ls -1t "$PKGDIR/$pkg"-*-"$ARCH".grain 2>/dev/null | head -n1)
    [ -n "$p" ] || { log "skip missing package $pkg"; continue; }
    "$SALT" install "$p" --root "$ROOTFS" --yes
  done
done

log "installing runit service tree"
if [ -d "$RUNIT_SRC" ]; then
  cp -a "$RUNIT_SRC/." "$ROOTFS/etc/runit/sv/"
  for svc in udevd dbus seatd socklog sshd chronyd dhcpcd agetty-tty1 agetty-tty2 sddm; do
    [ -d "$ROOTFS/etc/runit/sv/$svc" ] || continue
    ln -sf "/etc/runit/sv/$svc" "$ROOTFS/etc/runit/runsvdir/current/$svc"
  done
fi

log "writing fstab"
cat > "$ROOTFS/etc/fstab" <<'FSTAB'
# device                          mount   type    options                                 dump pass
LABEL=saltos-root                 /       btrfs   subvol=@,compress=zstd,noatime          0 0
LABEL=saltos-root                 /home   btrfs   subvol=@home,compress=zstd,noatime      0 0
LABEL=saltos-root                 /var    btrfs   subvol=@var,compress=zstd,noatime       0 0
LABEL=saltos-root                 /var/log btrfs  subvol=@log,compress=zstd,noatime       0 0
LABEL=saltos-root                 /.snapshots btrfs subvol=@snapshots,compress=zstd,noatime 0 0
LABEL=saltos-esp                  /boot   vfat    defaults,noatime                        0 2
tmpfs                             /tmp    tmpfs   defaults,nosuid,nodev                    0 0
FSTAB

log "seeding /etc skeleton"
cat > "$ROOTFS/etc/hostname" <<'EOF'
saltos
EOF

cat > "$ROOTFS/etc/hosts" <<'EOF'
127.0.0.1   localhost
::1         localhost ip6-localhost ip6-loopback
127.0.1.1   saltos
EOF

cat > "$ROOTFS/etc/os-release" <<'EOF'
NAME="saltOS"
PRETTY_NAME="saltOS"
ID=saltos
ANSI_COLOR="0;36"
HOME_URL="https://salt.os"
EOF

cat > "$ROOTFS/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/bash
EOF

cat > "$ROOTFS/etc/group" <<'EOF'
root:x:0:
wheel:x:10:
audio:x:63:
video:x:78:
seat:x:99:
EOF

cat > "$ROOTFS/etc/shells" <<'EOF'
/bin/sh
/bin/bash
EOF

cat > "$ROOTFS/etc/salt/repo.conf" <<'EOF'
[repo]
url = "file:///var/cache/salt/repo"
key = "/etc/salt/trusted.pub"
EOF

cat > "$ROOTFS/etc/salt/salt.conf" <<'EOF'
[install]
auto_expose = "prompt"

[strata]
expose_pm = true
auto_service = true
EOF

log "wiring stratum plane (shims on PATH + builtin recipes)"
mkdir -p "$ROOTFS/etc/profile.d" "$ROOTFS/usr/local/salt/shims" \
         "$ROOTFS/etc/salt/strata" "$ROOTFS/strata"
if [ -f "$REPO_ROOT/os/profile.d/salt-shims.sh" ]; then
  cp "$REPO_ROOT/os/profile.d/salt-shims.sh" "$ROOTFS/etc/profile.d/salt-shims.sh"
  chmod 0644 "$ROOTFS/etc/profile.d/salt-shims.sh"
fi
if [ -d "$REPO_ROOT/strata" ]; then
  cp "$REPO_ROOT"/strata/*.toml "$ROOTFS/etc/salt/strata/" 2>/dev/null || true
fi

ln -sf usr/bin "$ROOTFS/bin" 2>/dev/null || true
ln -sf usr/sbin "$ROOTFS/sbin" 2>/dev/null || true
ln -sf usr/lib "$ROOTFS/lib" 2>/dev/null || true

log "rootfs ready at $ROOTFS"
