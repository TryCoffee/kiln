#!/bin/bash
# Builds Kiln for Windows and packages it, in an MSYS2 shell (UCRT64) on Windows:
#   packaging/windows/make-installer.sh QTPREFIX LLVM_MINGW [OUTDIR]
# QTPREFIX, LLVM_MINGW: from packaging/windows/fetch-toolchain.sh (Qt's official
# llvm-mingw build and the llvm-mingw it was made with). MSYS2 provides only the
# tools: cmake, ninja, make, patch, sh, python3. Inno Setup 6 must be installed
# (ISCC=<path> to choose one). OUTDIR (default dist/) gets
#   Kiln-VERSION-x64-setup.exe   the installer
#   Kiln-VERSION-x64.zip         the same files, portable
#   gui-windows.png              the window, rendered offscreen
#   sources/                     the pinned third-party archives
set -euo pipefail

QT=$(cd "${1:?usage: $0 QTPREFIX LLVM_MINGW [OUTDIR]}" && pwd)
LLVM=$(cd "${2:?usage: $0 QTPREFIX LLVM_MINGW [OUTDIR]}" && pwd)
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${3:-$root/dist}
mkdir -p "$out" && out=$(cd "$out" && pwd)
version=$(sed -n 's/^project(kiln VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
fail() { echo "make-installer: $*" >&2; exit 1; }
win() { cygpath -m "$1"; }

iscc=${ISCC:-}
for c in "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe" "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
         "/c/Program Files/Inno Setup 6/ISCC.exe"; do
    [ -n "$iscc" ] && break
    c=$(cygpath -u "$c" 2>/dev/null || echo "$c")
    [ -f "$c" ] && iscc=$c
done
[ -n "$iscc" ] && [ -f "$iscc" ] || fail "Inno Setup 6 (ISCC.exe) not found; set ISCC=<path>"
sbom=$(ls "$QT"/sbom/qtbase-*.spdx.json 2>/dev/null | head -1)
[ -n "$sbom" ] || fail "no Qt SBOM in $QT/sbom"
rt=$LLVM/x86_64-w64-mingw32
[ -f "$rt/bin/libc++.dll" ] || fail "$rt/bin/libc++.dll not found: is $LLVM llvm-mingw?"
export PATH="$LLVM/bin:$PATH"
echo "Kiln $version, x64, Qt from $QT, $(clang --version | head -1)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cmake -DKILN_DOWNLOAD_DIR="$(win "$out/sources")" -P "$(win "$root/cmake/fetch-third-party.cmake")"
cmake -S "$(win "$root")" -B "$(win "$work/build")" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$(win "$LLVM/bin/x86_64-w64-mingw32-clang.exe")" \
      -DCMAKE_CXX_COMPILER="$(win "$LLVM/bin/x86_64-w64-mingw32-clang++.exe")" \
      -DCMAKE_RC_COMPILER="$(win "$LLVM/bin/x86_64-w64-mingw32-windres.exe")" \
      -DCMAKE_AR="$(win "$LLVM/bin/llvm-ar.exe")" -DCMAKE_RANLIB="$(win "$LLVM/bin/llvm-ranlib.exe")" \
      -DCMAKE_PREFIX_PATH="$(win "$QT")" -DCMAKE_IGNORE_PREFIX_PATH="C:/msys64/ucrt64;C:/msys64/usr" \
      -DCMAKE_INSTALL_BINDIR=. -DKILN_DOWNLOAD_DIR="$(win "$out/sources")"
cmake --build "$(win "$work/build")"
# The tests find libc++ and Qt here; the shipped programs find them beside themselves.
PATH="$rt/bin:$QT/bin:$PATH" ctest --test-dir "$(win "$work/build")" --output-on-failure
stage=$work/Kiln-$version
cmake --install "$(win "$work/build")" --prefix "$(win "$stage")"
for e in kiln kiln-gui kiln-helper; do [ -f "$stage/$e.exe" ] || fail "the install did not produce $e.exe"; done

# Qt's DLLs and the plug-ins the GUI uses; the C++ runtime from llvm-mingw. No
# --release: windeployqt reads every DLL of Qt's llvm-mingw build as "debug" (they
# carry debug-info sections), so --release would leave out qwindows.dll itself.
"$QT/bin/windeployqt6.exe" --no-translations --no-compiler-runtime --no-opengl-sw \
    --no-system-d3d-compiler --no-system-dxc-compiler --skip-plugin-types tls,networkinformation,generic \
    "$(win "$stage/kiln-gui.exe")"
cp "$rt/bin/libc++.dll" "$rt/bin/libunwind.dll" "$rt/bin/libwinpthread-1.dll" "$stage/"
mkdir -p "$stage/licenses/llvm-mingw"
cp "$LLVM/LICENSE.TXT" "$rt/share/mingw32/COPYING.MinGW-w64-runtime.txt" "$rt/share/mingw32/COPYING.winpthreads.txt" \
   "$stage/licenses/llvm-mingw/"
python3 -I "$(win "$root/packaging/qt-notices.py")" "$(win "$sbom")" "$(win "$stage")" \
        "$(win "$root/third_party/qt/LICENSES");$(win "$root/third_party/qt/extra")" "$(win "$stage/licenses/qt")"

# Check what ships: every DLL any file imports is next to it or part of Windows.
echo "--- checking the files ---"
bad=0
sys=/c/Windows/System32
while read -r f; do
    rel=${f#$stage/}
    for d in $(llvm-objdump -p "$f" | awk '/DLL Name:/{print $3}'); do
        if [ -f "$stage/$d" ] || [ -f "$(dirname "$f")/$d" ]; then continue; fi
        # api-ms-win-* are API sets Windows resolves itself; everything else must be in System32
        case $d in api-ms-win-*|API-MS-WIN-*) continue ;; esac
        [ -f "$sys/$d" ] || [ -f "$sys/${d,,}" ] || { echo "  BAD: $rel needs $d"; bad=1; }
        case ${d,,} in libstdc++*|libgcc*|libiconv*|libintl*|qt6*d.dll) echo "  BAD: $rel needs $d"; bad=1 ;; esac
    done
