#!/bin/bash
set -euo pipefail

ISO="${1:?usage: test-vm.sh <iso> [outdir]}"
OUT="${2:-$PWD/omakase-test}"
ARCH="${ARCH:-x86_64}"
MEM="${MEM:-4096}"
CPUS="${CPUS:-2}"
DISK_SIZE="${DISK_SIZE:-24G}"
INSTALL_TIMEOUT="${INSTALL_TIMEOUT:-1500}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-420}"
THEME="${THEME:-gruvbox}"
USERNAME="${OMAKASE_USER:-salt}"
PASSWORD="${OMAKASE_PASSWORD:-saltos}"
DISTRO="${OMAKASE_DISTRO:-arch}"

mkdir -p "$OUT"
ISO="$(readlink -f "$ISO")"
cd "$OUT"

case "$ARCH" in
  x86_64)
    QEMU=qemu-system-x86_64
    for c in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd; do
      [ -f "$c" ] && { CODE="$c"; break; }
    done
    for v in /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd /usr/share/edk2/x64/OVMF_VARS.4m.fd; do
      [ -f "$v" ] && { VARS_SRC="$v"; break; }
    done
    [ -n "${CODE:-}" ] && [ -n "${VARS_SRC:-}" ] || { echo "test-vm: OVMF firmware not found" >&2; exit 1; }
    MACHINE=(-machine "q35,accel=kvm:tcg" -cpu max)
    ;;
  aarch64)
    QEMU=qemu-system-aarch64
    CODE=/usr/share/AAVMF/AAVMF_CODE.fd
    VARS_SRC=/usr/share/AAVMF/AAVMF_VARS.fd
    MACHINE=(-machine "virt,accel=kvm:tcg" -cpu max)
    ;;
  *) echo "test-vm: unsupported arch $ARCH" >&2; exit 1 ;;
esac
cp -f "$VARS_SRC" ovmf-vars.fd

QPID=""
cleanup() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null || true; fi; }
trap cleanup EXIT

wait_marker() {
  local log="$1" ok="$2" fail="$3" timeout="$4" pid="$5" n=0
  while [ $n -lt "$timeout" ]; do
    if grep -q "$ok" "$log" 2>/dev/null; then return 0; fi
    if [ -n "$fail" ] && grep -q "$fail" "$log" 2>/dev/null; then return 1; fi
    kill -0 "$pid" 2>/dev/null || return 2
    sleep 5
    n=$((n + 5))
  done
  return 3
}

if [ "${OMAKASE_REUSE_TARGET:-0}" = 1 ] && [ -f target.qcow2 ]; then
  echo "==> reusing installed target.qcow2"
else
echo "==> preparing target disk and cidata"
rm -f target.qcow2 cidata.img
qemu-img create -q -f qcow2 target.qcow2 "$DISK_SIZE"
cat >install.toml <<EOF
[system]
hostname = "saltos-omakase"
locale = "en_US.UTF-8"
timezone = "UTC"
keymap = "us"

[kernel]
source = "native"

[[stratum]]
name = "$DISTRO"
role = "primary"
expose = true

[user]
username = "$USERNAME"
full_name = "saltOS Tester"
email = ""
deferred = false

[install]
profile = "omakase"
disk = "/dev/vda"
mode = "disk"
encrypt = false
serial_console = true
EOF
printf 'password=%s\n' "$PASSWORD" >credentials
truncate -s 8M cidata.img
mkfs.vfat -n cidata cidata.img >/dev/null
mcopy -i cidata.img install.toml ::install.toml
mcopy -i cidata.img credentials ::credentials

echo "==> phase 1: unattended install from $ISO"
rm -f install-serial.log qmon
"$QEMU" "${MACHINE[@]}" -m "$MEM" -smp "$CPUS" -no-reboot -display none \
  -drive if=pflash,format=raw,readonly=on,file="$CODE" \
  -drive if=pflash,format=raw,file=ovmf-vars.fd \
  -device virtio-vga -device virtio-rng-pci \
  -drive file=target.qcow2,if=virtio,format=qcow2 \
  -drive file=cidata.img,if=virtio,format=raw,readonly=on \
  -drive file="$ISO",media=cdrom,if=none,id=cd -device ide-cd,drive=cd,bootindex=0 \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial file:install-serial.log -monitor unix:qmon,server,nowait &
