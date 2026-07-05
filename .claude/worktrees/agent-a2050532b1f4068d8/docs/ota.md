# OTA updates

saltOS updates are pull-based and signed. A client runs `salt sync` to fetch a
signed package index from a repository URL, verifies it against a trusted ed25519
public key, then `salt update` snapshots the host and upgrades native packages.
On Raspberry Pi this is wrapped by `salt-ota`, a periodic runit service, with an
optional A/B tryboot path for atomic root switches.

## Server side

The "server" is any static HTTPS host serving a published repository tree. The
layout `salt` expects is:

```
<base-url>/<arch>/index.toml
<base-url>/<arch>/index.toml.sig
<base-url>/<arch>/packages/<pkg>.grain
```

### 1. Create a signing key (once)

```sh
salt keygen ./keys ota          # writes keys/ota.pub and keys/ota.sec
```

Keep `ota.sec` secret. Bake `ota.pub` into images via `OTA_PUBKEY=` (the Pi builder
installs it to `/etc/salt/keys/ota.pub`).

### 2. Build packages and publish a signed index

```sh
# build .grain packages into a repo tree (repo/<arch>/packages/*.grain)
OTA_SECRET_KEY=./keys/ota.sec bash os/ota/publish.sh ./repo
```

### 3. Host it

Any static host works (S3, GitHub Pages, nginx). For a quick self-hosted endpoint:

```sh
OTA_ROOT=./repo OTA_PORT=8080 bun os/ota/server.ts
# https: OTA_TLS_CERT=cert.pem OTA_TLS_KEY=key.pem bun os/ota/server.ts
```

Then point clients at the base URL (the directory that contains `<arch>/`):

```sh
# /etc/salt/repo.conf on the device
repo = "current"
source = "https://updates.example.com"
key = "/etc/salt/keys/ota.pub"
```

## Client side

`salt-ota` reads `[ota]` from `/etc/salt/salt.conf`:

```toml
[ota]
enabled = true
interval = "86400"
reboot_on_kernel = false
```

- `salt-ota run` — snapshot, `salt sync`, `salt update`; on failure it rolls back to
  the pre-update snapshot. This is the default daily-driver path.
- `salt-ota status` — show config, snapshots, and A/B/tryboot state.
- `salt-ota confirm` — commit a pending A/B tryboot as the new default.

The `salt-update` runit service runs `salt-ota run` every `interval` seconds and
calls `salt-ota confirm` once at boot (to commit a successful tryboot).

## A/B atomic updates (Raspberry Pi, experimental)

When `SALTOS_AB=1` and the Pi firmware directory is present, `salt-ota run` uses
`os/pi/ab-update.sh`:

1. `prepare` — snapshot the active Btrfs subvol (`@`) into the standby (`@b`) and
   mount it.
2. update is applied into the standby root via `salt --root`.
3. `finalize` — write a one-shot `tryboot.txt` whose `cmdline` selects the standby
   subvol.
4. reboot with `reboot "0 tryboot"`. If the new root boots and `salt-ota confirm`
   runs, it is committed; otherwise the Pi falls back to the previous root on the
   next normal reboot.

This path is implemented but **untested on hardware**; the snapshot+rollback path
above is the supported default until A/B is validated on a real Pi 5.
