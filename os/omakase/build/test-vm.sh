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
WALLPAPER_TIMEOUT="${WALLPAPER_TIMEOUT:-120}"
THEME="${THEME:-gruvbox}"
USERNAME="${OMAKASE_USER:-salt}"
PASSWORD="${OMAKASE_PASSWORD:-saltos}"
DISTRO="${OMAKASE_DISTRO:-arch}"
MODE="${OMAKASE_TEST_MODE:-erase}"
ROLLBACK="${OMAKASE_ROLLBACK:-auto}"
SALT_BIN="${SALT_BIN:-}"

case "$MODE" in
  erase) ENCRYPT=false; INSTALL_MODE=disk; INTERACTIVE=false ;;
  encrypt) ENCRYPT=true; INSTALL_MODE=disk; INTERACTIVE=false ;;
  alongside) ENCRYPT=false; INSTALL_MODE=free; INTERACTIVE=false ;;
  interactive) ENCRYPT=true; INSTALL_MODE=disk; INTERACTIVE=true ;;
  *) echo "test-vm: unknown OMAKASE_TEST_MODE $MODE (erase|encrypt|alongside|interactive)" >&2; exit 1 ;;
esac
if [ "$ROLLBACK" = auto ]; then
  ROLLBACK=0
  [ "$ENCRYPT" = true ] && ROLLBACK=1
fi
if [ "$ROLLBACK" = 1 ]; then
  [ -n "$SALT_BIN" ] || for b in "$PWD/build/src/salt/salt" "$PWD/build-iso/src/salt/salt"; do
    [ -x "$b" ] && { SALT_BIN="$b"; break; }
  done
  [ -x "$SALT_BIN" ] || { echo "test-vm: rollback check needs SALT_BIN (a host salt binary)" >&2; exit 1; }
  SALT_BIN="$(readlink -f "$SALT_BIN")"
fi

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
    VGA_DEV=(-device virtio-vga)
    CDROM_DEV=(-device "ide-cd,drive=cd,bootindex=0")
    ;;
  aarch64)
    QEMU=qemu-system-aarch64
    for c in /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd /usr/share/edk2/aarch64/QEMU_EFI-pflash.raw; do
      [ -f "$c" ] && { CODE="$c"; break; }
    done
    for v in /usr/share/AAVMF/AAVMF_VARS.fd /usr/share/edk2/aarch64/vars-template-pflash.raw; do
      [ -f "$v" ] && { VARS_SRC="$v"; break; }
    done
    [ -n "${CODE:-}" ] && [ -n "${VARS_SRC:-}" ] || { echo "test-vm: AAVMF firmware not found" >&2; exit 1; }
    if [ "$(uname -m)" = aarch64 ] && [ -w /dev/kvm ]; then
      MACHINE=(-machine "virt,accel=kvm,gic-version=max" -cpu host)
    else
      MACHINE=(-machine virt -accel "tcg,thread=multi" -cpu cortex-a72)
    fi
    VGA_DEV=(-device virtio-gpu-pci)
    CDROM_DEV=(-device "virtio-scsi-pci,id=scsi0" -device "scsi-cd,bus=scsi0.0,drive=cd,bootindex=0")
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
    if grep -a -q "$ok" "$log" 2>/dev/null; then return 0; fi
    if [ -n "$fail" ] && grep -a -q "$fail" "$log" 2>/dev/null; then return 1; fi
    kill -0 "$pid" 2>/dev/null || return 2
    sleep 5
    n=$((n + 5))
  done
  return 3
}

