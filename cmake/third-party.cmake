# The third-party source archives Kiln builds from, pinned by SHA-256. Read by
# CMakeLists.txt and by cmake/fetch-third-party.cmake (which downloads them
# for an offline package build). Packaging that lists them itself
# (packaging/arch/PKGBUILD) must match these.
set(KILN_CDIO_VERSION 2.4.0)
set(KILN_CDIO_URL https://github.com/libcdio/libcdio-C/releases/download/2.4.0/libcdio-2.4.0.tar.gz)
set(KILN_CDIO_SHA256 bf7cde63762bb12db7755c395c441e49406fde7e1d9f9a9be7e3b940b1f405d7)

set(KILN_WIMLIB_URL https://wimlib.net/downloads/wimlib-1.14.5.tar.gz)
set(KILN_WIMLIB_SHA256 84221a3abd5b91228f15f8e6065c335a336237b5738197b75bf419eea561a194)

set(KILN_HIVEX_URL https://download.libguestfs.org/hivex/hivex-1.3.24.tar.gz)
set(KILN_HIVEX_SHA256 a52fa45cecc9a78adb2d28605d68261e4f1fd4514a778a5473013d2ccc8a193c)

set(KILN_VENTOY_VERSION 1.1.18)
set(KILN_VENTOY_URL https://github.com/ventoy/Ventoy/releases/download/v1.1.18/ventoy-1.1.18-linux.tar.gz)
set(KILN_VENTOY_SHA256 d86ff9de63d94c8b8f1f6b14d23c55af4b88e8207bf2893890892a6a97b1ad54)

# GNU libiconv: Windows only (glibc and macOS have iconv built in).
set(KILN_ICONV_URL https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.19.tar.gz)
set(KILN_ICONV_SHA256 88dd96a8c0464eca144fc791ae60cd31cd8ee78321e67397e25fc095c4a19aa6)

set(KILN_THIRD_PARTY CDIO WIMLIB HIVEX VENTOY ICONV)

# Not used by the build: the source of the Ventoy files built into Kiln (the
# release archive above has only binaries). Attached to each release with the
# other source archives (fetch-third-party.cmake -DKILN_WITH_SOURCES=ON).
set(KILN_VENTOY_SOURCE_URL https://github.com/ventoy/Ventoy/archive/refs/tags/v1.1.18.tar.gz)
set(KILN_VENTOY_SOURCE_NAME Ventoy-1.1.18-source.tar.gz)
set(KILN_VENTOY_SOURCE_SHA256 083ea5b7b9c0ca30c55fbfa21d8cdc7cee5c2dc3b034b4e65a664a9aa22d29bb)
set(KILN_SOURCES_ONLY VENTOY_SOURCE)
