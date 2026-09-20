#!/bin/bash
set -euo pipefail

usage() {
  cat >&2 <<EOF
usage: $0 --iso FILE --config system.toml [--firmware bios|uefi] [--mode text|calamares]
          [--arch x86_64|aarch64] [--out DIR] [--disk-size 20G] [--mem MiB]
          [--install-timeout SEC] [--boot-timeout SEC]

Boots the live ISO under QEMU, hands it the configuration through fw_cfg so the
autoinstall service runs salt-setup (text) or drives Calamares (calamares),
then reboots the virtual disk alone and waits for the runit boot marker.
Serial logs and screenshots land in --out.
EOF
  exit 2
}

ISO="" CONFIG="" FW=bios MODE=text ARCH=x86_64 OUT="$PWD/qemu-test" DISK_SIZE=20G MEM=""
INSTALL_TMO=2400 BOOT_TMO=420
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --config) CONFIG="$2"; shift 2 ;;
    --firmware) FW="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --disk-size) DISK_SIZE="$2"; shift 2 ;;
    --mem) MEM="$2"; shift 2 ;;
    --install-timeout) INSTALL_TMO="$2"; shift 2 ;;
    --boot-timeout) BOOT_TMO="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$ISO" ] && [ -n "$CONFIG" ] || usage
[ -f "$ISO" ] || { echo "no such ISO: $ISO" >&2; exit 1; }
[ -f "$CONFIG" ] || { echo "no such config: $CONFIG" >&2; exit 1; }
case "$FW" in bios|uefi) ;; *) usage ;; esac
case "$MODE" in text|calamares) ;; *) usage ;; esac
[ "$ARCH" = x86_64 ] || [ "$FW" = uefi ] || { echo "$ARCH only boots with UEFI" >&2; exit 1; }
[ -n "$MEM" ] || { [ "$MODE" = calamares ] && MEM=4096 || MEM=3072; }

