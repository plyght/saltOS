local version = "24.1"

return {
  name = "python-packaging",
  version = version,
  release = 1,
  summary = "Python packaging core utilities (build-time module)",
  license = "Apache-2.0",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://files.pythonhosted.org/packages/source/p/packaging/packaging-" .. version .. ".tar.gz",
    sha256 = "026ed72c8ed3fcce5bf8950572258698927fd1dbda10a5e981cdf0ac37f4f002",
  },
  build = {
    system = "custom",
    deps = { "python" },
    script = [[
#!/bin/sh
site=$(python3 -c "import sysconfig; print(sysconfig.get_paths()['purelib'])")
install -d "$SALT_DEST$site"
cp -r src/packaging "$SALT_DEST$site/"
]],
  },
  package = {
    deps = { "python" },
  },
  reproducibility = {
    status = "verified",
  },
}
