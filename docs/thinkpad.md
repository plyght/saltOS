# saltOS on ThinkPad hardware

The `thinkpad` build profile turns the self-hosted saltOS base into an image that
boots on real laptop hardware rather than only under QEMU. It is built entirely
in GitHub Actions by the `thinkpad-image` workflow; nothing needs to be built
locally.

## What the profile adds

The default self-hosted profile builds a kernel from `defconfig` with a small
fragment, ships no modules and no firmware, and reaches the network only through
a BusyBox `wget` shim. None of that survives contact with a laptop. The
`thinkpad` profile changes five things.

**A real laptop kernel.** `os/selfhost/kernel-thinkpad-x86_64.config` is merged
over `defconfig` with `merge_config.sh` and covers NVMe, AHCI, xHCI, I2C-HID and
PS/2 touchpads, TrackPoint, Btrfs, EFI, suspend, battery and AC, backlight,
`intel_pstate` and `amd-pstate`, and the namespace, cgroup, and seccomp options
that strata need. Graphics, Wi-Fi, Bluetooth, audio, and Ethernet build as
modules so they load after the root filesystem is mounted.

Because `olddefconfig` silently drops symbols whose dependencies are unmet, the
build asserts every entry in `os/selfhost/kernel-required-x86_64.txt` against the
final `.config` and fails loudly rather than producing a kernel that quietly
cannot see the disk.

**Firmware.** `os/selfhost/firmware.sh` sparse-checks-out only the relevant parts
of linux-firmware (iwlwifi, i915, amdgpu, ath, rtw, mediatek, brcm, qca, intel,
microcode, `regulatory.db`) and xz-compresses them with a kernel-compatible
dictionary size. Without `regulatory.db` the wireless regulatory domain never
loads, so its absence is a hard build failure.

**Module loading.** Nothing in this rootfs probes hardware on its own, so
`salt-hw coldplug` walks `/sys/devices/**/modalias` and modprobes each match. It
runs twice, because some drivers only expose their buses after a first pass. It
is wired into runit stage 1, ahead of any service.

**A network stack.** `os/selfhost/net.sh` builds OpenSSL, libnl,
wpa_supplicant, and a real curl linked against OpenSSL, plus a CA bundle. This
replaces the BusyBox `wget` shim, so `salt sync` and stratum bootstrap get
genuine TLS. wpa_supplicant was chosen over iwd because iwd needs D-Bus purely
to run `iwctl`, while wpa_supplicant needs only libnl and OpenSSL — and OpenSSL
had to come in regardless.

**Partitioning and filesystem tools.** BusyBox `fdisk` writes only MBR and
BusyBox `mke2fs` makes only ext2. `os/selfhost/base-tools.sh` builds util-linux
and e2fsprogs so the installer can write a GPT label with correct type GUIDs and
a journaled ext4 root.

## Using it

Build and publish from Actions:

    gh workflow run thinkpad-image.yml

The workflow will not publish anything that has not passed three QEMU gates:

1. the live ISO boots to userspace, coldplug runs, and the hardware plane
   reports `SALTOS_THINKPAD_OK` (modules present, firmware present,
   wpa_supplicant runs, curl is OpenSSL-backed);
2. the installer writes a GPT disk and exits zero;
3. the installed disk boots under UEFI through OVMF and still reports a healthy
   hardware plane.

Then download the ISO from the release and write it to a USB stick:

    sudo dd if=saltos-0.1.0-selfhost-x86_64.iso of=/dev/sdX bs=4M status=progress oflag=sync

Boot it with Secure Boot disabled — saltOS ships no signed shim.

## On the machine

    salt-hw report                       detected hardware, drivers, missing firmware
    salt-wifi scan                       list nearby networks
    salt-wifi connect <ssid> <password>  join and request DHCP
    saltos-install                       erase a disk and install

Saved networks live in `/etc/wpa_supplicant.conf` and reconnect at boot through
the `wifi` runit service. Wired interfaces are handled separately by `netdhcp`,
which skips wireless devices so it does not race the supplicant.

`salt-hw report` is the first thing to run on unfamiliar hardware. Its "missing
firmware" section reads back the kernel's own failed firmware requests, which is
the fastest way to find a blob that needs adding to `firmware.sh`.

## Root device naming

The installer references the root filesystem by `PARTUUID`, not by `/dev/sdX` or
`/dev/nvme0n1p2`. Enumeration order is not stable once a second disk or a USB
stick is attached, and a laptop that boots correctly on the bench and fails with
a dock attached is the usual symptom.

## Scope

The image is console-only by design. It ships the hardware plane — kernel,
firmware, module loading, wireless, wired networking, audio, storage, and the
installer — and stops there. Choosing and installing a desktop is left to
whoever runs it.

`os/selfhost/desktop.sh` still exists and builds Xorg against the fbdev driver
with GLX, DRI, and glamor disabled. It is a proof that X can be built from
source, not an accelerated desktop, and the `thinkpad` profile does not use it.

A desktop on top of this base can come from either direction. A stratum is the
path the architecture is built for: the host stays self-hosted and the GUI
userland comes from Arch or Debian, exposed with `salt expose`. Building Mesa
and a modesetting Xorg from source is the self-hosted path, and needs libdrm,
Mesa, and — for a laptop-quality touchpad — libinput, which pulls in eudev for
libudev.
