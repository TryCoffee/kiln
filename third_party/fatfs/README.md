# FatFs R0.16 (vendored)

From http://elm-chan.org/fsw/ff/arc/ff16.zip
(SHA-256 `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`), by ChaN.
1-clause BSD-style licence, see `LICENSE.txt`.

Files kept: `ff.c`, `ff.h`, `ffunicode.c`, `diskio.h`, `ffconf.h`. Only `ffconf.h` is
changed (the options are listed at its top). The disk I/O glue is Kiln's own
(`src/fat.cpp`).
