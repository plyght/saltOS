#!/bin/sh
# End-to-end OTA test: build an x86_64 UEFI VM image whose factory kernel and
# salt are registered as grains, publish a newer salt + kernel with ship.sh,
# serve it from this host over QEMU user networking, and drive the guest over
# its serial console through update, trial-kernel fallback, confirm, rollback
# and the bad-hash / bad-signature refusals (see qemu-drive.py).
#
#   sudo -n sh os/ota/test-qemu.sh
#
# Env: SALT_BIN (default build/src/salt/salt), OUT (default ./out-ota), PORT
#      (default 8123), KERNEL_PKG (factory kernel, default linux-lts),
#      OTA_KERNEL_PKG (shipped kernel, default linux), OLD_VERSION/NEW_VERSION
#      (salt grain versions, default 0.1.0 / 0.1.1), IMG (skip the image build
#      and reuse an image whose $OUT/ota/ metadata is still present),
#      OVMF_CODE/OVMF_VARS (firmware paths). The serial log ends up in
#      $OUT/serial.log and the driver's assertions in $OUT/driver.log.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SALT_BIN=${SALT_BIN:-$REPO/build/src/salt/salt}
OUT=${OUT:-$PWD/out-ota}
PORT=${PORT:-8123}
KERNEL_PKG=${KERNEL_PKG:-linux-lts}
OTA_KERNEL_PKG=${OTA_KERNEL_PKG:-linux}
OLD_VERSION=${OLD_VERSION:-0.1.0}
NEW_VERSION=${NEW_VERSION:-0.1.1}
GUEST_HOST=${GUEST_HOST:-10.0.2.2}