mkdir -p "$OUT/shots"
OUT="$(cd "$OUT" && pwd)"
DISK="$OUT/disk.qcow2"
MON="$OUT/monitor.sock"
INSTALL_LOG="$OUT/install-serial.log"
BOOT_LOG="$OUT/boot-serial.log"
rm -f "$DISK" "$MON" "$INSTALL_LOG" "$BOOT_LOG" "$OUT/shots"/*.ppm "$OUT/shots"/*.png
qemu-img create -q -f qcow2 "$DISK" "$DISK_SIZE"

find_ovmf() {
  for c in "$@"; do [ -f "$c" ] && { echo "$c"; return 0; }; done
  return 1
}

ACCEL="tcg"
ACCEL_OPTS=(-accel tcg,thread=multi)
if [ -w /dev/kvm ] && [ "$ARCH" = "$(uname -m)" ]; then ACCEL="kvm"; ACCEL_OPTS=(-accel kvm); fi
case "$ARCH" in
  x86_64)
    QEMU=qemu-system-x86_64
    SERIAL=ttyS0
    if [ "$FW" = uefi ]; then
      CODE="$(find_ovmf /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/qemu/OVMF.fd)" || { echo "OVMF firmware not found" >&2; exit 1; }
      VARS_SRC="${CODE%CODE*}VARS${CODE##*CODE}"
      cp "$VARS_SRC" "$OUT/OVMF_VARS.fd" 2>/dev/null || cp "$CODE" "$OUT/OVMF_VARS.fd"
      MACHINE=(-machine q35 -drive if=pflash,format=raw,readonly=on,file="$CODE" -drive if=pflash,format=raw,file="$OUT/OVMF_VARS.fd")
    else
      MACHINE=(-machine pc)
    fi
    CPU=(-cpu max -smp 2)
    [ "$ACCEL" = kvm ] && CPU=(-cpu host -smp 2)
    VIDEO=(-vga std)
    ;;
  aarch64)
    QEMU=qemu-system-aarch64
    SERIAL=ttyAMA0
    CODE="$(find_ovmf /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd /usr/share/edk2/aarch64/QEMU_EFI.fd)" || { echo "AAVMF firmware not found" >&2; exit 1; }
    truncate -s 64M "$OUT/AAVMF_CODE.fd"; dd if="$CODE" of="$OUT/AAVMF_CODE.fd" conv=notrunc status=none
    truncate -s 64M "$OUT/AAVMF_VARS.fd"
    MACHINE=(-machine virt -drive if=pflash,format=raw,readonly=on,file="$OUT/AAVMF_CODE.fd" -drive if=pflash,format=raw,file="$OUT/AAVMF_VARS.fd")
    CPU=(-cpu max -smp 2)
    [ "$ACCEL" = kvm ] && CPU=(-cpu host -smp 2)
    VIDEO=(-device virtio-gpu-pci -device qemu-xhci -device usb-kbd -device usb-tablet)
    ;;
  *) usage ;;
esac

COMMON=("${MACHINE[@]}" "${ACCEL_OPTS[@]}" "${CPU[@]}" -m "$MEM" "${VIDEO[@]}" -display none -no-reboot
  -monitor unix:"$MON",server,nowait -netdev user,id=n0 -device virtio-net-pci,netdev=n0,romfile= -rtc base=utc)

screendump() {
  [ -S "$MON" ] || return 0
  printf 'screendump %s/shots/%s.ppm\n' "$OUT" "$1" | socat -t 2 - unix-connect:"$MON" > /dev/null 2>&1 || return 0
  sleep 1
  if [ -s "$OUT/shots/$1.ppm" ] && command -v pnmtopng > /dev/null 2>&1; then
    pnmtopng "$OUT/shots/$1.ppm" > "$OUT/shots/$1.png" 2> /dev/null && rm -f "$OUT/shots/$1.ppm"
  fi
}

wait_for() {
  log="$1" tmo="$2" shots="$3"; shift 3
  t=0 n=0
  while [ "$t" -lt "$tmo" ]; do
    for m in "$@"; do grep -q -- "$m" "$log" 2> /dev/null && return 0; done
    kill -0 "$QPID" 2> /dev/null || return 1
    sleep 5; t=$((t + 5))
    if [ "$shots" = 1 ] && [ $((t % 30)) -eq 0 ]; then
      n=$((n + 1)); screendump "$(printf '%s-%03d' "$STAGE" "$n")"
    fi
  done
  return 1
}

STAGE=install
echo "== install ($MODE, $FW, $ARCH, accel=$ACCEL) from $ISO"
CDROM=(-cdrom "$ISO" -boot d)
[ "$ARCH" = aarch64 ] && CDROM=(-drive file="$ISO",media=cdrom,if=none,id=cd,format=raw -device virtio-scsi-pci -device scsi-cd,drive=cd,bootindex=0)
"$QEMU" "${COMMON[@]}" \
  -serial file:"$INSTALL_LOG" \
  -drive file="$DISK",if=virtio,format=qcow2 \
  "${CDROM[@]}" \
  -fw_cfg name=opt/saltos/autoinstall-mode,string="$MODE" \
  -fw_cfg name=opt/saltos/autoinstall-then,string=poweroff \
  -fw_cfg name=opt/saltos/autoinstall,file="$CONFIG" &
QPID=$!
SHOTS=0; [ "$MODE" = calamares ] && SHOTS=1
set +e
wait_for "$INSTALL_LOG" "$INSTALL_TMO" "$SHOTS" "SALTOS_AUTOINSTALL OK" "SALTOS_AUTOINSTALL FAIL" "SALTOS_GUI_AUTOINSTALL FAIL"
rc=$?
set -e
screendump install-final
if [ "$rc" -ne 0 ] || ! grep -q "SALTOS_AUTOINSTALL OK" "$INSTALL_LOG"; then
  kill "$QPID" 2> /dev/null || true
  echo "----- install serial tail -----"; tail -n 150 "$INSTALL_LOG" || true
  echo "INSTALL FAILED ($MODE, $FW): no SALTOS_AUTOINSTALL OK within ${INSTALL_TMO}s" >&2
  exit 1
fi
t=0
while kill -0 "$QPID" 2> /dev/null && [ "$t" -lt 120 ]; do sleep 2; t=$((t + 2)); done
kill "$QPID" 2> /dev/null || true
wait "$QPID" 2> /dev/null || true
echo "INSTALL OK ($MODE, $FW)"

STAGE=boot
echo "== boot installed disk ($FW, $ARCH)"
"$QEMU" "${COMMON[@]}" \
  -serial file:"$BOOT_LOG" \
  -drive file="$DISK",if=virtio,format=qcow2 &
QPID=$!
set +e
wait_for "$BOOT_LOG" "$BOOT_TMO" 0 "SALTOS_BOOT_OK" "SALTOS_BOOT_FAIL"
rc=$?
set -e
sleep 5
screendump boot-final
kill "$QPID" 2> /dev/null || true
wait "$QPID" 2> /dev/null || true
echo "----- installed system serial tail -----"; tail -n 80 "$BOOT_LOG" || true
if grep -q "SALTOS_BOOT_OK" "$BOOT_LOG"; then
  echo "BOOT OK ($FW): installed system reached runit stage 2"
else
  echo "BOOT FAILED ($FW): SALTOS_BOOT_OK not found within ${BOOT_TMO}s" >&2
  exit 1
fi
