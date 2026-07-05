#!/usr/bin/env python3

import os

import libcalamares
from libcalamares.utils import target_env_call, debug


def salt_arch():
    machine = os.uname().machine
    if machine in ("arm64", "aarch64"):
        return "aarch64"
    if machine in ("amd64", "x86_64"):
        return "x86_64"
    return machine


def boot_device():
    storage = libcalamares.globalstorage
    partitions = storage.value("partitions") or []
    disk = storage.value("bootLoader")
    if isinstance(disk, dict) and disk.get("installPath"):
        return disk.get("installPath")
    for partition in partitions:
        if partition.get("mountPoint") == "/":
            device = partition.get("device", "")
            return device.rstrip("0123456789").rstrip("p")
    return None


def firmware_is_efi():
    return os.path.isdir("/sys/firmware/efi")


def run():
    conf = libcalamares.job.configuration
    storage = libcalamares.globalstorage

    arch = storage.value("saltArch") or salt_arch()
    efi_targets = conf.get("efiTargets") or {}
    efi_target = efi_targets.get(arch)
    bios_target = conf.get("biosTarget", "i386-pc")
    efi_directory = conf.get("efiDirectory", "/boot/efi")
    bootloader_id = conf.get("bootloaderId", "saltOS")
    grub_install = conf.get("grubInstall", "grub-install")
    grub_mkconfig = conf.get("grubMkconfig", "grub-mkconfig")
    grub_cfg = conf.get("grubCfg", "/boot/grub/grub.cfg")

    if firmware_is_efi() and efi_target:
        rc = target_env_call([
            grub_install,
            "--target={}".format(efi_target),
            "--efi-directory={}".format(efi_directory),
            "--bootloader-id={}".format(bootloader_id),
            "--recheck",
        ])
        if rc != 0:
            return ("GRUB install failed", "grub-install (EFI) exited with status {}.".format(rc))
    elif arch == "x86_64":
        device = boot_device()
        if not device:
            return ("No boot device", "Could not determine the disk for BIOS GRUB installation.")
        rc = target_env_call([
            grub_install,
            "--target={}".format(bios_target),
            "--recheck",
            device,
        ])
        if rc != 0:
            return ("GRUB install failed", "grub-install (BIOS) exited with status {}.".format(rc))
    else:
        return ("Unsupported firmware", "aarch64 requires UEFI firmware for GRUB installation.")

    rc = target_env_call([grub_mkconfig, "-o", grub_cfg])
    if rc != 0:
        return ("GRUB config failed", "grub-mkconfig exited with status {}.".format(rc))

    rc = target_env_call(["salt", "deployments", "--register-current"])
    if rc != 0:
        debug("salt deployments registration returned {}".format(rc))

    return None
