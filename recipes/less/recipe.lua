local version = "643"

return {
  name = "less",
  version = version,
  release = 1,
  summary = "Terminal pager program for viewing text files",
  license = "GPL-3.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://www.greenwoodsoftware.com/less/less-" .. version .. ".tar.gz",
    sha256 = "2911b5432c836fa084c8a2e68f6cd6312372c026a58faaa98862731c8b6052e8",
  },
  build = {
    system = "autotools",
    deps = { "gcc", "make", "ncurses", "glibc" },
  },
  package = {
    deps = { "ncurses", "glibc" },
  },
  reproducibility = {
    status = "verified",
  },
}
