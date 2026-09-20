#!/usr/bin/env python3
"""Drive a saltOS x86_64 UEFI image through a full OTA cycle over its serial console.

usage: qemu-drive.py --image IMG --ovmf-code F --ovmf-vars F --log serial.log \
         --repo-url http://10.0.2.2:PORT --old-kernel REL --new-kernel REL \
         --old-version V --new-version V

Boots the image under QEMU, then in order: checks/applies the update (salt +
kernel), reboots into the trial kernel, forces an unconfirmed reboot and checks
the fallback to the previous kernel, re-arms and confirms the new kernel, rolls
back to the previous deployment and boots it, and finally refuses a bad-hash
and a bad-signature repository leaving the system unchanged.  Every byte the
guest writes to ttyS0 lands in --log; the driver prints its own assertions to
stdout and exits non-zero on the first failure.
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time


class Fail(Exception):
    pass


class Serial:
    def __init__(self, path, log):
        self.path = path
        self.log = open(log, "ab", buffering=0)
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.sock = None
        self.alive = True
        self.n = 0

    def connect(self, proc, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if proc.poll() is not None:
                raise Fail("qemu exited with status %d before opening the serial socket" % proc.returncode)
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.path)
                self.sock = s
                threading.Thread(target=self._reader, daemon=True).start()
                return
            except OSError:
                time.sleep(0.5)
        raise Fail("serial socket %s never came up" % self.path)

    def _reader(self):
        while self.alive:
            try:
                d = self.sock.recv(65536)
            except OSError:
                break
            if not d:
                break
            self.log.write(d)
            with self.lock:
                self.buf += d

    def note(self, msg):
        line = ("\n### %s %s\n" % (time.strftime("%H:%M:%S"), msg)).encode()
        self.log.write(line)
        print(msg, flush=True)

    def send(self, s):
        self.sock.sendall(s.encode())

    def text(self):
        with self.lock:
            return self.buf.decode("utf-8", "replace")

    def mark(self):
        with self.lock:
            return len(self.buf)

    def since(self, mark):
        with self.lock:
            return self.buf[mark:].decode("utf-8", "replace")

    def wait(self, pattern, timeout, mark=0):
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = rx.search(self.since(mark))
            if m:
                return m
            time.sleep(0.5)
        raise Fail("timed out after %ds waiting for /%s/" % (timeout, pattern))

    def wait_login(self, timeout):
        self.note("waiting for autologin")
        mark = self.mark()
        self.wait(r"login: salt \(automatic login\)", timeout, mark)
        time.sleep(3)
        for _ in range(20):
            m = self.mark()
            self.send("\n")
            try:
                self.wait(r"\$ $", 3, m)
                break
            except Fail:
                pass
        self.send("stty -echo cols 250\n")
        time.sleep(0.5)
        self.run("true", 10)

    def run(self, cmd, timeout=60, check=None):
        self.n += 1
        tag = "__DONE_%d" % self.n
        mark = self.mark()
        self.log.write(("\n+ %s\n" % cmd).encode())
        self.send("%s; printf '__RC=%%d %s\\n' $?\n" % (cmd, tag))
        m = self.wait(r"__RC=(\d+) " + tag, timeout, mark)
        out = self.since(mark)
        out = out[: out.rfind("__RC=")]
        rc = int(m.group(1))
        if check is not None and rc != check:
            raise Fail("`%s` exited %d, expected %d\n%s" % (cmd, rc, check, out))
        return rc, out

    def close(self):
        self.alive = False
        try:
            self.sock.close()
        except Exception:
            pass
        self.log.close()


def qemu_cmd(a, serial_path, vars_copy):
    accel = "kvm" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "tcg"
    cpu = "host" if accel == "kvm" else "max"
    return [
        a.qemu, "-machine", "q35,accel=%s" % accel, "-cpu", cpu, "-m", str(a.mem), "-smp", "2",
        "-display", "none", "-vga", "none",
        "-drive", "if=pflash,format=raw,readonly=on,file=%s" % a.ovmf_code,
        "-drive", "if=pflash,format=raw,file=%s" % vars_copy,
        "-drive", "file=%s,if=virtio,format=%s,cache=unsafe" % (a.image, a.format),
        "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0",
        "-chardev", "socket,id=s0,path=%s,server=on,wait=off" % serial_path,
        "-serial", "chardev:s0",
        "-rtc", "base=utc",
    ]


def expect(cond, msg):
    if not cond:
        raise Fail(msg)
    print("ok: " + msg, flush=True)


def kernel_running(ser):
    return ser.run("uname -r", check=0)[1].strip().splitlines()[-1]


def installed_version(ser, name):
    _, out = ser.run("sudo -n salt list", check=0)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] == name:
            return parts[1].rsplit("-", 1)[0]
    return ""


def wait_boot_default(ser, kernel, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        rc, out = ser.run("sudo -n salt boot status", 30)
        if rc == 0 and re.search(r"default:\s+%s\s*$" % re.escape(kernel), out, re.M) and re.search(r"pending:\s+none", out):
            return out
        time.sleep(5)
    raise Fail("boot status never settled on %s as confirmed default" % kernel)


def wait_network(ser, url, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        rc, _ = ser.run("curl -fsS -o /dev/null %s" % url, 20)
        if rc == 0:
            return
        time.sleep(5)
    raise Fail("guest cannot reach %s" % url)


def set_repo(ser, url):
    ser.run("sudo -n sed -i 's#^source = .*#source = \"%s\"#' /etc/salt/repo.conf" % url, check=0)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--image", required=True)
    p.add_argument("--format", default="raw")
    p.add_argument("--ovmf-code", required=True)
    p.add_argument("--ovmf-vars", required=True)
    p.add_argument("--log", required=True)
    p.add_argument("--repo-url", required=True, help="base URL as seen from the guest, e.g. http://10.0.2.2:8123")
    p.add_argument("--old-kernel", required=True)
    p.add_argument("--new-kernel", required=True)
    p.add_argument("--old-version", required=True)
    p.add_argument("--new-version", required=True)
    p.add_argument("--qemu", default="qemu-system-x86_64")
    p.add_argument("--mem", type=int, default=2048)
    p.add_argument("--boot-timeout", type=int, default=420)
    a = p.parse_args()

    tmp = tempfile.mkdtemp(prefix="saltos-ota-")
    serial_path = os.path.join(tmp, "serial.sock")
    vars_copy = os.path.join(tmp, "OVMF_VARS.fd")
    shutil.copy(a.ovmf_vars, vars_copy)
    cmd = qemu_cmd(a, serial_path, vars_copy)
    print("+ " + " ".join(cmd), flush=True)
    q = subprocess.Popen(cmd)
    ser = Serial(serial_path, a.log)
    rc = 1
    try:
        ser.connect(q)
        good, badhash, badsig = (a.repo_url + s for s in ("/good", "/badhash", "/badsig"))

        ser.note("boot 1: factory image")
        ser.wait_login(a.boot_timeout)
        expect(kernel_running(ser) == a.old_kernel, "running factory kernel %s" % a.old_kernel)
        expect(installed_version(ser, "salt") == a.old_version, "salt %s installed" % a.old_version)
        _, out = ser.run("sudo -n salt deployments", check=0)
        expect("install" in out, "factory deployment recorded")
        _, out = ser.run("sudo -n salt-ota status", check=0)
        expect(re.search(r"ota.enabled\s*: true", out) is not None, "OTA enabled in salt.conf")
        expect(re.search(r"pending:\s+none", out) is not None, "no pending kernel before update")
        ser.run("sudo -n salt boot status", check=0)
        wait_network(ser, good + "/", 180)
        rc_, out = ser.run("sudo -n salt-ota check", 120)
        expect(rc_ == 2 and "salt" in out and "linux-saltos" in out, "salt-ota check reports salt + kernel updates (rc 2)")
        expect(installed_version(ser, "salt") == a.old_version, "check did not apply anything")

        ser.note("boot 1: salt-ota run (salt + kernel)")
        rc_, out = ser.run("sudo -n salt-ota run --no-reboot", 600)
        expect(rc_ == 3, "salt-ota run applied the update and requests a reboot (rc 3), got %d" % rc_)
        expect(installed_version(ser, "salt") == a.new_version, "salt %s active after update" % a.new_version)
        expect(installed_version(ser, "linux-saltos") != "", "linux-saltos grain installed")
        ser.run("test -f /boot/vmlinuz-%s -a -f /boot/vmlinuz-%s" % (a.old_kernel, a.new_kernel), check=0)
        print("ok: both kernels present in /boot", flush=True)
        ser.run("test -f /usr/lib/modules/%s/modules.dep -a -f /usr/lib/modules/%s/modules.dep" % (a.old_kernel, a.new_kernel), check=0)
        print("ok: module trees for both kernels are complete", flush=True)
        ser.run("sudo -n btrfs subvolume list /", check=0)
        _, out = ser.run("sudo -n salt deployments", check=0)
        expect("root-" in out, "previous deployment kept as a btrfs snapshot")
        ser.run("grep -c menuentry /boot/grub/grub.cfg", check=0)
        _, out = ser.run("cat /boot/grub/grub.cfg", check=0)
        expect("Previous deployments" in out and "vmlinuz-%s" % a.new_kernel in out and
               "vmlinuz-%s" % a.old_kernel in out, "grub.cfg lists both kernels and previous deployments")
        rc_, out = ser.run("sudo -n salt boot status", 30)
        expect(rc_ == 3 and ("pending:  %s (armed for next boot)" % a.new_kernel) in out,
               "one-shot trial armed for %s" % a.new_kernel)
        ser.run("sudo -n cat /var/log/salt-ota.log", check=0)
        ser.run("sudo -n salt-ota status", check=0)

        ser.note("reboot -> boot 2: trial kernel, left unconfirmed")
        ser.send("sudo -n reboot\n")
        ser.wait_login(a.boot_timeout)
        expect(kernel_running(ser) == a.new_kernel, "trial boot is running %s" % a.new_kernel)
        rc_, out = ser.run("sudo -n salt boot status", 30)
        expect(rc_ == 3 and ("pending:  %s (booted, awaiting confirm)" % a.new_kernel) in out,
               "trial flag consumed by GRUB, kernel awaiting confirmation")
        ser.note("reboot -f before confirmation -> boot 3: expect fallback")
        ser.send("sudo -n reboot -f\n")
        ser.wait_login(a.boot_timeout)
        expect(kernel_running(ser) == a.old_kernel, "unconfirmed trial fell back to %s" % a.old_kernel)
        wait_boot_default(ser, a.old_kernel, 300)
        print("ok: salt-update service cleared the failed trial", flush=True)
        _, out = ser.run("sudo -n cat /var/log/salt-ota.log", check=0)
        expect("fell back" in out, "fallback recorded in /var/log/salt-ota.log")

        ser.note("boot 3: re-arm trial and reboot -> boot 4")
        ser.run("sudo -n salt boot try", check=0)
        rc_, out = ser.run("sudo -n salt boot status", 30)
        expect(rc_ == 3 and "(armed for next boot)" in out, "trial re-armed")
        ser.send("sudo -n reboot\n")
        ser.wait_login(a.boot_timeout)
        expect(kernel_running(ser) == a.new_kernel, "second trial is running %s" % a.new_kernel)
        wait_boot_default(ser, a.new_kernel, 300)
        print("ok: salt-update service confirmed %s as the default kernel" % a.new_kernel, flush=True)
        _, out = ser.run("sudo -n cat /var/log/salt-ota.log", check=0)
        expect("confirmed" in out, "confirmation recorded in /var/log/salt-ota.log")
        rc_, out = ser.run("sudo -n salt-ota run --no-reboot", 300)
        expect(rc_ == 0, "salt-ota run is a no-op when current (rc 0), got %d" % rc_)

        ser.note("boot 4: salt rollback -> boot 5")
        _, before = ser.run("sudo -n salt deployments", check=0)
        rc_, out = ser.run("sudo -n salt rollback", 300)
        expect(rc_ == 3 and "undid deployment" in out, "salt rollback replaced the root subvolume (rc 3)")
        _, out = ser.run("sudo -n salt deployments", check=0)
        expect("rollback" in out, "rollback recorded as a deployment")
        ser.run("sudo -n btrfs subvolume list /", check=0)
        ser.send("sudo -n reboot\n")
        ser.wait_login(a.boot_timeout)
        expect(kernel_running(ser) == a.old_kernel, "rolled-back root boots %s" % a.old_kernel)
        expect(installed_version(ser, "salt") == a.old_version, "salt %s restored" % a.old_version)
        ser.run("test ! -e /boot/vmlinuz-%s" % a.new_kernel, check=0)
        print("ok: new kernel absent from the restored generation", flush=True)
        ser.run("sudo -n salt deployments", check=0)
        ser.run("sudo -n btrfs subvolume list /", check=0)
        wait_boot_default(ser, a.old_kernel, 300)

        ser.note("boot 5: refuse a corrupted grain")
        wait_network(ser, badhash + "/", 180)
        set_repo(ser, badhash)
        ser.run("sudo -n salt clean --all", check=0)
        _, before = ser.run("sudo -n salt deployments", check=0)
        rc_, out = ser.run("sudo -n salt-ota run --no-reboot", 300)
        expect(rc_ == 4 and "HASH MISMATCH" in out, "bad hash: update refused before any change (rc 4)")
        expect(installed_version(ser, "salt") == a.old_version, "bad hash: salt unchanged")
        expect(kernel_running(ser) == a.old_kernel and
               ser.run("test ! -e /boot/vmlinuz-%s" % a.new_kernel)[0] == 0, "bad hash: boot files unchanged")
        _, out = ser.run("sudo -n salt deployments", check=0)
        expect(out.strip() == before.strip(), "bad hash: no deployment created")

        ser.note("boot 5: refuse an index signed by an unknown key")
        set_repo(ser, badsig)
        rc_, out = ser.run("sudo -n salt-ota run --no-reboot", 300)
        expect(rc_ == 1 and "SIGNATURE" in out.upper(), "bad signature: update refused (rc 1)")
        expect(installed_version(ser, "salt") == a.old_version, "bad signature: salt unchanged")
        set_repo(ser, good)
        rc_, out = ser.run("sudo -n salt-ota check", 120)
        expect(rc_ == 2, "good repo reachable again, update still offered (rc 2)")
        ser.run("sudo -n cat /var/log/salt-ota.log", check=0)
        ser.note("SALTOS_OTA_OK")
        ser.send("echo SALTOS_OTA_OK; sudo -n poweroff\n")
        ser.wait("SALTOS_OTA_OK", 30)
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
