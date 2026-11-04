# XZ Embedded v2024-12-30 (vendored)

From https://github.com/tukaani-project/xz-embedded, tag `v2024-12-30`
(commit `ae63ae3a36ed01724674e8f3d750dc47bf125410`), by Lasse Collin. 0BSD licence, see
`LICENSE.txt`.

Files kept, unchanged, flattened into this directory: `linux/include/linux/xz.h`,
`userspace/xz_config.h`, and from `linux/lib/xz/`: `xz_crc32.c`, `xz_dec_stream.c`,
`xz_dec_lzma2.c`, `xz_lzma2.h`, `xz_private.h`, `xz_stream.h`. LZMA2 with a CRC32 check
only (no BCJ filters, no CRC64/SHA-256): what Ventoy's `core.img.xz` and
`ventoy.disk.img.xz` use. Kiln unpacks those two with it (`src/ventoy.cpp`).
