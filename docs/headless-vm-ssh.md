# Headless saltOS VM + SSH (no UTM)

Run, smoke-test, and **SSH into** a saltOS image on a Mac with no UTM and no
display — scriptably, over SSH. This is how you bring up a saltOS VM on a remote
build/test Mac (e.g. a headless Mac mini) and then `ssh` straight into it.

Two tools, same `Virtualization.framework` backend UTM uses (via
[vfkit](https://github.com/crc-org/vfkit)):

| Tool | Purpose |
|------|---------|
| [`os/build/vm-run.sh`](../os/build/vm-run.sh) | one-shot boot / `--check` "does it boot" / `--gui` |
| [`os/build/saltvm.py`](../os/build/saltvm.py) | **persistent**, drivable VM: keep it running in the background, type into its console and read it back from other shells |

`saltvm.py` is what you use to set a machine up for SSH, because the setup is
done over the serial console before sshd exists.

## Why a daemon (vfkit gotchas)

- vfkit's `stdio` console needs a **real PTY** — a pipe or file gives
  `operation not supported on socket`. So `saltvm.py` hands vfkit a pty slave and
  bridges console output → `console.log`, console input ← `in.fifo` (held
  `O_RDWR` so vfkit never sees EOF).
- macOS tears a process down when its SSH channel closes, so the bridge
  double-forks + `setsid` into its own session and survives logout.
- `--kernel` **requires** `--initrd`. For a disk boot with no real initramfs,
  feed an **empty** cpio (`: | cpio -o -H newc | gzip > empty.gz`); the kernel
  finds no `/init` and falls through to mounting `root=`. `saltvm.py` generates
  this automatically.

## Boot modes

```sh
# persistent: real ext4 disk root (changes survive reboots)
SALTOS_DISK=out-iso/saltos-0.1.0-selfhost-aarch64-installed.img \
SALTOS_KERNEL=Image  os/build/saltvm.py start

# live/initramfs: tmpfs root, ephemeral (wiped on reboot)
SALTOS_KERNEL=Image SALTOS_INITRD=initrd.gz  os/build/saltvm.py start

os/build/saltvm.py console 40      # last 40 lines of the console
os/build/saltvm.py send 'whoami'   # type a line into the console
os/build/saltvm.py ip              # guest IP from the macOS DHCP leases
os/build/saltvm.py stop
```

The persistent path boots the kernel **directly** with `root=/dev/vda2`. The
installed image's own EFI/GRUB chain produces **no console** under headless vfkit
(no GOP framebuffer for GRUB to draw on, and its `grub.cfg` has no serial
terminal), so we bypass it. The image still boots normally via GRUB in UTM / on
real hardware.

`Image` (the kernel) and, for live boots, `initrd.gz` come out of the build —
copy them next to the disk image (the build leaves them in the selfhost work
dir).

## Get a shell over SSH

saltOS ships **no sshd** — installing one per image would bloat every build and
bake in a key. Instead install it at runtime the saltOS-native way (a stratum),
driving the console with `saltvm.py send` (or, for multi-line scripts, serve them
over HTTP from the host and `wget` them in — a large paste overflows the tty's
line buffer and drops bytes).

1. **Network.** The `netdhcp` service does not reliably bring the NIC up on every
   boot — if `ip -4 addr` shows no address, do it by hand:

   ```sh
   ip link set eth0 up
   udhcpc -i eth0 -s /usr/share/udhcpc/default.script
   ```

2. **sshd via a stratum** (busybox-wget does HTTPS, without cert validation):

   ```sh
   salt --yes stratum add alpine
   apk add openssh util-linux
   ssh-keygen -A
   ```

3. **Authorize your key.** saltOS bind-mounts the host `/root` into the stratum,
   so root's `authorized_keys` goes in the **host** `/root/.ssh/`, *not*
   `/strata/alpine/root/.ssh`:

   ```sh
   mkdir -p /root/.ssh && chmod 700 /root/.ssh
   printf '%s\n' 'ssh-ed25519 AAAA... you' > /root/.ssh/authorized_keys
   chmod 600 /root/.ssh/authorized_keys
   ```

4. **Start sshd.** It must be exec'd with an **absolute path** (sshd re-execs
   itself). Strata share the host net + PID namespace, so sshd on `0.0.0.0:22` is
   reachable at the VM's DHCP IP:

   ```sh
   salt alpine/sh -c 'exec /usr/sbin/sshd -D' &
   ```

5. **PTYs for interactive ssh.** The init does not mount `devpts`, so without it
   you get `PTY allocation request failed`. Mount it (create the dir first — a
   fresh devtmpfs has no `/dev/pts`):

   ```sh
   mkdir -p /dev/pts
   mount -t devpts devpts /dev/pts -o gid=5,mode=620,ptmxmode=666
   ln -sf /dev/pts/ptmx /dev/ptmx
   ```
   Restart sshd afterwards so it inherits the new mount.

6. **Land in the saltOS host, not the stratum.** sshd lives in the Alpine
   stratum, whose userspace has no `salt`. Bridge the login back into the host's
   mount namespace (PID 1) with an sshd `ForceCommand`:

   ```sh
   cat > /strata/alpine/usr/local/bin/saltos-host-login <<'WRAP'
   #!/bin/sh
   [ -n "$SSH_ORIGINAL_COMMAND" ] && exec nsenter -t 1 -m -- /bin/bash -lc "$SSH_ORIGINAL_COMMAND"
   exec nsenter -t 1 -m -- /bin/bash -l
   WRAP
   chmod +x /strata/alpine/usr/local/bin/saltos-host-login
   echo 'ForceCommand /usr/local/bin/saltos-host-login' >> /strata/alpine/etc/ssh/sshd_config
   ```

### Make it survive reboots

The stratum, openssh, your key and config all live on the disk and persist. To
auto-start sshd on every boot, add a runit service **on the disk** (this is
machine config, not an image change):

```sh
mkdir -p /etc/runit/sv/sshd
cat > /etc/runit/sv/sshd/run <<'SVC'
#!/bin/sh
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts -o gid=5,mode=620,ptmxmode=666 2>/dev/null
ln -sf /dev/pts/ptmx /dev/ptmx 2>/dev/null
exec salt alpine/sh -c 'exec /usr/sbin/sshd -D'
SVC
chmod +x /etc/runit/sv/sshd/run
ln -sf /etc/runit/sv/sshd /etc/runit/runsvdir/current/sshd
```

The 4 GB default installed image is too small for real strata (Arch ARM alone
won't fit). Build a bigger one with `SIZE_MB=16384`, or grow an existing image:
`truncate -s 16G img && growpart <loop> 2 && resize2fs <loop>p2`.

## Reach it from your laptop (no manual hop)

The VM's IP is on the host's internal vmnet NAT — only reachable *from* the test
Mac. Use SSH `ProxyJump` so a plain `ssh saltvm` tunnels through it. In
`~/.ssh/config`:

```
Host saltmini
  HostName <test-mac-tailscale-ip>
  User <admin-user>

Host saltvm
  HostName 192.168.64.12          # the VM's DHCP address (saltvm.py ip)
  User root
  IdentityFile ~/.ssh/saltvm_key
  IdentitiesOnly yes
  ProxyJump saltmini
  StrictHostKeyChecking no
  UserKnownHostsFile /dev/null
```

Then `ssh saltvm` drops you straight into the saltOS host shell.