die() { echo "test-qemu: $1" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"; }
need qemu-system-x86_64; need qemu-img; need python3; need zstd
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
KEYS="$OUT/keys"
[ -f "$KEYS/ota.sec" ] || "$SALT_BIN" keygen "$KEYS" ota >/dev/null
[ -f "$KEYS/bad.sec" ] || "$SALT_BIN" keygen "$KEYS" bad >/dev/null
PUB=$(head -1 "$KEYS/ota.pub")

if [ -z "${IMG:-}" ]; then
  echo "==> building the VM image (factory salt $OLD_VERSION, kernel package $KERNEL_PKG; staging $OTA_KERNEL_PKG)"
  rm -rf "$OUT/ota"
  REPO_DIR="$REPO" SALT_BIN="$SALT_BIN" SALTSETUP_BIN="${SALTSETUP_BIN:-$REPO/build/src/setup/salt-setup}" \
    OUT="$OUT" WORK="$OUT/work" EDITION=console VERSION="$OLD_VERSION" \
    IMG_SIZE_MB="${IMG_SIZE_MB:-6144}" COMPRESS=0 KERNEL_PKG="$KERNEL_PKG" OTA_KERNEL_PKG="$OTA_KERNEL_PKG" \
    OTA_SIGNING_KEY="$KEYS/ota.sec" OTA_KEY="$PUB" OTA_SOURCE="http://$GUEST_HOST:$PORT/good" \
    OTA_SERVICE_CONF="SALTOS_OTA_CONFIRM_DELAY=${CONFIRM_DELAY:-90}
SALTOS_OTA_STARTUP_DELAY=3600" \
    bash "$REPO/os/build/vm-x86.sh"
  IMG=$(ls "$OUT"/*.img | head -1)
fi
[ -f "$IMG" ] || die "image not found: $IMG"
[ -f "$OUT/ota/kernel-release" ] || die "$OUT/ota/kernel-release missing (image built without OTA_KERNEL_PKG?)"
NEW_KERNEL=$(cat "$OUT/ota/kernel-release")
OLD_KERNEL=$(cat "$OUT/ota/factory-kernel-release")

echo "==> publishing OTA repos (salt $NEW_VERSION + kernel $NEW_KERNEL)"
WWW="$OUT/www"
rm -rf "$WWW"; mkdir -p "$WWW"
SALT="$SALT_BIN" VERSION="$NEW_VERSION" OTA_KEYS="$KEYS" SERVE=0 WORK="$OUT/ship-work" \
  KERNEL_TREE="$OUT/ota/kernel" KERNEL_RELEASE="$NEW_KERNEL" \
  URL_BASE="http://$GUEST_HOST:$PORT/releases" \
  sh "$HERE/ship.sh" "$WWW/good" >"$OUT/ship.log" 2>&1 || { cat "$OUT/ship.log"; die "ship.sh failed"; }
ARCH=$("$SALT_BIN" --version | sed -E 's/.*\((.*)\)/\1/')
[ -f "$WWW/good/$ARCH/index.toml" ] || die "ship.sh produced no $ARCH/index.toml"
grep -q "^url = \"http://$GUEST_HOST:$PORT/releases/" "$WWW/good/$ARCH/index.toml" || die "index carries no package urls"

# Same split as the GitHub channel: the index lives under <source>/<arch>/ (Pages),
# the grains under a flat /releases/ directory (Release assets) named by the url.
mkdir -p "$WWW/badhash/$ARCH/packages" "$WWW/badsig"
cp "$WWW/good/$ARCH/packages/"*.grain "$WWW/badhash/$ARCH/packages/"
"$SALT_BIN" --key "$(head -1 "$KEYS/ota.sec")" repo publish "$WWW/badhash/$ARCH" \
  "http://$GUEST_HOST:$PORT/releases-badhash" >/dev/null
mv "$WWW/badhash/$ARCH/packages" "$WWW/releases-badhash"
grain=$(ls "$WWW/releases-badhash/salt-"*.grain | head -1)
python3 -c 'import sys; p=sys.argv[1]; b=bytearray(open(p,"rb").read()); b[64]^=0xff; open(p,"wb").write(b)' "$grain"

cp -a "$WWW/good/$ARCH" "$WWW/badsig/$ARCH"
"$SALT_BIN" --key "$(head -1 "$KEYS/bad.sec")" repo publish "$WWW/badsig/$ARCH" \
  "http://$GUEST_HOST:$PORT/releases" >/dev/null
rm -rf "$WWW/badsig/$ARCH/packages"

mv "$WWW/good/$ARCH/packages" "$WWW/releases"

echo "==> serving $WWW on 127.0.0.1:$PORT"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$WWW" >"$OUT/http.log" 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null || true' EXIT INT TERM
for i in $(seq 1 30); do curl -fsS -o /dev/null "http://127.0.0.1:$PORT/good/$ARCH/index.toml" && break; sleep 0.5; done

echo "==> driving $IMG (kernel $OLD_KERNEL -> $NEW_KERNEL, salt $OLD_VERSION -> $NEW_VERSION)"
rm -f "$OUT/serial.log" "$OUT/scratch.qcow2"
qemu-img create -q -f qcow2 -b "$IMG" -F raw "$OUT/scratch.qcow2"
{
  drc=0
  python3 "$HERE/qemu-drive.py" --image "$OUT/scratch.qcow2" --format qcow2 --ovmf-code "$OVMF_CODE" --ovmf-vars "$OVMF_VARS" \
    --log "$OUT/serial.log" --repo-url "http://$GUEST_HOST:$PORT" \
    --old-kernel "$OLD_KERNEL" --new-kernel "$NEW_KERNEL" \
    --old-version "$OLD_VERSION" --new-version "$NEW_VERSION" \
    ${BOOT_TIMEOUT:+--boot-timeout "$BOOT_TIMEOUT"} 2>&1 || drc=$?
  echo "$drc" >"$OUT/driver.rc"
} | tee "$OUT/driver.log"
rc=$(cat "$OUT/driver.rc")
if [ "$rc" -eq 0 ] && grep -q SALTOS_OTA_OK "$OUT/serial.log"; then
  echo "OTA OK: upgrade, trial fallback, confirm, rollback and refusal all verified ($OUT/serial.log)"
  exit 0
fi
echo "----- serial tail -----"
tail -n 80 "$OUT/serial.log" || true
die "OTA test failed (see $OUT/driver.log and $OUT/serial.log)"
