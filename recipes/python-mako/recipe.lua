local version = "1.3.5"

return {
  name = "python-mako",
  version = version,
  release = 1,
  summary = "Mako templates for Python (build-time module)",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://files.pythonhosted.org/packages/source/M/Mako/Mako-" .. version .. ".tar.gz",
    sha256 = "48dbc20568c1d276a2698b36d968fa76161bf127194907ea6fc594fa81f943bc",
  },
  build = {
    system = "custom",
    deps = { "python", "python-markupsafe" },
    script = [[
#!/bin/sh
site=$(python3 -c "import sysconfig; print(sysconfig.get_paths()['purelib'])")
install -d "$SALT_DEST$site"
cp -r mako "$SALT_DEST$site/"
]],
  },
  package = {
    deps = { "python", "python-markupsafe" },
  },
  reproducibility = {
    status = "verified",
  },
}
