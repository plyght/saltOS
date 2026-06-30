#!/usr/bin/env python3
# saltvm.py -- run a saltOS image headless on macOS and drive its console over a
# detached, scriptable channel (no UTM, no display, survives ssh logout).
#
# vm-run.sh is the one-shot "does it boot" harness; saltvm.py is its persistent
# sibling: it keeps a VM running in the background and lets you type into the
# console and read it back from separate shells -- which is what you need to set
# the machine up for SSH (see docs/headless-vm-ssh.md).
#
# Why a Python daemon instead of `screen`/`nohup`: vfkit's `stdio` console needs
# a real PTY (a pipe/file gives "operation not supported on socket"), and macOS
# tears a process down when its ssh channel closes. So we double-fork + setsid
# into our own session, hand vfkit a PTY slave, and bridge:
#     console output -> $DIR/console.log      console input <- $DIR/in.fifo
# The fifo is held open O_RDWR so vfkit never sees EOF; append to it to type.
#
# Boot modes (vfkit always boots the kernel directly -- the installed image's
# EFI/GRUB chain produces no console under headless vfkit, so we bypass it):
#   live/initramfs (default): SALTOS_KERNEL=Image + SALTOS_INITRD=initrd.gz,
#       root is the initramfs (tmpfs) -- ephemeral, wiped on reboot.
#   persistent disk:          SALTOS_DISK=...-installed.img + SALTOS_KERNEL=Image;
#       booted with an EMPTY initramfs (no /init) so the kernel mounts the real
#       ext4 disk root (/dev/vda2) and runs the on-disk init -- changes persist.
#
# Usage:
#   os/build/saltvm.py start | stop | status | ip
#   os/build/saltvm.py send 'shell command'      # type a line into the console
#   os/build/saltvm.py console [N]               # print last N lines (default 60)
# Env: SALTOS_DIR (default ~/saltos-out), VFKIT, CPUS, MEM_MIB, SALTOS_MAC,
#      SALTOS_KERNEL, SALTOS_INITRD, SALTOS_DISK, SALTOS_CMDLINE.
import os, sys, signal, select, fcntl, pty, errno, gzip, time

DIR = os.path.expanduser(os.environ.get("SALTOS_DIR", "~/saltos-out"))
VFKIT = os.environ.get("VFKIT") or os.path.expanduser("~/.cache/saltos/vfkit-v0.6.3")
MAC = os.environ.get("SALTOS_MAC", "52:54:00:a1:b2:c3")
CPUS = os.environ.get("CPUS", "2")
MEM = os.environ.get("MEM_MIB", "2048")
FIFO = os.path.join(DIR, "in.fifo")
CONLOG = os.path.join(DIR, "console.log")
VLOG = os.path.join(DIR, "vfkit.log")
PIDF = os.path.join(DIR, "vm.pid")


def first_existing(*names):
    for n in names:
        p = n if os.path.isabs(n) else os.path.join(DIR, n)
        if os.path.exists(p):
            return p
    return None


def ensure_empty_initrd():
    # An empty newc cpio archive (just a TRAILER!!! entry). The kernel finds no
    # /init in it and falls through to mounting root= -- this is what lets vfkit's
    # mandatory --initrd coexist with a real on-disk root.
    path = os.path.join(DIR, "empty.gz")
    if not os.path.exists(path):
        name = b"TRAILER!!!\0"
        header = b"070701" + b"%08x" % 0 * 11 + b"%08x" % len(name) + b"%08x" % 0
        blob = header + name
        blob += b"\0" * ((-len(blob)) % 4)
        with open(path, "wb") as f:
            f.write(gzip.compress(blob))
    return path


def vfkit_cmd():
    base = [VFKIT, "--cpus", CPUS, "--memory", MEM,
            "--device", "virtio-rng",
            "--device", "virtio-net,nat,mac=" + MAC,
            "--device", "virtio-serial,stdio"]
    kernel = os.environ.get("SALTOS_KERNEL") or first_existing("Image", "bzImage")
    disk = os.environ.get("SALTOS_DISK")

    if disk:
        # Persistent install: direct-kernel boot of the real disk root.
        if not kernel:
            sys.exit("SALTOS_DISK set but no kernel found; set SALTOS_KERNEL=path/to/Image")
        initrd = os.environ.get("SALTOS_INITRD") or ensure_empty_initrd()
        cmdline = os.environ.get(
            "SALTOS_CMDLINE",
            "root=/dev/vda2 rw rootwait init=/sbin/runit-init console=hvc0")
        return base + ["--kernel", kernel, "--initrd", initrd, "--kernel-cmdline", cmdline,
                       "--device", "virtio-blk,path=" + disk]

    # Live/initramfs: kernel + a real initramfs as root.
    initrd = os.environ.get("SALTOS_INITRD") or first_existing("initrd.gz", "initrd")
    if kernel and initrd:
        cmdline = os.environ.get("SALTOS_CMDLINE", "console=hvc0 rdinit=/sbin/runit-init")
        return base + ["--kernel", kernel, "--initrd", initrd, "--kernel-cmdline", cmdline]

    sys.exit("no image found in %s: set SALTOS_DISK=installed.img for a persistent "
             "boot, or SALTOS_KERNEL+SALTOS_INITRD for a live boot" % DIR)


