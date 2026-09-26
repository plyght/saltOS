local version = "2.47"

return {
  name = "perl-xml-parser",
  version = version,
  release = 1,
  summary = "Perl XML::Parser, an expat binding (needed by intltool)",
  license = "Artistic-1.0-Perl OR GPL-1.0-or-later",
  arch = { "x86_64", "aarch64" },
  source = {
    url = "https://cpan.metacpan.org/authors/id/T/TO/TODDR/XML-Parser-" .. version .. ".tar.gz",
    sha256 = "ad4aae643ec784f489b956abe952432871a622d4e2b5c619e8855accbfc4d1d8",
  },
  build = {
    system = "custom",
    deps = { "gcc", "make", "perl", "expat" },
    script = [[
#!/bin/sh
set -e
perl Makefile.PL INSTALLDIRS=vendor
make -j"$SALT_JOBS"
make DESTDIR="$SALT_DEST" install
# perllocal.pod is per-machine state every module install would fight over.
find "$SALT_DEST" \( -name perllocal.pod -o -name .packlist \) -delete
]],
  },
  package = {
    deps = { "glibc", "perl", "expat" },
  },
  reproducibility = {
    status = "verified",
  },
}
