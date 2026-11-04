# Third-party notices

Kiln is free software under the GNU General Public License, version 3 or later
(`LICENSE`). The programs `kiln`, `kiln-gui` and `kiln-helper` have the
following third-party code built into them. Each one's licence text is
installed next to this file, in the folder named after it.

| Component | Version | Licence | Licence text |
|---|---|---|---|
| libcdio (libcdio, libiso9660, libudf) | 2.4.0 | GPL-3.0-or-later | `libcdio/COPYING` |
| wimlib (libwim), with Kiln's patch | 1.14.5 | GPL-3.0-or-later or LGPL-2.1-or-later (Kiln uses it under the GPL) | `wimlib/` |
| hivex (the library only) | 1.3.24 | LGPL-2.1-or-later | `hivex/LICENSE` |
| FatFs by ChaN | R0.16 | 1-clause BSD-style | `fatfs/LICENSE.txt` |
| XZ Embedded by Lasse Collin | 2024-12-30 | 0BSD | `xz-embedded/LICENSE.txt` |
| Ventoy by longpanda: `boot.img`, `core.img`, `ventoy.disk.img` | 1.1.18 | GPL-3.0-or-later; the parts inside them (GRUB, shim, iPXE, wimboot and others) under their own licences | `ventoy/` |
| GNU libiconv (Windows only) | 1.19 | LGPL-2.1-or-later | `libiconv/COPYING.LIB` |

The Ventoy files are taken unmodified from Ventoy's official release. They
are written onto a drive only when you make a Ventoy disk.

The graphical interface uses the Qt toolkit (LGPL-3.0), linked as a shared
library: on Linux it is your distribution's Qt package. The macOS app carries
Qt's official build in `Kiln.app/Contents/Frameworks` and `PlugIns`, and the
Windows packages carry Qt's official build for Windows (llvm-mingw) next to
`kiln-gui.exe`; what those files contain, their licences and where Qt's source
is: `qt/NOTICES.md`.

On Windows, Kiln is built with llvm-mingw and ships its C++ runtime,
`libc++.dll` and `libunwind.dll` (LLVM, Apache-2.0 WITH LLVM-exception) and
`libwinpthread-1.dll` (MinGW-w64's winpthreads, MIT and BSD-style), and contains
parts of the MinGW-w64 runtime; their notices are in `llvm-mingw/`.

Kiln's code for building the Windows answer file (`src/unattend.cpp`) is
ported from Rufus (GPL-3.0-or-later), with attribution in that file.

## Source code

Kiln's source code is at https://github.com/TryCoffee/kiln. Every release on
https://github.com/TryCoffee/kiln/releases carries the source of that release
and the unmodified upstream source archives of the components above that are
under the GPL or LGPL (libcdio, wimlib, hivex, Ventoy, libiconv). Kiln's patch to wimlib
is part of Kiln's source (`third_party/wimlib/`). The build checks every
archive against a pinned SHA-256.

- libcdio: https://github.com/libcdio/libcdio-C/releases/tag/2.4.0
- wimlib: https://wimlib.net/downloads/wimlib-1.14.5.tar.gz
- hivex: https://download.libguestfs.org/hivex/hivex-1.3.24.tar.gz
- Ventoy: https://github.com/ventoy/Ventoy/tree/v1.1.18 (attached to each
  release as `Ventoy-1.1.18-source.tar.gz`; the release archive Kiln takes the
  three files from is `ventoy-1.1.18-linux.tar.gz`)
- libiconv: https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.19.tar.gz