def alive(pid):
    try:
        os.kill(pid, 0); return True
    except OSError:
        return False


def read_pid():
    try:
        return int(open(PIDF).read().strip())
    except Exception:
        return None


def nonblock(fd):
    fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)


def norm_mac(m):
    # macOS /var/db/dhcpd_leases drops leading zeros per octet ("00"->"0").
    return ":".join(o.lstrip("0") or "0" for o in m.lower().split(":"))


def lease_ips():
    out = []
    try:
        block = {}
        for line in open("/var/db/dhcpd_leases"):
            line = line.strip()
            if line == "{":
                block = {}
            elif line == "}":
                hw = block.get("hw_address", "").split(",")[-1]
                if hw and block.get("ip_address"):
                    out.append((hw, block["ip_address"]))
            elif "=" in line:
                k, v = line.split("=", 1)
                block[k] = v
    except FileNotFoundError:
        pass
    return out


def daemon_main(cmd):
    dn = os.open(os.devnull, os.O_RDWR); os.dup2(dn, 0)
    vl = os.open(VLOG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    os.dup2(vl, 1); os.dup2(vl, 2)
    master, slave = pty.openpty()
    pid = os.fork()
    if pid == 0:
        os.setsid(); os.close(master)
        os.dup2(slave, 0); os.dup2(slave, 1); os.dup2(slave, 2)
        if slave > 2:
            os.close(slave)
        try:
            os.execv(cmd[0], cmd)
        except Exception as e:
            os.write(2, ("execv failed: %s\n" % e).encode()); os._exit(127)
    os.close(slave)
    open(PIDF, "w").write(str(pid))
    fifo_fd = os.open(FIFO, os.O_RDWR)
    con = os.open(CONLOG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    nonblock(master); nonblock(fifo_fd)
    while True:
        try:
            if os.waitpid(pid, os.WNOHANG)[0] == pid:
                break
        except ChildProcessError:
            break
        try:
            r, _, _ = select.select([master, fifo_fd], [], [], 1.0)
        except OSError:
            continue
        if master in r:
            try:
                data = os.read(master, 65536)
                if data:
                    os.write(con, data)
            except OSError as e:
                if e.errno == errno.EIO:
                    break
        if fifo_fd in r:
            try:
                data = os.read(fifo_fd, 65536)
                if data:
                    os.write(master, data)
            except OSError:
                pass
    os._exit(0)


def do_start():
    pid = read_pid()
    if pid and alive(pid):
        print("already running pid", pid); return
    os.makedirs(DIR, exist_ok=True)
    cmd = vfkit_cmd()
    try:
        os.unlink(FIFO)
    except FileNotFoundError:
        pass
    os.mkfifo(FIFO)
    if os.fork() > 0:
        os.wait(); print("started (SALTOS_DIR=%s)" % DIR); return
    os.setsid()
    if os.fork() > 0:
        os._exit(0)
    daemon_main(cmd)


def do_stop():
    pid = read_pid()
    if pid and alive(pid):
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
        # Wait for vfkit to actually exit and release the disk image. A start that
        # races a still-open image fails with "storage device attachment is
        # invalid" (Virtualization.framework needs exclusive disk access).
        for _ in range(50):
            if not alive(pid):
                break
            time.sleep(0.2)
        else:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
            time.sleep(1)
    for f in (PIDF, FIFO):
        try:
            os.unlink(f)
        except FileNotFoundError:
            pass
    print("stopped")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    if cmd == "start":
        do_start()
    elif cmd == "stop":
        do_stop()
    elif cmd == "status":
        pid = read_pid()
        print("running pid %d" % pid if pid and alive(pid) else "not running")
    elif cmd == "send":
        with open(FIFO, "w") as f:
            f.write(" ".join(sys.argv[2:]) + "\r")
    elif cmd == "console":
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
        try:
            lines = open(CONLOG, "rb").read().splitlines()
            sys.stdout.buffer.write(b"\n".join(lines[-n:]) + b"\n")
        except FileNotFoundError:
            print("(no console log yet)")
    elif cmd == "ip":
        want = norm_mac(MAC)
        for hw, ip in lease_ips():
            mark = "  <- this VM" if norm_mac(hw) == want else ""
            print("%s  %s%s" % (hw, ip, mark))
        print("(VM MAC %s; macOS leases drop leading zeros)" % MAC)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
