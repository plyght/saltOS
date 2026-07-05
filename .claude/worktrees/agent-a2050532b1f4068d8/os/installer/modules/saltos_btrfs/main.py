#!/usr/bin/env python3

import os
import subprocess

import libcalamares
from libcalamares.utils import debug


SUBVOL_ORDER = ["@", "@home", "@var", "@log", "@snapshots"]


def find_btrfs_root_device():
    storage = libcalamares.globalstorage
    partitions = storage.value("partitions") or []
    for partition in partitions:
        if partition.get("mountPoint") == "/" and partition.get("fs") == "btrfs":
            return partition.get("device"), partition
    for partition in partitions:
        if partition.get("mountPoint") == "/":
            return partition.get("device"), partition
    return None, None


def mount_options(partition, conf, top_dev):
    is_ssd = False
    try:
        base = os.path.basename(top_dev).rstrip("0123456789")
        rotational = "/sys/block/{}/queue/rotational".format(base)
        if os.path.exists(rotational):
            with open(rotational) as handle:
                is_ssd = handle.read().strip() == "0"
    except OSError:
        is_ssd = False
    if is_ssd:
        return conf.get("mountOptionsSsd", "rw,noatime,compress=zstd:1,ssd,space_cache=v2,discard=async")
    return conf.get("mountOptions", "rw,noatime,compress=zstd:1,space_cache=v2")


def run():
    conf = libcalamares.job.configuration
    storage = libcalamares.globalstorage

    device, partition = find_btrfs_root_device()
    if device is None:
        return ("No root device", "No Btrfs root partition was found in global storage.")

    root_mount_point = storage.value("rootMountPoint")
    if not root_mount_point:
        return ("No mount point", "rootMountPoint is not set in global storage.")

    subvolumes = conf.get("subvolumes") or {}
    options = mount_options(partition, conf, device)

    tmp_top = "/tmp/saltos-btrfs-top"
    os.makedirs(tmp_top, exist_ok=True)

    try:
        subprocess.check_call(["umount", "-R", root_mount_point])
    except subprocess.CalledProcessError:
        debug("root mount point was not mounted, continuing")

    subprocess.check_call(["mount", "-o", "rw,noatime", device, tmp_top])

    try:
        for name in SUBVOL_ORDER:
            if name not in subvolumes:
                continue
            target = os.path.join(tmp_top, name)
            if not os.path.exists(target):
                subprocess.check_call(["btrfs", "subvolume", "create", target])
    finally:
        subprocess.check_call(["umount", tmp_top])

    os.makedirs(root_mount_point, exist_ok=True)
    subprocess.check_call(
        ["mount", "-o", "{},subvol=@".format(options), device, root_mount_point]
    )

    for name in SUBVOL_ORDER:
        mount_point = subvolumes.get(name)
        if name == "@" or mount_point is None:
            continue
        target = os.path.normpath(root_mount_point + mount_point)
        os.makedirs(target, exist_ok=True)
        subprocess.check_call(
            ["mount", "-o", "{},subvol={}".format(options, name), device, target]
        )

    extra = storage.value("extraMounts") or []
    extra.append({"device": device, "fs": "btrfs", "mountPoint": "/", "options": "{},subvol=@".format(options)})
    storage.insert("extraMounts", extra)

    return None
