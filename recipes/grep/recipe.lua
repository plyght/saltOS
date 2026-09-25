local version = "3.11"

return {
  name = "grep",
  version = version,
  release = 1,
  summary = "GNU grep pattern matching utility",
  license = "GPL-3.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://ftp.gnu.org/gnu/grep/grep-" .. version .. ".tar.xz",
    sha256 = "1db2aedde89d0dea42b16d9528f894c8d15dae4e190b59aecc78f5a951276eab",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "glibc" },
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
