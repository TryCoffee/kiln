#!/bin/sh
# Installs a built package on a fresh distribution container and checks it:
#   packaging/linux/smoke-test.sh debian|ubuntu|fedora|arch PACKAGE [OUTDIR]
# Checks: the package manager installs it with its dependencies; no library is
# missing; `kiln --version` runs; the polkit policy names the helper that was
# installed; the desktop file and icon are in place; the GUI renders offscreen
# (screenshot to OUTDIR/gui-DISTRO.png); removing the package removes it all.
# The container needs network for the dependencies.
set -eu
distro=${1:-}; pkg=${2:-}
[ -f "$pkg" ] || { echo "usage: $0 debian|ubuntu|fedora|arch PACKAGE [outdir]" >&2; exit 64; }
pkgdir=$(cd "$(dirname "$pkg")" && pwd); pkgname=$(basename "$pkg")
out=$(mkdir -p "${3:-$pkgdir}" && cd "${3:-$pkgdir}" && pwd)
case $distro in
debian) image=debian:trixie
        install="apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq fonts-dejavu-core /pkg/$pkgname"
        remove="apt-get remove -y -qq kiln" ;;
ubuntu) image=ubuntu:26.04
        install="apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq fonts-dejavu-core /pkg/$pkgname"
        remove="apt-get remove -y -qq kiln" ;;
fedora) image=fedora:44
        install="dnf -y -q install dejavu-sans-fonts /pkg/$pkgname"
        remove="dnf -y -q remove kiln" ;;
arch)   image=archlinux:latest
        install="pacman -Syu --noconfirm -q ttf-dejavu && pacman -U --noconfirm /pkg/$pkgname"
        remove="pacman -R --noconfirm kiln" ;;
*) echo "unknown distribution $distro" >&2; exit 64 ;;
esac
# --network host: the image build needs no bridge (works without the veth module).
docker run --rm --network host -v "$pkgdir:/pkg:ro" -v "$out:/out" -e OWNER="$(id -u):$(id -g)" -e DISTRO="$distro" \
    "$image" sh -euc "
$install >/tmp/install.log 2>&1 || { tail -30 /tmp/install.log; exit 1; }
fail=0
ok() { echo \"PASS  \$1\"; }
bad() { echo \"FAIL  \$1\"; fail=1; }
v=\$(kiln --version 2>&1) && ok \"kiln --version: \$v\" || bad \"kiln --version: \$v\"
for f in /usr/bin/kiln /usr/bin/kiln-gui; do
    if ldd \$f | grep -q 'not found'; then bad \"\$f: missing libraries\"; else ok \"\$f: all libraries found\"; fi
done
policy=/usr/share/polkit-1/actions/io.github.trycoffee.kiln.policy
helper=\$(sed -n 's,.*exec.path\">\\(.*\\)</annotate>.*,\\1,p' \$policy)
[ -x \"\$helper\" ] && ok \"polkit policy names the installed helper (\$helper)\" || bad \"policy names \$helper, not installed\"
[ -x /usr/bin/pkexec ] && ok 'pkexec is installed' || bad 'pkexec missing'
grep -q '^Icon=io.github.trycoffee.kiln\$' /usr/share/applications/kiln.desktop && ok 'desktop file names the Kiln icon' || bad 'desktop file icon'
[ -f /usr/share/icons/hicolor/scalable/apps/io.github.trycoffee.kiln.svg ] && ok 'scalable icon installed' || bad 'scalable icon'
case \$DISTRO in
debian|ubuntu) grep -q 'Third-party notices' /usr/share/doc/kiln/copyright && grep -q 'GNU LESSER GENERAL PUBLIC' /usr/share/doc/kiln/copyright \
                   && ok 'licences in /usr/share/doc/kiln/copyright' || bad 'no /usr/share/doc/kiln/copyright' ;;
*) [ -f /usr/share/licenses/kiln/THIRD-PARTY-NOTICES.md ] && [ -f /usr/share/licenses/kiln/ventoy/license-grub2.txt ] \
       && ok 'third-party notices and licence texts installed' || bad 'third-party notices' ;;
esac
QT_QPA_PLATFORM=offscreen timeout 60 kiln-gui --screenshot /tmp/gui.png >/tmp/gui.log 2>&1 || true
if [ -s /tmp/gui.png ]; then install -o \"\${OWNER%:*}\" -g \"\${OWNER#*:}\" -m 644 /tmp/gui.png /out/gui-\$DISTRO.png; ok 'GUI renders offscreen'; else bad 'GUI did not render'; cat /tmp/gui.log; fi
$remove >/tmp/remove.log 2>&1 || { tail -20 /tmp/remove.log; bad 'package removal'; }
[ ! -e /usr/bin/kiln ] && [ ! -e \"\$helper\" ] && [ ! -e \$policy ] && ok 'removal removes the programs and the policy' || bad 'files left after removal'
exit \$fail
"
