return {
  name = "arch",
  family = "arch",
  arch = "aarch64",
  package_manager = "pacman",
  root = "/strata/arch",
  trust = "community",
  bootstrap = {
    method = "rootfs",
    url = "http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz",
    sha256 = "",
    strip = 0,
  },
  integration = {
    graphics = true,
    audio = true,
    dbus = true,
  },
  repository = {
    {
      name = "core",
      url = "http://mirror.archlinuxarm.org/$arch/$repo",
    },
    {
      name = "extra",
      url = "http://mirror.archlinuxarm.org/$arch/$repo",
    },
    {
      name = "alarm",
      url = "http://mirror.archlinuxarm.org/$arch/$repo",
    },
    {
      name = "aur",
      url = "http://mirror.archlinuxarm.org/$arch/$repo",
    },
  },
}
