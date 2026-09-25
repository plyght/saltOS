#!/usr/bin/env python3
"""Drive a booted saltOS x86_64 UEFI image through the native package-manager
lifecycle over its serial console.

usage: qemu_pkg_drive.py --image IMG --ovmf-code F --ovmf-vars F --log serial.log \
         --repo-url http://10.0.2.2:PORT --pubkey HEX

The host serves two signed repos under --repo-url: /v1 (hello 1.0, libgreet,
greeter -> libgreet) and /v2 (hello 1.1).  In order: sync against v1, install
with dependency resolution, refuse removing a dependency and honour --cascade,
update to v2, lock / lock diff / lock apply, verify and tamper detection,
config gc pruning btrfs snapshots, salt rollback (root subvolume swap, rc 3),
reboot into the rolled-back root and keep transacting there.  Every byte the
guest writes to ttyS0 lands in --log; the driver prints its assertions to
stdout and exits non-zero on the first failure.  The serial plumbing is shared
with os/ota/qemu-drive.py.
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "qemu_drive", os.path.join(HERE, "..", "os", "ota", "qemu-drive.py"))
qd = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(qd)
Serial, Fail, expect = qd.Serial, qd.Fail, qd.expect


def last_line(out):
    lines = [l for l in out.strip().splitlines() if l.strip()]
    return lines[-1].strip() if lines else ""


def snapshot_count(ser):
    _, out = ser.run("sudo -n btrfs subvolume list / | grep -c root-", check=0)
    return int(last_line(out))


def write_repo_conf(ser, source, pubkey):
    ser.run("printf 'return {\\n  repo = \"current\",\\n  source = \"%s\",\\n  key = \"%s\",\\n}\\n'"
            " | sudo -n tee /etc/salt/repo.lua" % (source, pubkey), check=0)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--image", required=True)
    p.add_argument("--format", default="raw")
    p.add_argument("--ovmf-code", required=True)
    p.add_argument("--ovmf-vars", required=True)
    p.add_argument("--log", required=True)
    p.add_argument("--repo-url", required=True, help="base URL as seen from the guest, e.g. http://10.0.2.2:8124")
    p.add_argument("--pubkey", required=True, help="hex public key the repo indexes are signed with")
    p.add_argument("--qemu", default="qemu-system-x86_64")
    p.add_argument("--mem", type=int, default=2048)
    p.add_argument("--boot-timeout", type=int, default=420)
    a = p.parse_args()

    tmp = tempfile.mkdtemp(prefix="saltos-pkg-")
    serial_path = os.path.join(tmp, "serial.sock")
    vars_copy = os.path.join(tmp, "OVMF_VARS.fd")
    shutil.copy(a.ovmf_vars, vars_copy)
    cmd = qd.qemu_cmd(a, serial_path, vars_copy)
    print("+ " + " ".join(cmd), flush=True)
    q = subprocess.Popen(cmd)
    ser = Serial(serial_path, a.log)
    rc = 1
    try:
        ser.connect(q)
        v1, v2 = a.repo_url + "/v1", a.repo_url + "/v2"

        ser.note("boot 1: factory image")
        ser.wait_login(a.boot_timeout)
        _, out = ser.run("stat -f -c %T /", check=0)
        expect("btrfs" in out, "root filesystem is btrfs")
        ser.run("sudo -n btrfs subvolume list /", check=0)
        qd.wait_network(ser, v1 + "/x86_64/index.toml", 180)
        write_repo_conf(ser, v1, a.pubkey)

        ser.note("sync + install with dependencies")
        _, out = ser.run("sudo -n salt sync", 120, check=0)
        expect("verified with trusted key" in out, "signed index verified against the configured key")
        _, out = ser.run("sudo -n salt --yes install hello greeter", 300, check=0)
        expect("libgreet" in out, "dependency libgreet pulled into the plan")
        _, out = ser.run("hello; greeter; libgreet", check=0)
        expect("hello 1.0" in out and "greeter 1.0" in out and "libgreet 1.0" in out,
               "installed binaries run from the live root")
        _, out = ser.run("sudo -n salt list --installed", check=0)
        expect(all(n in out for n in ("hello", "greeter", "libgreet")), "salt list --installed shows all three")
        _, out = ser.run("sudo -n salt owner /usr/bin/greeter", check=0)
        expect("greeter" in out, "salt owner resolves /usr/bin/greeter")
        _, out = ser.run("sudo -n salt info hello", check=0)
        expect("1.0" in out, "salt info hello reports 1.0")

        ser.note("remove refusal and --cascade")
        r, out = ser.run("sudo -n salt --yes remove libgreet", 120)
        expect(r != 0 and "greeter" in out, "removing a dependency is refused naming the dependant (rc %d)" % r)
        ser.run("test -x /usr/bin/libgreet", check=0)
        _, out = ser.run("sudo -n salt --yes remove --cascade libgreet", 120, check=0)
        expect("greeter" in out, "--cascade removes the dependant too")
        ser.run("test ! -e /usr/bin/greeter -a ! -e /usr/bin/libgreet", check=0)
        print("ok: cascade removed both binaries", flush=True)
        ser.run("sudo -n salt --yes install greeter", 300, check=0)

        ser.note("update from repo v2")
        write_repo_conf(ser, v2, a.pubkey)
        ser.run("sudo -n salt sync", 120, check=0)
        _, out = ser.run("sudo -n salt list --upgradable", check=0)
        expect("hello" in out and "1.1" in out, "hello 1.1 is listed as upgradable")
        _, out = ser.run("sudo -n salt --yes update", 300, check=0)
        expect("1.0-1 -> 1.1-1" in out, "update plan shows hello 1.0-1 -> 1.1-1")
        _, out = ser.run("hello", check=0)
        expect("hello 1.1" in out, "updated binary active on the live root")
        _, out = ser.run("sudo -n salt deployments", check=0)
        expect("root-" in out and "update" in out, "deployments record their btrfs snapshots")
        expect(snapshot_count(ser) >= 3, "btrfs snapshots exist under @snapshots")

        ser.note("lock / lock diff / lock apply")
        ser.run("sudo -n salt lock --output /tmp/system.lock.toml", 60, check=0)
        ser.run("grep -q 'name = \"hello\"' /tmp/system.lock.toml", check=0)
        ser.run("sudo -n salt lock diff /tmp/system.lock.toml", 60, check=0)
        print("ok: lock diff is clean right after lock", flush=True)
        ser.run("sudo -n salt --yes remove hello", 120, check=0)
        r, out = ser.run("sudo -n salt lock diff /tmp/system.lock.toml", 60)
        expect(r != 0 and "hello" in out, "lock diff reports the missing hello (rc %d)" % r)
        _, out = ser.run("sudo -n salt --yes lock apply /tmp/system.lock.toml", 300, check=0)
        expect("hello" in out, "lock apply reinstalls hello")
        ser.run("sudo -n salt lock diff /tmp/system.lock.toml", 60, check=0)
        _, out = ser.run("hello", check=0)
        expect("hello 1.1" in out, "lock apply restored the exact version")

        ser.note("verify + tamper detection")
        ser.run("sudo -n salt verify hello", check=0)
        ser.run("echo tampered | sudo -n tee -a /usr/bin/hello >/dev/null", check=0)
        r, out = ser.run("sudo -n salt verify hello")
        expect(r != 0 and "hello" in out, "verify detects the tampered file (rc %d)" % r)
        ser.run("sudo -n salt --yes remove hello", 120, check=0)
        ser.run("sudo -n salt --yes install hello", 300, check=0)
        ser.run("sudo -n salt verify hello", check=0)
        print("ok: reinstall restored the file", flush=True)

        ser.note("config gc on btrfs snapshots")
        before = snapshot_count(ser)
        ser.run("sudo -n salt config gc --keep 3 --dry-run", 60, check=0)
        expect(snapshot_count(ser) == before, "gc --dry-run deleted nothing")
        _, out = ser.run("sudo -n salt --yes config gc --keep 3", 120, check=0)
        expect("pruned generation" in out and "freed" in out, "gc pruned old generations")
        after = snapshot_count(ser)
        expect(after < before, "btrfs snapshots deleted (%d -> %d)" % (before, after))
        ser.run("sudo -n salt verify hello", check=0)
        ser.run("sudo -n salt deployments", check=0)

        ser.note("rollback the last deployment -> root subvolume swap -> reboot")
        ser.run("sudo -n salt --yes remove greeter", 120, check=0)
        ser.run("test ! -e /usr/bin/greeter", check=0)
        r, out = ser.run("sudo -n salt rollback", 300)
        expect(r == 3 and "undid deployment" in out, "salt rollback staged a new root (rc 3)")
        ser.run("sudo -n btrfs subvolume list /", check=0)
        ser.run("sudo -n btrfs subvolume get-default /", check=0)
        ser.send("sudo -n reboot\n")
        ser.wait_login(a.boot_timeout)

        ser.note("boot 2: rolled-back root")
        _, out = ser.run("greeter", check=0)
        expect("greeter 1.0" in out, "greeter is back after rollback + reboot")
        _, out = ser.run("sudo -n salt list --installed", check=0)
        expect("greeter" in out and "hello" in out, "database matches the rolled-back root")
        _, out = ser.run("sudo -n salt deployments", check=0)
        expect("rollback" in out and "undone" in out, "rollback and undone deployments recorded")
        ser.run("sudo -n salt verify", 120, check=0)
        print("ok: whole root verifies against its database after rollback", flush=True)
        ser.run("sudo -n btrfs subvolume list /", check=0)
        ser.run("sudo -n salt history", check=0)
        ser.run("sudo -n salt --yes remove hello", 120, check=0)
        _, out = ser.run("sudo -n salt --yes install hello", 300, check=0)
        expect("transaction" in out and "complete" in out, "transactions keep working on the swapped root")
        ser.run("sudo -n salt --yes config gc --keep 2", 120, check=0)
        ser.run("sudo -n salt verify", 120, check=0)
        ser.run("sudo -n salt deployments", check=0)
        ser.note("SALTOS_PKG_OK")
        ser.send("echo SALTOS_PKG_OK; sudo -n poweroff\n")
        ser.wait("SALTOS_PKG_OK", 30)
        rc = 0
    except Fail as e:
        ser.note("FAIL: %s" % e)
        print("FAIL: %s" % e, file=sys.stderr, flush=True)
        rc = 1
    finally:
        try:
            q.wait(60)
        except subprocess.TimeoutExpired:
            q.kill()
        ser.close()
        shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(rc)


if __name__ == "__main__":
    main()
