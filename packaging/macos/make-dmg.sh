#!/bin/bash
# Builds Kiln.app and Kiln-VERSION-arm64.dmg on a Mac:
#   packaging/macos/make-dmg.sh QTPREFIX [OUTDIR]
# QTPREFIX: an official Qt build for macOS (e.g. aqt install-qt mac desktop 6.12.0
# clang_64 --archives qtbase -> <dir>/6.12.0/macos). OUTDIR (default dist/) gets
# the .dmg, sources/ (the pinned third-party archives) and gui-macos.png.
# Needs Xcode's command line tools, CMake and Ninja on PATH, and python3.
#
# The app: Kiln (GUI) in Contents/MacOS, the command line in Contents/Helpers/kiln,
# Qt's frameworks and plug-ins (macdeployqt, thinned to arm64), licence texts and
# notices in Contents/Resources/licenses. Signed ad hoc (no Developer ID): macOS
# asks before the first start of a downloaded copy.
set -euo pipefail

QT=$(cd "${1:?usage: $0 QTPREFIX [OUTDIR]}" && pwd)
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${2:-$root/dist}
mkdir -p "$out" && out=$(cd "$out" && pwd)
version=$(sed -n 's/^project(kiln VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
arch=arm64
fail() { echo "make-dmg: $*" >&2; exit 1; }
[ -x "$QT/bin/macdeployqt" ] || fail "$QT/bin/macdeployqt not found"
sbom=$(ls "$QT"/sbom/qtbase-*.spdx.json 2>/dev/null | head -1)
[ -n "$sbom" ] || fail "no Qt SBOM in $QT/sbom"

# The oldest macOS this Qt runs on: everything else is built for it too.
minos=$(otool -l "$QT/lib/QtCore.framework/Versions/A/QtCore" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
[ -n "$minos" ] || fail "cannot read QtCore's minimum macOS"
export MACOSX_DEPLOYMENT_TARGET=$minos
echo "Kiln $version, $arch, macOS $minos+, Qt from $QT"

work=$(mktemp -d)
trap 'hdiutil detach -quiet "$work/mnt" 2>/dev/null || true; rm -rf "$work"' EXIT

cmake -DKILN_DOWNLOAD_DIR="$out/sources" -P "$root/cmake/fetch-third-party.cmake"
cmake -S "$root" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$QT" -DCMAKE_OSX_ARCHITECTURES=$arch -DCMAKE_OSX_DEPLOYMENT_TARGET="$minos" \
      -DKILN_DOWNLOAD_DIR="$out/sources"
cmake --build "$work/build"
ctest --test-dir "$work/build" --output-on-failure
cmake --install "$work/build" --prefix "$work/stage"
APP=$work/stage/Kiln.app
C=$APP/Contents
[ -x "$C/MacOS/Kiln" ] && [ -x "$C/Helpers/kiln" ] || fail "the install did not produce Kiln.app with Helpers/kiln"

# Qt's frameworks and the plug-ins the app uses. The installed program has no run
# path into $QT (cmake --install drops the build tree's), so macdeployqt is told
# where Qt is; without -libpath it cannot resolve @rpath/Qt*.framework.
"$QT/bin/macdeployqt" "$APP" -verbose=1 -libpath="$QT/lib"
# The run path into $QT was only for macdeployqt: Kiln keeps @executable_path/../Frameworks.
for rp in $(otool -l "$C/MacOS/Kiln" | awk '/LC_RPATH/{g=1} g&&/path /{print $2; g=0}'); do
    case $rp in @*) ;; *) install_name_tool -delete_rpath "$rp" "$C/MacOS/Kiln" ;; esac
done
# Official Qt is universal: keep only $arch.
find "$C" -type f | while read -r f; do
    if file "$f" | grep -q 'universal binary'; then
        lipo -thin $arch -output "$f.thin" "$f" && mv "$f.thin" "$f"
    fi
done

python3 -I "$root/packaging/macos/qt-notices.py" "$sbom" "$C" "$root/third_party/qt/LICENSES:$root/third_party/qt/extra" \
        "$C/Resources/licenses/qt"