SERIAL_LOG=""
SERIAL_POS=0
serial_send() { printf '%s\r' "$1" | socat -T 2 - UNIX-CONNECT:serial.sock >/dev/null 2>&1 || true; }
serial_type() { printf '%s' "$1" | socat -T 2 - UNIX-CONNECT:serial.sock >/dev/null 2>&1 || true; }
serial_plain() { sed 's/\x1b\[[0-9;?]*[A-Za-z]//g; s/\x1b[^[]//g' "$SERIAL_LOG" 2>/dev/null >serial-plain.txt; }
serial_expect() {
  local pattern="$1" timeout="${2:-120}" n=0 hit off
  while [ $n -lt "$timeout" ]; do
    serial_plain
    hit=$(tail -c +"$((SERIAL_POS + 1))" serial-plain.txt | LC_ALL=C grep -a -b -o -m1 -- "$pattern" | head -1)
    if [ -n "$hit" ]; then
      off=${hit%%:*}
      SERIAL_POS=$((SERIAL_POS + off + ${#hit} - ${#off} - 1))
      return 0
    fi
    kill -0 "$QPID" 2>/dev/null || { echo "qemu exited while waiting for: $pattern"; return 2; }
    sleep 1
    n=$((n + 1))
  done
  echo "timeout waiting for: $pattern"
  return 3
}
stop_vm() { kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; QPID=""; }

make_windows_disk() {
  local raw=windows.raw esp=esp.img ntfs=ntfs.img
  rm -f "$raw" "$esp" "$ntfs"
  truncate -s "$DISK_SIZE" "$raw"
  sgdisk -o -n 1:2048:+100M -t 1:ef00 -c 1:"EFI system partition" \
    -n 2:0:+3G -t 2:0700 -c 2:"Basic data partition" "$raw" >/dev/null
  truncate -s 100M "$esp"
  mkfs.vfat -F 32 -n SYSTEM "$esp" >/dev/null
  mmd -i "$esp" ::EFI ::EFI/Microsoft ::EFI/Microsoft/Boot ::EFI/Boot
  head -c 65536 /dev/urandom >bootmgfw.efi
  head -c 32768 /dev/urandom >BCD
  mcopy -i "$esp" bootmgfw.efi ::EFI/Microsoft/Boot/bootmgfw.efi
  mcopy -i "$esp" BCD ::EFI/Microsoft/Boot/BCD
  local fallback=bootx64.efi
  [ "$ARCH" = aarch64 ] && fallback=bootaa64.efi
  mcopy -i "$esp" bootmgfw.efi "::EFI/Boot/$fallback"
  truncate -s 3G "$ntfs"
  mkntfs -F -Q -q -L Windows "$ntfs" >/dev/null 2>&1
  ESP_START=$(sgdisk -i 1 "$raw" | awk -F': ' '/First sector/ {print $2+0}')
  NTFS_START=$(sgdisk -i 2 "$raw" | awk -F': ' '/First sector/ {print $2+0}')
  NTFS_SECTORS=$(sgdisk -i 2 "$raw" | awk -F': ' '/Partition size/ {print $2+0}')
  dd if="$esp" of="$raw" bs=512 seek="$ESP_START" conv=notrunc,sparse status=none
  dd if="$ntfs" of="$raw" bs=512 seek="$NTFS_START" conv=notrunc,sparse status=none
  rm -f "$esp" "$ntfs" bootmgfw.efi BCD
  echo "$ESP_START $NTFS_START $NTFS_SECTORS" >windows.geometry
  disk_fingerprint "$raw" >before.txt
  qemu-img convert -q -f raw -O qcow2 "$raw" target.qcow2
  rm -f "$raw"
}

disk_fingerprint() {
  local raw="$1" p
  read -r ESP_START NTFS_START NTFS_SECTORS <windows.geometry
  echo "disk guid: $(sgdisk -p "$raw" | awk '/Disk identifier/ {print $NF}')"
  for p in 1 2; do
    sgdisk -i "$p" "$raw" | grep -E 'GUID|First sector|Last sector|Partition name'
  done
  echo "ntfs sha256: $(dd if="$raw" bs=512 skip="$NTFS_START" count="$NTFS_SECTORS" status=none | sha256sum | cut -d' ' -f1)"
  local off=$((ESP_START * 512))
  echo "bootmgfw sha256: $(mtype -i "$raw@@$off" ::EFI/Microsoft/Boot/bootmgfw.efi | sha256sum | cut -d' ' -f1)"
  echo "bcd sha256: $(mtype -i "$raw@@$off" ::EFI/Microsoft/Boot/BCD | sha256sum | cut -d' ' -f1)"
}

build_rollback_repo() {
  local work=rollback-work
  rm -rf "$work" repo
  mkdir -p "$work/src" "$work/recipes/hello-1.0" "$work/keys" repo
  touch "$work/src/.keep"
  cat >"$work/recipes/hello-1.0/recipe.lua" <<EOF
return {
  name = "hello",
  version = "1.0",
  release = 1,
  arch = { "x86_64", "aarch64" },
  summary = "omakase rollback test package",
  license = "MIT",
  source = {
    url = "file://$PWD/$work/src",
    sha256 = "",
  },
  build = {
    system = "custom",
    script = [[
mkdir -p "\$SALT_DEST/usr/bin"
{ echo "#!/bin/sh"; echo "echo SALTOS_HELLO_1.0"; } >"\$SALT_DEST/usr/bin/hello"
chmod +x "\$SALT_DEST/usr/bin/hello"
]],
  },
  package = {
    deps = {},
  },
}
EOF
  SALT_OUT="$PWD/$work/out" SALT_WORK="$PWD/$work/work" "$SALT_BIN" build "$work/recipes/hello-1.0" >"$work/build.log" 2>&1 \
    || { cat "$work/build.log"; echo "test-vm: salt build failed" >&2; exit 1; }
  "$SALT_BIN" keygen "$work/keys" repo >/dev/null
  mkdir -p "repo/$ARCH/packages"
  cp "$work/out/$ARCH/packages/hello-1.0-1-$ARCH.grain" "repo/$ARCH/packages/"
  "$SALT_BIN" --key "$(head -1 "$work/keys/repo.sec")" repo publish "repo/$ARCH" >/dev/null
  cp "$work/keys/repo.pub" repo/repo.pub
}

echo "==> preparing target disk and cidata ($MODE)"
rm -f target.qcow2 cidata.img
if [ "$MODE" = alongside ]; then
  make_windows_disk
  echo "fake Windows layout:"; cat before.txt
else
  qemu-img create -q -f qcow2 target.qcow2 "$DISK_SIZE"
fi
cat >install.lua <<EOF
return {
  system = {
    hostname = "saltos-omakase",
    locale = "en_US.UTF-8",
    timezone = "UTC",
    keymap = "us",
  },
  kernel = { source = "native" },
  stratum = {
    { name = "$DISTRO", role = "primary", expose = true },
  },
  user = {
    username = "$USERNAME",
    full_name = "saltOS Tester",
    email = "",
    deferred = false,
  },
  install = {
    profile = "omakase",
    disk = "/dev/vda",
    mode = "$INSTALL_MODE",
    encrypt = $ENCRYPT,
    serial_console = true,
  },
}
EOF
printf 'password=%s\n' "$PASSWORD" >credentials
truncate -s 16M cidata.img
mkfs.vfat -n cidata cidata.img >/dev/null
if [ "$INTERACTIVE" = false ]; then
  mcopy -i cidata.img install.lua ::install.lua
  mcopy -i cidata.img credentials ::credentials
fi
if [ "$ROLLBACK" = 1 ]; then
  build_rollback_repo
  mcopy -s -i cidata.img repo ::repo
fi

CIDATA_DRIVE=()
[ "$INTERACTIVE" = true ] || CIDATA_DRIVE=(-drive "file=cidata.img,if=virtio,format=raw,readonly=on")
echo "==> phase 1: $([ "$INTERACTIVE" = true ] && echo interactive || echo unattended) install from $ISO"
rm -f install-serial.log serial.sock qmon
"$QEMU" "${MACHINE[@]}" -m "$MEM" -smp "$CPUS" -no-reboot -display none \
  -drive if=pflash,format=raw,readonly=on,file="$CODE" \
  -drive if=pflash,format=raw,file=ovmf-vars.fd \
  "${VGA_DEV[@]}" -device virtio-rng-pci \
  -drive file=target.qcow2,if=virtio,format=qcow2 \
  ${CIDATA_DRIVE[@]+"${CIDATA_DRIVE[@]}"} \
  -drive file="$ISO",media=cdrom,if=none,id=cd "${CDROM_DEV[@]}" \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -chardev socket,id=ser0,path=serial.sock,server=on,wait=off,logfile=install-serial.log \
  -serial chardev:ser0 -monitor unix:qmon,server,nowait &
QPID=$!
set +e
if [ "$INTERACTIVE" = true ]; then
  SERIAL_LOG=install-serial.log
  SERIAL_POS=0
  GUM_SETTLE=6
  gum_ready() { serial_expect 'enter submit' 60 && sleep "$GUM_SETTLE"; }
  gum_confirm_ready() { serial_expect 'y Yes' 60 && sleep "$GUM_SETTLE"; }
  drive_configurator() {
    serial_expect 'saltos-live login: salt' 300 || return 1
    sleep 4
    serial_send "sudo saltos-live-install"
    serial_expect 'Press Return to Start Install' 60 || return 1
    serial_send ""
    serial_expect 'Keyboard layout' 60 || return 1
    gum_ready || return 1; serial_send ""
    serial_expect 'Your account' 60 || return 1
    gum_ready || return 1; serial_type "$USERNAME"; sleep 1; serial_send ""
    serial_expect 'password' 30 || return 1
    gum_ready || return 1; serial_type "$PASSWORD"; sleep 1; serial_send ""
    serial_expect 'confirm' 30 || return 1
    gum_ready || return 1; serial_type "$PASSWORD"; sleep 1; serial_send ""
    serial_expect 'full name' 30 || return 1
    gum_ready || return 1; serial_type "saltOS Tester"; sleep 1; serial_send ""
    serial_expect 'email' 30 || return 1
    gum_ready || return 1; serial_send ""
    serial_expect 'Hostname' 30 || return 1
    gum_ready || return 1; serial_send ""
    serial_expect 'Timezone' 30 || return 1
    gum_ready || return 1; serial_send ""
    serial_expect 'Base distribution' 30 || return 1
    gum_ready || return 1; serial_send ""
    serial_expect 'Does this look right' 30 || return 1
    gum_confirm_ready || return 1; serial_type "y"
    serial_expect 'Install disk' 30 || return 1
    gum_ready || return 1; serial_send ""
    serial_expect 'Install mode' 30 || return 1
    gum_ready || return 1; serial_send ""
    serial_expect 'Disk encryption' 30 || return 1
    gum_confirm_ready || return 1; serial_type "y"
    serial_expect 'Encryption: true' 30 || { echo "encryption toggle not reflected on the summary"; return 1; }
    gum_confirm_ready || return 1; serial_type "y"
    return 0
  }
  drive_configurator
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "CONFIGURATOR DRIVE FAILED (rc=$rc)"; tail -c 4000 install-serial.log | tr -d '\033'; stop_vm; exit 1
  fi
  echo "configurator driven over serial (encryption on)"
fi
wait_marker install-serial.log SALTOS_OMAKASE_INSTALL_OK SALTOS_OMAKASE_INSTALL_FAIL "$INSTALL_TIMEOUT" $QPID
rc=$?
if [ $rc -eq 0 ] && [ "$INTERACTIVE" = true ]; then
  gum_confirm_ready && serial_type "n"
  sleep 3
  serial_send "salt eval /run/saltos-install/install.lua | grep -E '^install\\.(encrypt|serial_console|mode|disk) ' | sed 's/^install\\./SALTOS_CFG /'"
  if ! serial_expect 'SALTOS_CFG serial_console = true' 30 ||
     ! grep -a -q 'SALTOS_CFG encrypt = true' install-serial.log; then
    echo "CONFIGURATOR OUTPUT MISMATCH"; grep -a 'SALTOS_CFG' install-serial.log; rc=1
  else
    echo "configurator wrote encrypt = true, serial_console = true"
  fi
fi
set -e
sleep 5
stop_vm
if [ $rc -ne 0 ]; then
  echo "INSTALL FAILED (rc=$rc)"; tail -n 80 install-serial.log; exit 1
fi
echo "install OK"
if [ "$MODE" = alongside ]; then
  echo "==> checking the pre-existing partitions"
  qemu-img convert -q -f qcow2 -O raw target.qcow2 after.raw
  disk_fingerprint after.raw >after.txt
  echo "partition table after install:"; sgdisk -p after.raw
  rm -f after.raw
  if ! diff -u before.txt after.txt; then
    echo "EXISTING PARTITIONS CHANGED"; exit 1
  fi
  echo "existing ESP + NTFS partitions untouched"
fi

boot_installed() {
  local phase="$1"
  echo "==> $phase: boot installed disk"
  rm -f boot-serial.log serial.sock qmon
  cp -f "$VARS_SRC" ovmf-vars.fd
  "$QEMU" "${MACHINE[@]}" -m "$MEM" -smp "$CPUS" -no-reboot -display none \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file=ovmf-vars.fd \
    "${VGA_DEV[@]}" -device virtio-rng-pci -device virtio-keyboard-pci -device virtio-mouse-pci \
    -drive file=target.qcow2,if=virtio,format=qcow2 \
    -drive file=cidata.img,if=virtio,format=raw,readonly=on \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -chardev socket,id=ser0,path=serial.sock,server=on,wait=off,logfile=boot-serial.log \
    -serial chardev:ser0 -monitor unix:qmon,server,nowait &
  QPID=$!
  SERIAL_LOG=boot-serial.log
  SERIAL_POS=0
  if [ "$ENCRYPT" = true ]; then
    if ! serial_expect 'Please unlock disk saltos-root' "$BOOT_TIMEOUT"; then
      echo "NO LUKS PASSPHRASE PROMPT"; tail -n 60 boot-serial.log; stop_vm; exit 1
    fi
    echo "LUKS passphrase prompt seen"
    sleep 2
    serial_send "$PASSWORD"
  fi
  set +e
  wait_marker boot-serial.log SALTOS_SWAY_SESSION_OK SALTOS_SWAY_SESSION_TIMEOUT "$BOOT_TIMEOUT" $QPID
  local rc=$?
  set -e
  if [ $rc -ne 0 ]; then
    echo "SWAY SESSION FAILED (rc=$rc)"; tail -n 80 boot-serial.log; stop_vm; exit 1
  fi
  echo "sway session OK"
  sleep 10
  serial_send ""
  sleep 3
}

run_checked() {
  local cmd="$1" marker="$2" timeout="${3:-90}" what="$4"
  serial_send "$cmd"
  set +e
  wait_marker boot-serial.log "$marker" "" "$timeout" $QPID
  local rc=$?
  set -e
  if [ $rc -ne 0 ]; then
    echo "$what FAILED (rc=$rc)"; tail -n 40 boot-serial.log; stop_vm; exit 1
  fi
  echo "$what OK"
}

sudo_cmd() { printf "printf '%%s\\\\n' '%s' | sudo -S -p '' %s" "$PASSWORD" "$1"; }

boot_installed "phase 2"
run_checked "saltos-theme set $THEME && echo SALTOS_THEME_OK \$(saltos-theme current)" "SALTOS_THEME_OK $THEME" 90 "theme switch"
run_checked "sleep 3; pgrep -a swaybg | grep -q -- \"-i /usr/share/saltos/wallpapers/$THEME/01-\" && echo SALTOS_SWAYBG_PHOTO_OK" SALTOS_SWAYBG_PHOTO_OK 60 "swaybg shows the theme's first Unsplash photo"
run_checked "saltos-wallpaper next && sleep 3 && pgrep -a swaybg | grep -q -- \"-i /usr/share/saltos/wallpapers/$THEME/\$(saltos-wallpaper current)\" && [ \"\$(saltos-wallpaper current)\" != \"\$(saltos-wallpaper list | head -1)\" ] && echo SALTOS_WALLPAPER_NEXT_OK" SALTOS_WALLPAPER_NEXT_OK 60 "saltos-wallpaper next cycles swaybg to the second photo"
run_checked "[ -s /usr/share/saltos/wallpapers/CREDITS ] && salt run $DISTRO grep -q unsplash.com /usr/share/saltos/wallpapers/CREDITS && echo SALTOS_WALLPAPER_CREDITS_OK" SALTOS_WALLPAPER_CREDITS_OK 60 "wallpaper CREDITS shipped on host and visible in the stratum"
if command -v pnmtopng >/dev/null && command -v identify >/dev/null; then
  colors=0 n=0
  while [ $n -lt "$WALLPAPER_TIMEOUT" ]; do
    sleep 5
    n=$((n + 5))
    rm -f wallpaper.ppm
    printf 'screendump wallpaper.ppm\n' | socat -T 2 - UNIX-CONNECT:qmon >/dev/null 2>&1 || true
    sleep 3
    [ -f wallpaper.ppm ] || continue
    pnmtopng wallpaper.ppm >wallpaper.png 2>/dev/null || continue
    colors="$(identify -format '%k' wallpaper.png 2>/dev/null || echo 0)"
    [ "$colors" -ge 2000 ] && break
  done
  [ -f wallpaper.png ] && echo "screenshot: $OUT/wallpaper.png"
  if [ "$colors" -lt 2000 ]; then
    echo "wallpaper screenshot has only $colors distinct colours after ${n}s: swaybg is not showing a photo FAILED"; stop_vm; exit 1
  fi
  echo "wallpaper screenshot has $colors distinct colours (a photo, not a solid fill) OK"
fi
run_checked "ps -o user=,supgrp= -C sway | grep -q '^$USERNAME .*seat' && echo SALTOS_SESSION_IDENTITY_OK" SALTOS_SESSION_IDENTITY_OK 60 "sway runs as $USERNAME with host groups (real uid, not a userns)"
run_checked "[ \"\$(salt run $DISTRO id -un)\" = $USERNAME ] && [ \"\$(salt run $DISTRO getent passwd \$(id -u) | cut -d: -f1)\" = $USERNAME ] && echo SALTOS_STRATUM_IDENTITY_OK" SALTOS_STRATUM_IDENTITY_OK 60 "$USERNAME's uid resolves to $USERNAME inside the $DISTRO stratum (no stock rootfs account)"
run_checked "SWAYSOCK=\$(ls /run/user/\$(id -u)/sway-ipc.*.sock | head -n1) salt run $DISTRO swaymsg exec \"sh -c 'sudo -n salt stratum list >/tmp/saltos-sudo-test 2>&1; echo rc=\\\$? >>/tmp/saltos-sudo-test'\" && sleep 6 && grep -q '^rc=0' /tmp/saltos-sudo-test && grep -q '^$DISTRO ' /tmp/saltos-sudo-test && echo SALTOS_SESSION_SUDO_OK" SALTOS_SESSION_SUDO_OK 90 "sudo salt works from inside the desktop session"
serial_send "$(sudo_cmd "sv status /etc/runit/runsvdir/current/*") 2>/dev/null | sed 's/^/SALTOS_SV /'"
sleep 5
serial_send "grep -c . ~/.config/saltos/theme/sway.conf | sed 's/^/SALTOS_THEME_LINES /'"
sleep 5
serial_send "for p in sway waybar mako swayidle vicinae-server pipewire wireplumber; do pgrep -x \"\$p\" >/dev/null && echo SALTOS_PROC \$p up || echo SALTOS_PROC \$p DOWN; done"
sleep 5
serial_send "tail -n 30 ~/.local/state/saltos/session.log | sed 's/^/SALTOS_SESSION_LOG /'"
sleep 5
serial_send "lsblk -o NAME,SIZE,TYPE,FSTYPE,PARTTYPENAME,MOUNTPOINTS | sed 's/^/SALTOS_LSBLK /'"
sleep 5
if [ "$ENCRYPT" = true ]; then
  run_checked "$(sudo_cmd "cryptsetup status saltos-root") | grep -q 'type: *LUKS2' && findmnt -no SOURCE / | grep -q /dev/mapper/saltos-root && echo SALTOS_LUKS_OK" SALTOS_LUKS_OK 60 "LUKS2 root active"
fi
if [ "$MODE" = alongside ]; then
  run_checked "$(sudo_cmd "grep -c 'menuentry .Windows Boot Manager' /boot/grub/grub.cfg") | sed 's/^/SALTOS_GRUB_WINDOWS /'" "SALTOS_GRUB_WINDOWS [1-9]" 60 "GRUB lists Windows Boot Manager"
  serial_send "$(sudo_cmd "grep -E '^menuentry|^\\s+menuentry' /boot/grub/grub.cfg") | sed 's/^/SALTOS_GRUB_ENTRY /'"
  sleep 5
  run_checked "ls /boot/efi/EFI/Microsoft/Boot/bootmgfw.efi /boot/efi/EFI/Microsoft/Boot/BCD >/dev/null && echo SALTOS_ESP_SHARED_OK" SALTOS_ESP_SHARED_OK 60 "shared ESP keeps the Windows loader"
fi
serial_send "SWAYSOCK=\$(ls /run/user/\$(id -u)/sway-ipc.*.sock | head -1) salt run $DISTRO swaymsg exec foot"
sleep 6
printf 'screendump screen.ppm\n' | socat -T 2 - UNIX-CONNECT:qmon >/dev/null 2>&1 || true
sleep 3
if [ -f screen.ppm ] && command -v pnmtopng >/dev/null; then
  pnmtopng screen.ppm >desktop.png 2>/dev/null && echo "screenshot: $OUT/desktop.png"
fi

if [ "$ROLLBACK" = 1 ]; then
  echo "==> rollback: install a grain from the cidata repo, roll it back, reboot"
  REPO_URL="file:///mnt/cidata/repo"
  run_checked "$(sudo_cmd "sh -c 'mkdir -p /mnt/cidata && mount -o ro \$(blkid -L cidata || blkid -L CIDATA) /mnt/cidata'") && echo SALTOS_CIDATA_MOUNTED" SALTOS_CIDATA_MOUNTED 60 "cidata mounted"
  run_checked "sudo -n salt --repo $REPO_URL --key /mnt/cidata/repo/repo.pub sync && sudo -n salt --repo $REPO_URL --key /mnt/cidata/repo/repo.pub --yes install hello && hello | sed 's/^/SALTOS_ROLLBACK_INSTALLED /'" "SALTOS_ROLLBACK_INSTALLED SALTOS_HELLO_1.0" 300 "grain install (deployment with snapshot)"
  serial_send "sudo -n salt deployments | sed 's/^/SALTOS_DEPLOYMENTS /'"
  sleep 5
  run_checked "sudo -n salt rollback; echo SALTOS_ROLLBACK_RC \$?" "SALTOS_ROLLBACK_RC 3" 300 "salt rollback"
  grep -a -q 'undid deployment' boot-serial.log || { echo "salt rollback did not report the undone deployment"; stop_vm; exit 1; }
  run_checked "sudo -n salt deployments | grep -q rollback && echo SALTOS_ROLLBACK_RECORDED" SALTOS_ROLLBACK_RECORDED 60 "rollback recorded as a deployment"
  serial_send "$(sudo_cmd "umount /mnt/cidata"); sync; $(sudo_cmd "reboot")"
  for _ in $(seq 1 24); do kill -0 "$QPID" 2>/dev/null || break; sleep 5; done
  stop_vm
  cp -f boot-serial.log boot-serial-1.log
  boot_installed "phase 3 (after rollback)"
  run_checked "command -v hello >/dev/null && echo SALTOS_ROLLBACK_STALE || echo SALTOS_ROLLBACK_OK" SALTOS_ROLLBACK_OK 60 "rolled-back root boots without the grain"
  serial_send "sudo -n salt deployments | sed 's/^/SALTOS_DEPLOYMENTS /'"
  sleep 5
  if [ "$ENCRYPT" = true ]; then
    run_checked "findmnt -no SOURCE / | grep -q /dev/mapper/saltos-root && echo SALTOS_LUKS_OK2" SALTOS_LUKS_OK2 60 "LUKS2 root active after rollback"
  fi
fi

stop_vm
grep -a -q "SALTOS_SV" boot-serial.log && echo "runit services listed"
echo "ALL OK ($MODE)"
