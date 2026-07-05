# saltOS runit init layer

This directory holds the runit service tree, the two-stage boot scripts, and the
`svc` management wrapper for saltOS. It is arch-neutral (x86_64 and aarch64).

## On-disk layout (install targets)

| Source here              | Installs to                          | Mode  |
|--------------------------|--------------------------------------|-------|
| `stages/1`               | `/etc/runit/1`                       | 0755  |
| `stages/2`               | `/etc/runit/2`                       | 0755  |
| `stages/3`               | `/etc/runit/3`                       | 0755  |
| `sv/<name>/`             | `/etc/runit/sv/<name>/`              | dir   |
| `sv/<name>/run`          | `/etc/runit/sv/<name>/run`           | 0755  |
| `sv/<name>/finish`       | `/etc/runit/sv/<name>/finish`        | 0755  |
| `sv/<name>/check`        | `/etc/runit/sv/<name>/check`         | 0755  |
| `svc`                    | `/usr/bin/svc`                       | 0755  |

The active run level is the directory `/etc/runit/runsvdir/current`, which is a
symlink to a named run level (for example `/etc/runit/runsvdir/default`).
Enabled services are symlinks inside `current` pointing back at
`/etc/runit/sv/<name>`.

## Boot stages

runit boots in three stages, all driven by `/sbin/init` (PID 1) reading
`/etc/runit/{1,2,3}`:

1. **Stage 1** (`stages/1`): one-shot system bring-up. Mounts the pseudo
   filesystems, starts eudev (`udevd`), triggers coldplug, sets the hostname,
   activates swap, and runs `fsck` on the remaining mounts.
2. **Stage 2** (`stages/2`): the supervision loop. Runs `runsvdir` over
   `/etc/runit/runsvdir/current`, which keeps every enabled service alive.
3. **Stage 3** (`stages/3`): shutdown. Stops services, kills stragglers, disables
   swap, remounts read-only, and reboots or powers off.

## Services

| Service        | Purpose                                              |
|----------------|------------------------------------------------------|
| `udevd`        | eudev device manager                                 |
| `dbus`         | system message bus                                   |
| `socklog`      | system logging (socklog + svlogd)                    |
| `agetty-tty1`  | login on tty1                                        |
| `agetty-tty2`  | login on tty2                                        |
| `dhcpcd`       | DHCP networking                                      |
| `sshd`         | OpenSSH server                                       |
| `chronyd`      | NTP time synchronization                             |
| `seatd`        | seat management for the display stack                |
| `sddm`         | SDDM display manager / LXQt session login            |
| `snooze`       | periodic job runner (cron replacement)               |

Services that own a `log/run` (such as `socklog`) get their output captured by a
companion `svlogd` supervised under the same service directory.

## Managing services with `svc`

```sh
svc enable sshd       # link /etc/runit/sv/sshd into the active run level
svc disable sshd      # stop and unlink it
svc start sshd
svc stop sshd
svc restart sshd
svc status            # status of every enabled service
svc status sshd       # status of one service
```

`enable` refuses services that have no executable `run` script; `disable` refuses
to remove anything that is not a symlink, so it never deletes a service
definition by accident.
