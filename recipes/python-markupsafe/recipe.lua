local version = "2.1.5"

return {
  name = "python-markupsafe",
  version = version,
  release = 1,
  summary = "Safe string markup for Python (build-time module)",
  license = "BSD-3-Clause",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://files.pythonhosted.org/packages/source/M/MarkupSafe/MarkupSafe-" .. version .. ".tar.gz",
    sha256 = "d283d37a890ba4c1ae73ffadf8046435c76e7bc2247bbb63c00bd1a709c6544b",
  },
  build = {
    system = "custom",
    deps = { "python" },
    script = [[
#!/bin/sh
site=$(python3 -c "import sysconfig; print(sysconfig.get_paths()['purelib'])")
install -d "$SALT_DEST$site"
cp -r src/markupsafe "$SALT_DEST$site/"
rm -f "$SALT_DEST$site/markupsafe/"*.c
]],
  },
  package = {
    deps = { "python" },
  },
  reproducibility = {
    status = "verified",
  },
}
