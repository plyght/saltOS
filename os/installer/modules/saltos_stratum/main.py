#!/usr/bin/env python3

import os
import shutil

import libcalamares
from libcalamares.utils import debug, target_env_call


def run():
    conf = libcalamares.job.configuration
    storage = libcalamares.globalstorage

    root_mount_point = storage.value("rootMountPoint")
    if not root_mount_point:
        return ("No mount point", "rootMountPoint is not set in global storage.")

    strata_conf_dir = conf.get("strataConfDir", "/etc/salt/strata")
    strata_root = conf.get("strataRoot", "/strata")
    shims_dir = conf.get("shimsDir", "/usr/local/salt/shims")
    profile_script = conf.get("profileScript", "/etc/profile.d/salt-shims.sh")

    for path in (strata_conf_dir, strata_root, shims_dir):
        target = os.path.normpath(root_mount_point + path)
        os.makedirs(target, exist_ok=True)

    target_profile = os.path.normpath(root_mount_point + profile_script)
    if not os.path.exists(target_profile) and os.path.exists(profile_script):
        os.makedirs(os.path.dirname(target_profile), exist_ok=True)
        shutil.copy2(profile_script, target_profile)

    src_recipes = conf.get("recipeSource", "/etc/salt/strata")
    if os.path.isdir(src_recipes):
        dest = os.path.normpath(root_mount_point + strata_conf_dir)
        os.makedirs(dest, exist_ok=True)
        for name in os.listdir(src_recipes):
            if not name.endswith(".toml"):
                continue
            target = os.path.join(dest, name)
            if not os.path.exists(target):
                shutil.copy2(os.path.join(src_recipes, name), target)

    rc = target_env_call(["salt", "--root", "/", "stratum", "list"])
    if rc != 0:
        debug("salt stratum list returned {} on the target".format(rc))

    return None