# Sign ad hoc, inside out.
sign() { codesign --force --sign - --timestamp=none "$@"; }
find "$C/Frameworks" "$C/PlugIns" -type f -name '*.dylib' | while read -r f; do sign "$f"; done
for fw in "$C"/Frameworks/*.framework; do sign "$fw"; done
sign "$C/Helpers/kiln"
sign --identifier io.github.trycoffee.kiln "$APP"

# Check the bundle itself: every library it loads is in it or part of macOS,
# nothing is newer than $minos, no foreign run paths, the signature holds.
echo "--- checking Kiln.app ---"
bad=0
while read -r f; do
    file "$f" | grep -q 'Mach-O' || continue
    rel=${f#$APP/}
    file "$f" | grep -q "$arch" || { echo "  BAD: $rel is not $arch"; bad=1; }
    for d in $(otool -L "$f" | sed 1d | awk '{print $1}'); do
        case $d in
            /System/*|/usr/lib/*|@loader_path/*|@executable_path/*) ;;
            @rpath/*) [ -e "$C/Frameworks/${d#@rpath/}" ] || { echo "  BAD: $rel needs $d"; bad=1; } ;;
            *) echo "  BAD: $rel needs $d (outside the app and macOS)"; bad=1 ;;
        esac
    done
    m=$(otool -l "$f" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
    if [ -n "$m" ] && [ "$(printf '%s\n%s\n' "$m" "$minos" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)" != "$minos" ]; then
        echo "  BAD: $rel needs macOS $m"; bad=1
    fi
    for rp in $(otool -l "$f" | awk '/LC_RPATH/{g=1} g&&/path /{print $2; g=0}'); do
        case $rp in @executable_path/*|@loader_path/*) ;; *) echo "  BAD: $rel has run path $rp"; bad=1 ;; esac
    done
done < <(find "$C" -type f \( -perm -u+x -o -name '*.dylib' \))
otool -l "$C/MacOS/Kiln" | grep -q 'path @executable_path/../Frameworks ' \
    || { echo "  BAD: Kiln has no run path to Contents/Frameworks"; bad=1; }
codesign --verify --strict --deep "$APP" || { echo "  BAD: the signature does not verify"; bad=1; }
plutil -lint "$C/Info.plist" >/dev/null || { echo "  BAD: Info.plist"; bad=1; }
[ -f "$C/Resources/kiln.icns" ] || { echo "  BAD: no icon"; bad=1; }
[ -f "$C/Resources/licenses/THIRD-PARTY-NOTICES.md" ] && [ -f "$C/Resources/licenses/qt/NOTICES.md" ] \
    || { echo "  BAD: notices missing"; bad=1; }
[ "$bad" = 0 ] || fail "Kiln.app failed its checks (above)"

# It runs: the command line, and the GUI rendered offscreen. The offscreen
# platform plug-in is not shipped, so that runs on a copy that has it added.
"$C/Helpers/kiln" --version
cp -R "$APP" "$work/Smoke.app"
mkdir -p "$work/Smoke.app/Contents/PlugIns/platforms"
lipo -thin $arch -output "$work/Smoke.app/Contents/PlugIns/platforms/libqoffscreen.dylib" \
     "$QT/plugins/platforms/libqoffscreen.dylib" 2>/dev/null \
  || cp "$QT/plugins/platforms/libqoffscreen.dylib" "$work/Smoke.app/Contents/PlugIns/platforms/"
codesign --force --deep --sign - "$work/Smoke.app" >/dev/null
QT_QPA_PLATFORM=offscreen "$work/Smoke.app/Contents/MacOS/Kiln" --screenshot "$out/gui-macos.png"
[ -s "$out/gui-macos.png" ] || fail "the GUI did not render"

# The disk image: Kiln.app and a link to /Applications.
dmg=$out/Kiln-$version-$arch.dmg
mkdir "$work/dmg"
cp -R "$APP" "$work/dmg/"
ln -s /Applications "$work/dmg/Applications"
rm -f "$dmg"
hdiutil create -quiet -volname "Kiln $version" -srcfolder "$work/dmg" -fs HFS+ -format UDZO "$dmg"
hdiutil verify -quiet "$dmg"
mkdir "$work/mnt"
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$work/mnt" "$dmg"
codesign --verify --strict --deep "$work/mnt/Kiln.app" || fail "the app in the disk image does not verify"
"$work/mnt/Kiln.app/Contents/Helpers/kiln" --version >/dev/null || fail "the app in the disk image does not run"
hdiutil detach -quiet "$work/mnt"
shasum -a 256 "$dmg"
ls -l "$dmg"
