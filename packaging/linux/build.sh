#!/bin/sh
# Builds Kiln's Linux packages in containers, offline:
#   packaging/linux/build.sh arch|debian|fedora [OUTDIR]
# OUTDIR (default dist/) gets the package, the source archive the package was
# built from (kiln-VERSION.tar.gz: the tracked + untracked-but-not-ignored
# files of this tree) and sources/ (the pinned third-party archives).
# Needs docker; the images (kiln-build-DISTRO) are built on first use.
set -eu
distro=${1:-}
case $distro in arch|debian|fedora) ;; *) echo "usage: $0 arch|debian|fedora [outdir]" >&2; exit 64 ;; esac
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mkdir -p "${2:-$root/dist}" && cd "${2:-$root/dist}" && pwd)
dl=$out/sources
mkdir -p "$dl"
version=$(sed -n 's/^project(kiln VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || { echo "no version in CMakeLists.txt" >&2; exit 1; }

cmake -DKILN_DOWNLOAD_DIR="$dl" -P "$root/cmake/fetch-third-party.cmake"
tarball=$out/kiln-$version.tar.gz
(cd "$root" && git ls-files -co --exclude-standard -z | grep -z -v '^dist/' \
   | tar --null -T - --transform "s,^,kiln-$version/," --sort=name --owner=0 --group=0 -czf "$tarball")

image=kiln-build-$distro
docker build -q --network host -t "$image" -f "$root/packaging/linux/Dockerfile.$distro" "$root/packaging/linux" >/dev/null

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar -xzf "$tarball" -C "$work"
src=$work/kiln-$version
case $distro in
debian|fedora)
    fmt=deb; [ "$distro" = fedora ] && fmt=rpm
    docker run --rm --network none -v "$src:/src:ro" -v "$dl:/dl:ro" -v "$out:/out" \
        --user "$(id -u):$(id -g)" -e HOME=/tmp "$image" sh /src/packaging/linux/build-in-container.sh "$fmt" ;;
arch)
    pkg=$work/pkg
    mkdir -p "$pkg"
    cp "$src/packaging/arch/PKGBUILD" "$tarball" "$dl"/* "$pkg/"
    chmod -R a+rwX "$work"
    docker run --rm --network none -v "$pkg:/pkg" -v "$out:/out" -e OWNER="$(id -u):$(id -g)" "$image" \
        sh -c 'cd /pkg && su builder -c "makepkg -f --noconfirm" && install -o "${OWNER%:*}" -g "${OWNER#*:}" -m 644 /pkg/*.pkg.tar.zst /out/' ;;
esac
ls -l "$out"
