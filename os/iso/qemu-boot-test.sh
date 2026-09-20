#!/bin/bash
set -euo pipefail

usage() {
  cat >&2 <<EOF
usage: $0 (--iso FILE | --disk FILE) --ok MARKER [--fail MARKER] [--also MARKER]...
          [--arch x86_64|aarch64] [--firmware bios|uefi] [--console serial|virtio]
          [--mem MiB] [--timeout SEC] [--net] [--log FILE] [--screenshot FILE]

Boots a live ISO or a raw/qcow2 disk image under QEMU with an explicitly chosen
accelerator (KVM when /dev/kvm is writable and the guest matches the host, else
multi-threaded TCG), waits for --ok on the guest console, and fails when it does
not appear (or --fail appears) within --timeout. Every --also marker must also be
present. --console virtio captures /dev/hvc0 instead of the UART.
EOF
  exit 2
}

ISO="" DISK="" OK="" FAIL="" ARCH="$(uname -m)" FW="" CONSOLE=serial MEM=2560 TMO=480 NET=0 LOG="$PWD/qemu.log" SHOT=""
ALSO=()
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --disk) DISK="$2"; shift 2 ;;
    --ok) OK="$2"; shift 2 ;;
    --fail) FAIL="$2"; shift 2 ;;
    --also) ALSO+=("$2"); shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --firmware) FW="$2"; shift 2 ;;
    --console) CONSOLE="$2"; shift 2 ;;
    --mem) MEM="$2"; shift 2 ;;
    --timeout) TMO="$2"; shift 2 ;;
    --net) NET=1; shift ;;
    --log) LOG="$2"; shift 2 ;;
    --screenshot) SHOT="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$OK" ] || usage
[ -n "$ISO" ] || [ -n "$DISK" ] || usage
[ -z "$ISO" ] || [ -f "$ISO" ] || { echo "no such ISO: $ISO" >&2; exit 1; }
[ -z "$DISK" ] || [ -f "$DISK" ] || { echo "no such disk image: $DISK" >&2; exit 1; }
case "$CONSOLE" in serial|virtio) ;; *) usage ;; esac
[ -n "$FW" ] || { [ "$ARCH" = x86_64 ] && FW=bios || FW=uefi; }
case "$FW" in bios|uefi) ;; *) usage ;; esac
[ "$ARCH" = x86_64 ] || [ "$FW" = uefi ] || { echo "$ARCH only boots with UEFI" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
MON="$WORK/monitor.sock"
rm -f "$LOG"

find_fw() {
  for c in "$@"; do [ -f "$c" ] && { echo "$c"; return 0; }; done
  return 1
}

ACCEL=tcg
ACCEL_OPTS=(-accel tcg,thread=multi)
if [ -w /dev/kvm ] && [ "$ARCH" = "$(uname -m)" ]; then ACCEL=kvm; ACCEL_OPTS=(-accel kvm); fi
case "$ARCH" in
  x86_64)
    QEMU=qemu-system-x86_64
    if [ "$FW" = uefi ]; then
      CODE="$(find_fw /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/qemu/OVMF.fd)" || { echo "OVMF firmware not found" >&2; exit 1; }
      VARS_SRC="${CODE%CODE*}VARS${CODE##*CODE}"
      cp "$VARS_SRC" "$WORK/vars.fd" 2>/dev/null || cp "$CODE" "$WORK/vars.fd"
      MACHINE=(-machine q35 -drive if=pflash,format=raw,readonly=on,file="$CODE" -drive if=pflash,format=raw,file="$WORK/vars.fd")
    else
      MACHINE=(-machine pc)
    fi
    CPU=(-cpu max -smp 2)
    [ "$ACCEL" = kvm ] && CPU=(-cpu host -smp 2)
    VIDEO=(-vga std)
    ;;
  aarch64)
    QEMU=qemu-system-aarch64
    CODE="$(find_fw /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd /usr/share/edk2/aarch64/QEMU_EFI.fd)" || { echo "AAVMF firmware not found" >&2; exit 1; }
    truncate -s 64M "$WORK/code.fd"; dd if="$CODE" of="$WORK/code.fd" conv=notrunc status=none
    truncate -s 64M "$WORK/vars.fd"
    MACHINE=(-machine virt -drive if=pflash,format=raw,readonly=on,file="$WORK/code.fd" -drive if=pflash,format=raw,file="$WORK/vars.fd")
    CPU=(-cpu max -smp 2)
    [ "$ACCEL" = kvm ] && CPU=(-cpu host -smp 2)
    VIDEO=(-device virtio-gpu-pci)
    ;;
  *) usage ;;
