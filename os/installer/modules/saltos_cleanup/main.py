#!/usr/bin/env python3

import os
import shutil

import libcalamares
from libcalamares.utils import debug, target_env_call


def getty_run_script(name):
    tty = name[len("agetty-"):] if name.startswith("agetty-") else name
    return "#!/bin/sh\nexec 2>&1\nexec agetty --noclear {} 38400 linux\n".format(tty)


def installed_real_user(root_mount_point, live_user):
    passwd = os.path.join(root_mount_point, "etc/passwd")
    if not os.path.exists(passwd):
        return None
    try:
        with open(passwd) as handle:
            for line in handle:
                fields = line.split(":")
                if len(fields) < 4:
                    continue
                name = fields[0]
                try:
                    uid = int(fields[2])
                except ValueError:
                    continue
                if name == live_user:
                    continue
                if 1000 <= uid < 60000:
                    return name
    except OSError:
        return None
    return None


def has_user(root_mount_point, user):
    passwd = os.path.join(root_mount_point, "etc/passwd")
    if not os.path.exists(passwd):
        return False
    try:
        with open(passwd) as handle:
            return any(line.split(":", 1)[0] == user for line in handle)
    except OSError:
        return False


def run():
    conf = libcalamares.job.configuration
    storage = libcalamares.globalstorage

    root_mount_point = storage.value("rootMountPoint")
    if not root_mount_point:
        return ("No mount point", "rootMountPoint is not set in global storage.")

    live_user = conf.get("liveUser", "salt")
    sv_dir = os.path.join(root_mount_point, "etc/runit/sv")

    for name in conf.get("gettys") or []:
        run_path = os.path.join(sv_dir, name, "run")
        if os.path.isfile(run_path):
            with open(run_path, "w") as handle:
                handle.write(getty_run_script(name))
            os.chmod(run_path, 0o755)
            debug("reset getty {} to non-autologin".format(name))

    runsvdir = os.path.normpath(root_mount_point + "/etc/runit/runsvdir/current")
    for name in conf.get("removeServices") or []:
        link = os.path.join(runsvdir, name)
        if os.path.islink(link):
            os.unlink(link)
        service = os.path.join(sv_dir, name)
        if os.path.isdir(service):
            shutil.rmtree(service, ignore_errors=True)
            debug("removed live-only service {}".format(name))

    for name in conf.get("sudoersDrops") or []:
        drop = os.path.join(root_mount_point, "etc/sudoers.d", name)
        if os.path.exists(drop):
            os.remove(drop)
            debug("removed sudoers drop-in {}".format(name))

    real_user = installed_real_user(root_mount_point, live_user)
    removed_live_user = False
    if conf.get("removeLiveUser", True) and real_user and has_user(root_mount_point, live_user):
        rc = target_env_call(["userdel", "-r", live_user])
        if rc != 0:
            rc = target_env_call(["userdel", live_user])
        removed_live_user = rc == 0
        if removed_live_user:
            debug("removed live user {} (installed user is {})".format(live_user, real_user))
        else:
            debug("could not remove live user {}".format(live_user))

    if not removed_live_user:
        home = os.path.join(root_mount_point, "home", live_user)
        for rel in conf.get("removeFromHome") or []:
            target = os.path.join(home, rel)
            if os.path.lexists(target):
                if os.path.isdir(target) and not os.path.islink(target):
                    shutil.rmtree(target, ignore_errors=True)
                else:
                    os.remove(target)
                debug("removed live artifact {} from {}".format(rel, live_user))

    return None
