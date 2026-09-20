#!/bin/sh
# End-to-end package-manager test on a booted saltOS: build an x86_64 UEFI VM
# image (btrfs root), build two small signed .grain repos (v1: hello 1.0 +
# libgreet + greeter; v2: hello 1.1), serve them from this host over QEMU user
# networking, and drive the guest over its serial console through sync,
# install, remove/--cascade, update, lock/diff/apply, verify, config gc,
# rollback and a reboot into the rolled-back root (see qemu_pkg_drive.py).
#
#   sudo -n sh tests/qemu_pkg_smoke.sh
#
# Env: SALT_BIN (default build/src/salt/salt), SALTSETUP_BIN, OUT (default
#      ./out-pkg), PORT (default 8124), IMG (skip the image build and reuse an
#      existing raw image), OVMF_CODE/OVMF_VARS (firmware paths). The serial log
#      ends up in $OUT/serial.log and the driver's assertions in $OUT/driver.log.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SALT_BIN=${SALT_BIN:-$REPO/build/src/salt/salt}
SALTSETUP_BIN=${SALTSETUP_BIN:-$REPO/build/src/setup/salt-setup}
OUT=${OUT:-$PWD/out-pkg}
PORT=${PORT:-8124}
GUEST_HOST=${GUEST_HOST:-10.0.2.2}
ARCH=x86_64

die() { echo "qemu_pkg_smoke: $1" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"; }
need qemu-system-x86_64; need qemu-img; need python3; need curl
[ -x "$SALT_BIN" ] || die "salt binary not found at $SALT_BIN (build first)"
[ "$(id -u)" = 0 ] || die "run as root (the image build needs loop devices and chroot)"

find_fw() {
  for f in "$@"; do [ -f "$f" ] && { echo "$f"; return; }; done
  return 1
}
OVMF_CODE=${OVMF_CODE:-$(find_fw /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
  /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/qemu/OVMF_CODE.fd)} \
  || die "OVMF_CODE firmware not found"
OVMF_VARS=${OVMF_VARS:-$(find_fw /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd \
  /usr/share/edk2/x64/OVMF_VARS.4m.fd /usr/share/edk2/ovmf/OVMF_VARS.fd /usr/share/qemu/OVMF_VARS.fd)} \
  || die "OVMF_VARS firmware not found"

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

if [ -z "${IMG:-}" ]; then
  echo "==> building the VM image (console edition)"
  REPO_DIR="$REPO" SALT_BIN="$SALT_BIN" SALTSETUP_BIN="$SALTSETUP_BIN" \
    OUT="$OUT" WORK="$OUT/work" EDITION=console VERSION="${VERSION:-0.1.0}" \
    IMG_SIZE_MB="${IMG_SIZE_MB:-6144}" COMPRESS=0 \
    bash "$REPO/os/build/vm-x86.sh"
  IMG=$(ls "$OUT"/*.img | head -1)
fi
[ -f "$IMG" ] || die "image not found: $IMG"

echo "==> building the signed test repos"
R="$OUT/repo"
rm -rf "$R"; mkdir -p "$R/src" "$R/recipes" "$R/www"
touch "$R/src/.keep"
recipe() { # dir name version deps
  mkdir -p "$R/recipes/$1"
  cat > "$R/recipes/$1/recipe.toml" <<EOF
name = "$2"
version = "$3"
release = 1
arch = ["x86_64", "aarch64"]
summary = "vm test package $2"
license = "MIT"

[source]
url = "file://$R/src"
sha256 = "TODO-sha256"

[build]
system = "custom"
script = """
mkdir -p "\$SALT_DEST/usr/bin"
printf '#!/bin/sh\\necho $2 $3\\n' > "\$SALT_DEST/usr/bin/$2"
chmod +x "\$SALT_DEST/usr/bin/$2"
"""

[package]
deps = [$4]
conflicts = []

[reproducibility]
status = "verified"
EOF
}
export SALT_OUT="$R/out" SALT_WORK="$R/work"
recipe hello10 hello 1.0 ""
recipe libgreet libgreet 1.0 ""
recipe greeter greeter 1.0 '"libgreet"'
for r in hello10 libgreet greeter; do "$SALT_BIN" build "$R/recipes/$r" >/dev/null; done
"$SALT_BIN" keygen "$R/keys" repo >/dev/null
"$SALT_BIN" --key "$R/keys/repo.sec" repo publish "$R/out/$ARCH" >/dev/null
cp -a "$R/out" "$R/www/v1"
recipe hello11 hello 1.1 ""
"$SALT_BIN" build "$R/recipes/hello11" >/dev/null
rm -f "$R/out/$ARCH/packages/hello-1.0-1-$ARCH.grain"
"$SALT_BIN" --key "$R/keys/repo.sec" repo publish "$R/out/$ARCH" >/dev/null
cp -a "$R/out" "$R/www/v2"
PUB=$(head -1 "$R/keys/repo.pub" | tr -d '\r\n')
[ -f "$R/www/v1/$ARCH/index.toml.sig" ] && [ -f "$R/www/v2/$ARCH/index.toml.sig" ] || die "repo indexes are not signed"

echo "==> serving $R/www on 127.0.0.1:$PORT"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$R/www" >"$OUT/http.log" 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null || true' EXIT INT TERM
for i in $(seq 1 30); do curl -fsS -o /dev/null "http://127.0.0.1:$PORT/v1/$ARCH/index.toml" && break; sleep 0.5; done

echo "==> driving $IMG"
rm -f "$OUT/serial.log" "$OUT/scratch.qcow2"
qemu-img create -q -f qcow2 -b "$IMG" -F raw "$OUT/scratch.qcow2"
{
  drc=0
  python3 "$HERE/qemu_pkg_drive.py" --image "$OUT/scratch.qcow2" --format qcow2 \
    --ovmf-code "$OVMF_CODE" --ovmf-vars "$OVMF_VARS" --log "$OUT/serial.log" \
    --repo-url "http://$GUEST_HOST:$PORT" --pubkey "$PUB" || drc=$?
  echo "driver exit: $drc"
  exit $drc
} 2>&1 | tee "$OUT/driver.log"
drc=$(tail -1 "$OUT/driver.log" | sed -n 's/^driver exit: //p')
[ "$drc" = 0 ] || die "driver failed (exit $drc); see $OUT/serial.log"
echo "qemu_pkg_smoke: OK"
