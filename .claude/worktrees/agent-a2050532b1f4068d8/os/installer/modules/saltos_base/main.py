#!/usr/bin/env python3

import os
import subprocess

import libcalamares
from libcalamares.utils import debug


def salt_arch():
    machine = os.uname().machine
    if machine in ("arm64", "aarch64"):
        return "aarch64"
    if machine in ("amd64", "x86_64"):
        return "x86_64"
    return machine


def run():
    conf = libcalamares.job.configuration
    storage = libcalamares.globalstorage

    root_mount_point = storage.value("rootMountPoint")
    if not root_mount_point:
        return ("No mount point", "rootMountPoint is not set in global storage.")

    repo = conf.get("repo", "/run/salt/repo")
    packages = list(conf.get("basePackages") or [])
    if conf.get("installDesktop", True):
        packages += list(conf.get("desktopPackages") or [])

    if not packages:
        return ("No packages", "No base packages were configured for saltos_base.")

    os.makedirs(os.path.join(root_mount_point, "var/lib/salt"), exist_ok=True)
    os.makedirs(os.path.join(root_mount_point, "etc/salt"), exist_ok=True)

    command = [
        "salt",
        "install",
        "--root", root_mount_point,
        "--repo", repo,
        "--yes",
    ] + packages

    debug("running: {}".format(" ".join(command)))

    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    total = float(len(packages))
    done = 0
    for raw in process.stdout:
        line = raw.decode("utf-8", "replace").rstrip()
        debug(line)
        if line.startswith("installed "):
            done += 1
            libcalamares.job.setprogress(min(0.99, done / total))
    code = process.wait()
    if code != 0:
        return ("Base install failed", "salt install exited with status {}.".format(code))

    db = os.path.join(root_mount_point, "var/lib/salt/db.sqlite")
    if not os.path.exists(db):
        return ("Missing database", "salt did not create /var/lib/salt/db.sqlite in the target.")

    libcalamares.job.setprogress(1.0)
    storage.insert("saltArch", salt_arch())
    return None
