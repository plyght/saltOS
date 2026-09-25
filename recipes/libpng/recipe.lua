local version = "1.6.43"

return {
  name = "libpng",
  version = version,
  release = 1,
  summary = "Portable Network Graphics reference library",
  license = "Libpng",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://download.sourceforge.net/libpng/libpng-" .. version .. ".tar.xz",
    sha256 = "6a5ca0652392a2d7c9db2ae5b40210843c0bbc081cbd410825ab00cc59f14a6c",
  },
  build = {
    system = "autotools",
    deps = { "pkgconf", "gcc", "make", "zlib" },
  },
  package = {
    deps = { "glibc", "zlib" },
  },
  reproducibility = {
    status = "verified",
  },
}
