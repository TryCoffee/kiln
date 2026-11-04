#!/bin/bash
# Downloads and unpacks what the Windows packages are built with, pinned by SHA-256:
#   packaging/windows/fetch-toolchain.sh DIR
# DIR/qt          Qt 6.12.0's official build for Windows with llvm-mingw (qtbase only)
# DIR/llvm-mingw  the llvm-mingw toolchain that Qt build was made with (LLVM 22.1.7, UCRT)
# Run in an MSYS2 shell on Windows (needs curl, sha256sum and Windows' own tar.exe,
# which unpacks .7z). Archives already in DIR with the right SHA-256 are kept.
set -euo pipefail
dir=${1:?usage: $0 DIR}
mkdir -p "$dir" && cd "$dir"

repo=https://download.qt.io/online/qtsdkrepository/windows_x86/desktop
qt_url=$repo/qt6_6120/qt6_6120_llvm_mingw/qt.qt6.6120.win64_llvm_mingw/6.12.0-0-202609280346qtbase-Windows-Windows_11_24H2-Clang-Windows-Windows_11_24H2-X86_64.7z
qt_sha256=391aa3976b33577f4f953958d58c5112aad1c2c760039c49dc178cb0bd1a7db6
llvm_url=$repo/tools_llvm_mingw2217/qt.tools.win64_llvm_mingw2217/22.1.7-202608200704llvm_mingw2217.7z
llvm_sha256=4a38f6ec4c491f6763bc058d41a0a72bcbb3c1f86c88d78e5a46b43f6fa880a0

fetch() {  # url sha256 file
    if [ -f "$3" ] && echo "$2  $3" | sha256sum -c --status; then return; fi
    curl -sSfL --retry 3 -o "$3.part" "$1"
    echo "$2  $3.part" | sha256sum -c --status || { echo "$3: SHA-256 does not match" >&2; rm -f "$3.part"; exit 1; }
    mv "$3.part" "$3"
}
unpack() {  # archive folder
    if [ -f "$2/.from-$(sha256sum "$1" | cut -c1-16)" ]; then return; fi
    rm -rf "$2" && mkdir "$2"
    /c/Windows/System32/tar.exe -xf "$1" -C "$2"
    touch "$2/.from-$(sha256sum "$1" | cut -c1-16)"
}
fetch "$qt_url" $qt_sha256 qtbase.7z
fetch "$llvm_url" $llvm_sha256 llvm-mingw.7z
unpack qtbase.7z qt
unpack llvm-mingw.7z llvm-mingw
echo "Qt:         $PWD/qt"
echo "llvm-mingw: $PWD/llvm-mingw ($(llvm-mingw/bin/clang.exe --version | head -1))"
