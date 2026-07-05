# saltOS Stratum Plane

A **stratum** is a managed foreign-distro userspace — a Void, Arch, or Debian
root that lives alongside the native saltOS system. The native `salt` tool
bootstraps strata, runs software from them, exposes their commands as host
shims, adopts components from them into host roles, and rolls them back. This
gives saltOS Bedrock-like mix-and-match power with native accountability.

A stratum owns its root filesystem, its native package manager, its package
database, its repository configuration, and its own rollback snapshots. It does
**not** own the bootloader, the host kernel, the host init, host `/usr`, host
`/etc`, the host package database, or the host rollback policy. Those stay
native.

For how this fits the wider system, see [architecture.md](architecture.md). For
the recipe format that defines a stratum, see
[../strata/README.md](../strata/README.md). The native package manager is
documented in [package-manager.md](package-manager.md) and rollback in
[rollback.md](rollback.md).

## 1. Bootstrapping a stratum

saltOS ships official stratum definitions, so you bootstrap one by name:

```sh
salt stratum add void
salt stratum add arch
salt stratum add debian
```

`salt stratum add` reads the recipe (from `strata/<name>.toml`, or a path you
pass directly), downloads and verifies the bootstrap tarball or runs
`debootstrap`, and populates the stratum root under `/strata/<name>`. You can
also point it at a custom recipe:

```sh
salt stratum add ./my-stratum.toml
```

A recipe is validated before use:

```sh
salt stratum lint strata/arch.toml
```

## 2. Listing strata

```sh
$ salt stratum list
  NAME     FAMILY   PM       ROOT             TRUST      STATUS
* void     void     xbps     /strata/void     official   ready
  arch     arch     pacman   /strata/arch     official   ready
  debian   debian   apt      /strata/debian   official   ready
```

`salt stratum status <name>` shows a single stratum in detail — its repositories,
exposed commands, adopted providers, and snapshots.

## 3. Running foreign software

Run any command from a stratum without exposing it globally:

```sh
salt run arch firefox
salt run debian gcc --version
salt run void xbps-query -l
```

`salt run <stratum> <command> [args...]` executes the command inside the
stratum's root with the integration permissions (`graphics`, `audio`, `dbus`)
its recipe grants. For an interactive session inside the stratum:

```sh
salt stratum shell arch
```

There is also a shorthand that drops the `run` keyword: `salt <stratum>/<command>`.

```sh
salt arch/nvim notes.txt
salt debian/gcc --version
```

`salt <stratum>/<command> [args...]` is exactly equivalent to
`salt run <stratum> <command> [args...]`.

## 4. Package passthrough

Drive a stratum's native package manager through `salt` so the operation is
recorded:

```sh
salt pkg void install ripgrep
salt pkg arch install firefox
salt pkg debian install build-essential
salt pkg arch remove chromium
salt pkg void update
```

`salt pkg <stratum> <op> [packages...]` maps onto the stratum's package manager
(`xbps`, `pacman`, `apt`) and records the operation against that stratum. You
can also run the package manager directly from `salt stratum shell`, but going
through `salt pkg` keeps saltOS's record of what changed.

### 4a. The `<stratum>/<pkg>` shortcuts

For everyday use the top-level package verbs accept a `<stratum>/<pkg>` form, so
the same word (`install`, `remove`, `search`, `update`) works for both the
native plane and any stratum:

```sh
salt install alpine/neovim     # apk add neovim, then offer to expose its binaries
salt remove alpine/neovim      # apk del neovim, then drop the host shims it added
salt search alpine/ripgrep     # search the alpine repositories
salt update alpine             # upgrade everything inside the alpine stratum
```

A bare name with no `<stratum>/` prefix is resolved against the native
repository first; if it is not a native package, `salt` lists the available
strata and asks which one to install from.

If the named stratum has not been bootstrapped yet, `salt install` offers to
bootstrap it from its built-in recipe first, so you do not have to run
`salt stratum add` by hand:

```sh
salt install arch/neofetch     # offers to bootstrap arch, then installs neofetch
```

After a stratum install, `salt` diffs the stratum's `bin` directories *and* its
`usr/share/applications` desktop entries, finds what the package added (for
example `neovim` provides the `nvim` command; `firefox` provides both the
`firefox` command and a `firefox.desktop` launcher), and — by default — asks once
before exposing them. Commands go on `PATH`; desktop apps get a host menu entry.
So a GUI app appears in your application menu and a CLI tool is on your path with
no extra `salt expose` / `salt expose-desktop` step. This is governed by
`/etc/salt/salt.conf`:

```toml
[install]
auto_expose = "prompt"   # always | prompt | never
```

`--expose` and `--no-expose` override the configured mode for a single command,
and `--yes` accepts the expose prompt non-interactively.

### 4b. Using a stratum's package manager directly

When a stratum is bootstrapped, saltOS also exposes its native package manager as
a host command, so the tools you already know work straight from the host:

```sh
sudo pacman -S neofetch     # in an arch stratum
sudo apt install neofetch   # in a debian stratum
sudo apk add neofetch       # in an alpine stratum
```

These are thin host shims (`/usr/local/salt/shims/pacman`, ...) that run the real
package manager *as root inside the owning stratum*, using that stratum's
configuration and mirrors. They behave like raw `pacman`/`apt`/`apk`: arbitrary
flags pass straight through. saltOS takes a safety snapshot before **mutating**
operations (install/remove/upgrade) and skips it for read-only ones
(`pacman -Q`, `apt search`, ...), so queries stay fast.

This is controlled by `/etc/salt/salt.conf`:

```toml
[strata]
expose_pm = true   # set false to keep foreign package managers off the host
```

with `expose_pm = false` (or a one-off `--no-expose`) you keep raw access through
`salt stratum shell <name>` instead. The shim is named after the package manager,
so two strata of the *same* family (e.g. two pacman strata) would share one
`pacman` shim — the most recently bootstrapped one wins; use `salt pkg <stratum>`
or `salt stratum shell` to disambiguate. Equivalent explicit form:

```sh
salt pm arch pacman -S neofetch
```

## 5. Exposing commands as host shims

By default, stratum commands are not on your `PATH`. Expose the ones you want as
**shims** owned by saltOS:

```sh
salt expose arch firefox
salt expose void rg as rg
salt expose debian gcc as gcc-debian
salt unexpose firefox
salt exposed
```

Shims live in:

```
/usr/local/salt/shims
```

That directory must be on your `PATH` for exposed commands to be found. saltOS
adds it via a profile fragment (`/etc/profile.d/salt-shims.sh`), which prepends
the shim directory if it is not already present. Each shim records its source
stratum, the target command, the chosen alias, its environment policy, its
graphical/session permissions, and when it was created — so an exposed command
is always traceable back to the stratum that provides it.

If more than one stratum (or the native system) provides the same command,
saltOS does not guess: you choose which one to expose. Native saltOS packages
win for host-owned paths, and a foreign package manager is never allowed to
overwrite native host files.

## 6. Exposing desktop applications

Graphical apps from strata are a first-class daily-driver case. In normal use you
do not run this command at all: `salt install <stratum>/<app>` already detects the
`.desktop` entries the package added and offers to expose them (see §4a), so the
app shows up in your menu right after install.

`salt expose-desktop` is the manual form, for re-exposing an app you skipped or
for apps installed some other way:

```sh
salt expose-desktop arch firefox
salt expose-desktop debian libreoffice
```

Either way, saltOS installs a host desktop entry that launches the app through
its stratum, wiring up Wayland/X11 sockets, audio sockets, D-Bus session access,
fonts, icons, and GPU device access according to the stratum's `[integration]`
permissions.

## 7. Component providers and adoption

