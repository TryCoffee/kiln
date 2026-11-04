#!/bin/sh
# Runs inside a build image (see build.sh): /src = the source tree (read-only),
# /dl = the third-party archives (read-only), /out = where the package goes.
# No network: everything the build needs is already in /dl.
#   build-in-container.sh deb | rpm
set -eu
fmt=$1
b=/tmp/build
case $fmt in
deb)
    # Debian's own compiler flags, with every hardening option on.
    eval "$(DEB_BUILD_MAINT_OPTIONS=hardening=+all dpkg-buildflags --export=sh)"
    set -- -DCMAKE_C_COMPILER=clang-19 -DKILN_LICENSE_DIR=share/doc/kiln -DKILN_DEBIAN_COPYRIGHT=ON
    gen=DEB ;;
rpm)
    # Fedora's own compiler flags (hardened; no package note: that needs rpmbuild's spec).
    CFLAGS=$(rpm --define '_package_note_flags %{nil}' --eval '%{build_cflags}')
    CXXFLAGS=$(rpm --define '_package_note_flags %{nil}' --eval '%{build_cxxflags}')
    LDFLAGS=$(rpm --define '_package_note_flags %{nil}' --eval '%{build_ldflags}')
    export CFLAGS CXXFLAGS LDFLAGS
    set --
    gen=RPM ;;
*)
    echo "usage: $0 deb|rpm" >&2; exit 64 ;;
esac
cmake -S /src -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      -DKILN_DOWNLOAD_DIR=/dl "$@"
cmake --build "$b"
ctest --test-dir "$b" --output-on-failure
cd "$b" && cpack -G "$gen" -B /tmp/cpack
cp /tmp/cpack/*.deb /tmp/cpack/*.rpm /out/ 2>/dev/null || true