QPID=$!
set +e
wait_marker install-serial.log SALTOS_OMAKASE_INSTALL_OK SALTOS_OMAKASE_INSTALL_FAIL "$INSTALL_TIMEOUT" $QPID
rc=$?
set -e
sleep 5
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
if [ $rc -ne 0 ]; then
  echo "INSTALL FAILED (rc=$rc)"; tail -n 80 install-serial.log; exit 1
fi
echo "install OK"
fi

echo "==> phase 2: boot installed disk"
rm -f boot-serial.log serial.sock qmon
cp -f "$VARS_SRC" ovmf-vars.fd
"$QEMU" "${MACHINE[@]}" -m "$MEM" -smp "$CPUS" -no-reboot -display none \
  -drive if=pflash,format=raw,readonly=on,file="$CODE" \
  -drive if=pflash,format=raw,file=ovmf-vars.fd \
  -device virtio-vga -device virtio-rng-pci -device virtio-keyboard-pci -device virtio-mouse-pci \
  -drive file=target.qcow2,if=virtio,format=qcow2 \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -chardev socket,id=ser0,path=serial.sock,server=on,wait=off,logfile=boot-serial.log \
  -serial chardev:ser0 -monitor unix:qmon,server,nowait &
QPID=$!
set +e
wait_marker boot-serial.log SALTOS_SWAY_SESSION_OK SALTOS_SWAY_SESSION_TIMEOUT "$BOOT_TIMEOUT" $QPID
rc=$?
set -e
if [ $rc -ne 0 ]; then
  echo "SWAY SESSION FAILED (rc=$rc)"; tail -n 80 boot-serial.log; kill $QPID 2>/dev/null || true; exit 1
fi
echo "sway session OK"
sleep 10

serial_send() { printf '%s\r' "$1" | socat -T 2 - UNIX-CONNECT:serial.sock >/dev/null 2>&1 || true; }
serial_send ""
sleep 3
serial_send "saltos-theme set $THEME && echo SALTOS_THEME_OK \$(saltos-theme current)"
set +e
wait_marker boot-serial.log "SALTOS_THEME_OK $THEME" "" 90 $QPID
rc=$?
set -e
if [ $rc -ne 0 ]; then
  echo "THEME SWITCH FAILED (rc=$rc)"; tail -n 40 boot-serial.log; kill $QPID 2>/dev/null || true; exit 1
fi
echo "theme switch OK"
serial_send "printf '%s\\n' '$PASSWORD' | sudo -S sv status /etc/runit/runsvdir/current/* 2>/dev/null | sed 's/^/SALTOS_SV /'"
sleep 5
serial_send "grep -c . ~/.config/saltos/theme/sway.conf | sed 's/^/SALTOS_THEME_LINES /'"
sleep 5
serial_send "for p in sway waybar mako swayidle vicinae pipewire wireplumber; do pgrep -x \"\$p\" >/dev/null && echo SALTOS_PROC \$p up || echo SALTOS_PROC \$p DOWN; done"
sleep 5
serial_send "tail -n 30 ~/.local/state/saltos/session.log | sed 's/^/SALTOS_SESSION_LOG /'"
sleep 5
printf 'screendump screen.ppm\n' | socat -T 2 - UNIX-CONNECT:qmon >/dev/null 2>&1 || true
sleep 3
if [ -f screen.ppm ] && command -v pnmtopng >/dev/null; then
  pnmtopng screen.ppm >desktop.png 2>/dev/null && echo "screenshot: $OUT/desktop.png"
fi
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true
grep -q "SALTOS_SV" boot-serial.log && echo "runit services listed"
echo "ALL OK"
