local version = "3.1"

return {
  name = "gperf",
  version = version,
  release = 1,
  summary = "Perfect hash function generator",
  license = "GPL-3.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://ftp.gnu.org/gnu/gperf/gperf-" .. version .. ".tar.gz",
    sha256 = "588546b945bba4b70b6a3a616e80b4ab466e3f33024a352fc2198112cdbb3ae2",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make" },
  },
  package = {
    deps = { "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
