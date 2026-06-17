#!/usr/bin/env python3

import os

import libcalamares
from libcalamares.utils import debug


def run():
    conf = libcalamares.job.configuration
    storage = libcalamares.globalstorage

    root_mount_point = storage.value("rootMountPoint")
    if not root_mount_point:
        return ("No mount point", "rootMountPoint is not set in global storage.")

    runsvdir = conf.get("runsvdir", "/etc/runit/runsvdir/current")
    enable = list(conf.get("enable") or [])
    disable = set(conf.get("disable") or [])

    sv_dir = os.path.join(root_mount_point, "etc/runit/sv")
    current = os.path.normpath(root_mount_point + runsvdir)
    os.makedirs(current, exist_ok=True)

    if not os.path.isdir(sv_dir):
        return ("No service tree", "Service definitions were not found at /etc/runit/sv in the target.")

    for name in enable:
        if name in disable:
            continue
        source = os.path.join(sv_dir, name)
        if not os.path.isdir(source):
            debug("skipping missing service {}".format(name))
            continue
        link = os.path.join(current, name)
        if os.path.islink(link) or os.path.exists(link):
            continue
        os.symlink(os.path.join("/etc/runit/sv", name), link)

    for name in disable:
        link = os.path.join(current, name)
        if os.path.islink(link):
            os.unlink(link)

    return None
