# wimlib 1.14.5 + Kiln patch

Downloaded and built at configure time by CMake (`ExternalProject`), from
https://wimlib.net/downloads/wimlib-1.14.5.tar.gz
(SHA-256 `84221a3abd5b91228f15f8e6065c335a336237b5738197b75bf419eea561a194`),
configured `--disable-shared --without-fuse --without-ntfs-3g` (so libwim may be
used under LGPL-2.1+ or GPL-3.0+; Kiln is GPL-3.0+ either way).

`kiln-read-callback.patch` adds `wimlib_open_wim_from_reader()`: open a WIM
whose bytes come from a callback, read-only. Kiln uses it to split
`sources/install.wim` straight out of a Windows ISO (read through libcdio)
without extracting it first. Rufus solves the same problem by teaching its
wimlib copy to open "image.iso|sources/install.wim" through libcdio; the
callback keeps libcdio out of wimlib.