esac

NETDEV=(-nic none)
[ "$NET" = 1 ] && NETDEV=(-netdev user,id=n0 -device virtio-net-pci,netdev=n0,romfile=)

CONSOLEDEV=(-serial file:"$LOG")
if [ "$CONSOLE" = virtio ]; then
  CONSOLEDEV=(-serial none -device virtio-serial-pci -chardev file,id=hvc0,path="$LOG" -device virtconsole,chardev=hvc0)
fi

MEDIA=()
if [ -n "$DISK" ]; then
  FMT=raw
  case "$DISK" in *.qcow2) FMT=qcow2 ;; esac
  MEDIA+=(-drive file="$DISK",if=virtio,format="$FMT")
fi
if [ -n "$ISO" ]; then
  if [ "$ARCH" = aarch64 ]; then
    MEDIA+=(-drive file="$ISO",media=cdrom,if=none,id=cd,format=raw -device virtio-scsi-pci -device scsi-cd,drive=cd,bootindex=0)
  else
    MEDIA+=(-cdrom "$ISO" -boot d)
  fi
fi

echo "== boot ($ARCH, $FW, accel=$ACCEL, console=$CONSOLE) ${ISO:+iso=$ISO }${DISK:+disk=$DISK }waiting up to ${TMO}s for '$OK'"
"$QEMU" "${MACHINE[@]}" "${ACCEL_OPTS[@]}" "${CPU[@]}" -m "$MEM" "${VIDEO[@]}" -display none -no-reboot \
  -monitor unix:"$MON",server,nowait "${NETDEV[@]}" -rtc base=utc "${CONSOLEDEV[@]}" "${MEDIA[@]}" &
QPID=$!

all_present() {
  for m in "$OK" "${ALSO[@]}"; do grep -q -- "$m" "$LOG" 2>/dev/null || return 1; done
}

t=0 rc=1
while [ "$t" -lt "$TMO" ]; do
  if all_present; then rc=0; break; fi
  if [ -n "$FAIL" ] && grep -q -- "$FAIL" "$LOG" 2>/dev/null; then break; fi
  kill -0 "$QPID" 2>/dev/null || break
  sleep 5; t=$((t + 5))
done
if [ -n "$SHOT" ] && [ -S "$MON" ]; then
  sleep 5
  printf 'screendump %s/screen.ppm\n' "$WORK" | socat -t 2 - unix-connect:"$MON" >/dev/null 2>&1 || true
  sleep 2
  if [ -s "$WORK/screen.ppm" ]; then
    if command -v pnmtopng >/dev/null 2>&1; then pnmtopng "$WORK/screen.ppm" > "$SHOT" 2>/dev/null; else cp "$WORK/screen.ppm" "${SHOT%.png}.ppm"; fi
  fi
fi
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

echo "----- guest console tail -----"
tail -n 100 "$LOG" 2>/dev/null || true
if [ "$rc" -ne 0 ]; then
  for m in "$OK" "${ALSO[@]}"; do
    grep -q -- "$m" "$LOG" 2>/dev/null || echo "BOOT FAILED: '$m' not found within ${TMO}s" >&2
  done
  exit 1
fi
echo "BOOT OK: found '$OK'${ALSO[*]:+ and ${ALSO[*]}}"