done < <(find "$stage" -type f \( -name '*.exe' -o -name '*.dll' \))
[ -f "$stage/platforms/qwindows.dll" ] || { echo "  BAD: no platforms/qwindows.dll"; bad=1; }
[ -f "$stage/licenses/THIRD-PARTY-NOTICES.md" ] && [ -f "$stage/licenses/qt/NOTICES.md" ] \
    && [ -f "$stage/licenses/libiconv/COPYING.LIB" ] || { echo "  BAD: notices missing"; bad=1; }
[ "$bad" = 0 ] || fail "the files failed their checks (above)"
ls "$stage"

# They run with nothing but Windows on PATH: the command line, and the GUI
# rendered offscreen (on a copy with Qt's offscreen platform plug-in added).
PATH=$sys "$stage/kiln.exe" --version
cp -r "$stage" "$work/smoke"
cp "$QT/plugins/platforms/qoffscreen.dll" "$work/smoke/platforms/"
rm -f "$out/gui-windows.png"
QT_QPA_PLATFORM=offscreen PATH=$sys "$work/smoke/kiln-gui.exe" --screenshot "$(win "$out/gui-windows.png")"
[ -s "$out/gui-windows.png" ] || fail "the GUI did not render"

# The portable zip and the installer.
rm -f "$out/Kiln-$version-x64.zip" "$out/Kiln-$version-x64-setup.exe"
(cd "$work" && /c/Windows/System32/tar.exe -a -c -f "$(win "$out/Kiln-$version-x64.zip")" "Kiln-$version")
# MSYS2_ARG_CONV_EXCL: MSYS2 would turn /Q, /DVERSION=... into C:/msys64/... paths.
MSYS2_ARG_CONV_EXCL='*' "$iscc" /Q "/DVERSION=$version" "/DSRC=$(cygpath -w "$stage")" "/DOUT=$(cygpath -w "$out")" \
        "/DICON=$(cygpath -w "$root/data/icons/kiln.ico")" "$(cygpath -w "$root/packaging/windows/kiln.iss")"
setup=$out/Kiln-$version-x64-setup.exe
[ -f "$setup" ] || fail "Inno Setup made no installer"

# The installer installs, the installed copy runs, the uninstaller removes it.
if [ "${KILN_TEST_INSTALL:-1}" = 1 ]; then
    dir=$(cygpath -w "$work/installed")
    MSYS2_ARG_CONV_EXCL='*' "$setup" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART "/DIR=$dir" "/LOG=$(cygpath -w "$work/setup.log")" \
        || fail "the installer failed (log: $work/setup.log)"
    PATH=$sys "$work/installed/kiln.exe" --version || fail "the installed kiln.exe does not run"
    [ -f "$work/installed/platforms/qwindows.dll" ] || fail "the installed copy has no platforms/qwindows.dll"
    MSYS2_ARG_CONV_EXCL='*' "$work/installed/unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -e "$work/installed/kiln.exe" ] || break; sleep 1; done
    [ ! -e "$work/installed/kiln.exe" ] || fail "the uninstaller left kiln.exe behind"
    echo "installer: installed, ran, uninstalled"
fi
sha256sum "$setup" "$out/Kiln-$version-x64.zip"
ls -l "$setup" "$out/Kiln-$version-x64.zip"
