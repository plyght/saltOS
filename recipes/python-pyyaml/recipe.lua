local version = "6.0.2"

return {
  name = "python-pyyaml",
  version = version,
  release = 1,
  summary = "YAML parser for Python (build-time module)",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://files.pythonhosted.org/packages/source/p/pyyaml/pyyaml-" .. version .. ".tar.gz",
    sha256 = "d584d9ec91ad65861cc08d42e834324ef890a082e591037abe114850ff7bbc3e",
  },
  build = {
    system = "custom",
    deps = { "python" },
    script = [[
#!/bin/sh
site=$(python3 -c "import sysconfig; print(sysconfig.get_paths()['purelib'])")
install -d "$SALT_DEST$site"
cp -r lib/yaml "$SALT_DEST$site/"
]],
  },
  package = {
    deps = { "python" },
  },
  reproducibility = {
    status = "verified",
  },
}
