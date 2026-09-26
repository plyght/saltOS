local version = "24.1.5"

return {
  name = "mesa",
  version = version,
  release = 3,
  summary = "OpenGL, EGL and GBM implementation (Gallium drivers)",
  license = "MIT",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://archive.mesa3d.org/mesa-" .. version .. ".tar.xz",
    sha256 = "02761ffd965dd64b95421ebfca1191d73724aba00f30034009237564f34cf976",
  },
  build = {
    system = "meson",
    deps = {
      "meson",
      "ninja",
      "pkgconf",
      "gcc",
      "llvm",
      "libdrm",
      "expat",
      "zlib",
      "zstd",
      "elfutils",
      "eudev",
      "bison",
      "flex",
      "python-mako",
      "python-pyyaml",
      "libx11",
      "libxext",
      "libxfixes",
      "libxrender",
      "libxrandr",
      "libxi",
      "libxcursor",
      "libxcb",
      "libxshmfence",
      "libxxf86vm",
      "wayland",
      "wayland-protocols",
    },
    script = [[
#!/bin/sh
case "$SALT_ARCH" in
  x86_64) gallium="swrast,virgl,crocus,radeonsi,nouveau,zink" ;;
  *) gallium="swrast,virgl,radeonsi,nouveau,v3d,vc4,panfrost,freedreno,lima,zink" ;;
esac
# iris needs intel-clc (clang + libclc + SPIRV-LLVM-Translator), which the
# native stack does not build yet; gen8+ Intel uses llvmpipe until it does.
meson setup build --prefix=/usr --libdir=lib --buildtype=release -Dplatforms=x11,wayland -Dgallium-drivers="$gallium" -Dvulkan-drivers= -Dglx=dri -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dshared-glapi=enabled -Dllvm=enabled -Dshared-llvm=enabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false -Dvideo-codecs= -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dgallium-nine=false -Dgallium-opencl=disabled -Dgallium-rusticl=false -Dmicrosoft-clc=disabled -Dosmesa=false
ninja -C build -j"$SALT_JOBS"
DESTDIR="$SALT_DEST" ninja -C build install
]],
  },
  package = {
    deps = {
      "glibc",
      "llvm",
      "libdrm",
      "expat",
      "zlib",
      "zstd",
      "elfutils",
      "eudev",
      "libx11",
      "libxext",
      "libxfixes",
      "libxcb",
      "libxshmfence",
      "libxxf86vm",
      "libxrandr",
      "wayland",
    },
  },
  reproducibility = {
    status = "verified",
  },
}