Beyond running apps, a stratum can provide a system component that saltOS
**adopts** into a host role — coreutils, a compiler toolchain, a kernel, fonts,
language runtimes, and so on. saltOS owns the adoption decision, not the foreign
package manager:

```sh
salt provider list coreutils
salt provider set coreutils debian/coreutils
salt provider status
salt provider rollback coreutils
```

`salt provider set <component> <stratum>/<package>` adopts a stratum's component
into the host role. Adoption is stricter than command exposure: exposing
`arch/firefox` as a desktop app is low-risk, but adopting Debian coreutils or an
Arch kernel into the host changes core system behavior. Every adoption requires
an explicit action, takes a pre-adoption snapshot, runs compatibility and file
conflict checks, warns about the trust boundary it crosses, and records a
rollback entry with clear ownership. `salt provider rollback <component>`
restores the previous provider from that entry.

## 8. Per-stratum snapshots and rollback

Each stratum has its own snapshot and rollback history, independent of the host
deployment history:

```sh
salt stratum snapshot arch
salt stratum rollback arch
```

`salt stratum snapshot <name>` takes an explicit snapshot of the stratum root;
strata also take pre-transaction snapshots before package operations. `salt
stratum rollback <name>` restores a stratum to its previous snapshot without
touching the host system or any other stratum. This mirrors the native rollback
model (see [rollback.md](rollback.md)) but is scoped to one stratum.

## 9. Service integration

Host services are runit services. A stratum may ship daemons, but they are
integrated through saltOS-managed service wrappers rather than letting a foreign
init control the host:

In normal use this is automatic: when `salt install <stratum>/<pkg>` adds a
daemon, salt detects the new systemd unit(s) the package shipped, reads each
unit's `ExecStart`, generates a runit service that runs that daemon **inside its
stratum**, and enables it (under `auto_expose = "always"` it does so silently;
under `prompt` it lists the daemons and asks first). `salt remove` tears the
service back down. So installing, say, a web server brings its daemon up under
runit with no extra step.

The manual commands remain for tuning or for daemons added another way:

```sh
salt service import arch docker
salt service enable arch docker
salt service start arch docker
```

`salt service import <stratum> <service>` wraps a stratum daemon as a host
runit service; `enable` and `start` then manage it the same way as any native
service.

Automatic import is governed by `/etc/salt/salt.conf`:

```toml
[strata]
auto_service = true   # set false to import daemons only by hand
```

Two honest limits: the conversion is best-effort from the unit's `ExecStart`
(systemd `Type=forking`, `User=`, socket activation, and `EnvironmentFile`
semantics are not fully reproduced under runit), and it covers systemd units
(Debian/Arch/Fedora/openSUSE). Alpine (OpenRC) and Void (runit) daemons are not
auto-detected yet — import those with `salt service import`.

## 10. Trust boundaries

The stratum plane spans several trust levels, and saltOS keeps them visible:

- **Native `.grain` packages** are the highest trust: built from pinned,
  hash-verified sources, served from a signed repository, and verified against a
  signed index before install. They own host paths and the host rollback policy.
- **Official foreign repositories** (the upstream Void, Arch, and Debian repos a
  shipped stratum recipe declares) are trusted at their own ecosystem's level —
  signed by the upstream distro, not by saltOS. Software from them is fine to
  `run` and `expose`, but it is foreign code with foreign provenance.
- **Lower-trust sources** — custom stratum recipes, third-party repositories
  added to a stratum, or AUR-style user content — carry the least assurance and
  should be treated accordingly.

Exposing a command or desktop app is low-risk and stays inside the stratum.
Adopting a component into a host role crosses a trust boundary into the native
system, which is why adoption demands explicit action, a snapshot, conflict
checks, and a recorded rollback point. A foreign package manager is never
allowed to overwrite native host files, and saltOS records which stratum every
exposed command, adopted provider, and imported service comes from.
