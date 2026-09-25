return {
  system = {
    hostname = "saltos-arm64",
    locale = "en_US.UTF-8",
    timezone = "UTC",
    keymap = "us",
  },
  install = {
    disk = "/dev/vda",
    mode = "erase",
    filesystem = "btrfs",
    encrypt = false,
    swap = "file",
    swap_size = "512M",
    desktop = "none",
  },
  boot = {
    firmware = "auto",
    os_prober = false,
    cmdline = "console=tty0 console=ttyAMA0,115200",
  },
  user = {
    name = "tester",
    password = "saltos",
    sudo = true,
  },
  network = {
    mode = "dhcp",
  },
  kernel = {
    source = "native",
  },
  stratum = {
    {
      name = "alpine",
      role = "primary",
    },
  },
}
