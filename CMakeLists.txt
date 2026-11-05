cmake_minimum_required(VERSION 3.25)
project(kiln VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(KILN_LINUX ON)
elseif(APPLE)
    set(KILN_MACOS ON)
elseif(WIN32 AND MINGW)
    set(KILN_WINDOWS ON)  # MSYS2 UCRT64: GCC, and its sh/make for the third-party configure scripts
    enable_language(RC)
    find_program(KILN_SH sh REQUIRED)
else()
    message(FATAL_ERROR "Kiln builds on Linux, macOS and Windows (MSYS2 MinGW)")
endif()
# A configure script: run directly, out of tree, on Linux/macOS. On Windows CMake
# cannot execute a shell script, so MSYS2's sh runs it, in the source tree:
# given an out-of-tree C:/ path, autoconf cannot find its auxiliary files.
set(KILN_CONFIGURE <SOURCE_DIR>/configure)
set(KILN_IN_SOURCE FALSE)
if(KILN_WINDOWS)
    # CMake's own archiver too, so the configure scripts never mix in another
    # toolchain's binutils that happen to be on PATH.
    set(KILN_CONFIGURE ${KILN_SH} ./configure AR=${CMAKE_AR} RANLIB=${CMAKE_RANLIB})
    set(KILN_IN_SOURCE TRUE)
endif()

find_package(Threads REQUIRED)
include(ExternalProject)
include(GNUInstallDirs)
include(cmake/third-party.cmake)

# Where the pinned third-party tarballs (libcdio, wimlib, hivex, Ventoy) are
# looked for and downloaded to. A package build puts them here beforehand and
# builds offline: a file whose SHA-256 matches is never downloaded again.
set(KILN_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads" CACHE PATH "third-party tarballs")
include(ProcessorCount)
ProcessorCount(NPROC)

# --- third party ----------------------------------------------------------

# libcdio (GPL-3.0+): UDF reading. 2.4.0 built here, library only, on every
# platform: the version Kiln's multi-extent install.wim reading was proven on
# (Debian 13 and MSYS2 ship older ones). KILN_SYSTEM_LIBCDIO=ON uses the
# system's instead (Linux).
# iconv (libcdio and hivex convert names with it): glibc and macOS have it; on
# Windows, GNU libiconv built here, static (LGPL-2.1+).
add_library(kiln_iconv INTERFACE)
set(KILN_ICONV_FLAGS)
if(KILN_MACOS)
    target_link_libraries(kiln_iconv INTERFACE iconv)
elseif(KILN_WINDOWS)
    set(ICONV_PREFIX ${CMAKE_BINARY_DIR}/iconv)
    ExternalProject_Add(iconv_ext
        URL ${KILN_ICONV_URL}
        URL_HASH SHA256=${KILN_ICONV_SHA256}
        DOWNLOAD_DIR ${KILN_DOWNLOAD_DIR}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        BUILD_IN_SOURCE ${KILN_IN_SOURCE}
        CONFIGURE_COMMAND ${KILN_CONFIGURE} --prefix=<INSTALL_DIR> --disable-shared --enable-static
                          --disable-nls --disable-dependency-tracking CC=${CMAKE_C_COMPILER}
        # the library and its header only (the top level would also build iconv.exe, man pages...)
        BUILD_COMMAND make lib/localcharset.h && make -j${NPROC} -C libcharset && make -j${NPROC} -C lib
        INSTALL_COMMAND make -C libcharset install && make -C lib install
                        && ${CMAKE_COMMAND} -E copy include/iconv.h.inst <INSTALL_DIR>/include/iconv.h
        INSTALL_DIR ${ICONV_PREFIX}
        BUILD_BYPRODUCTS ${ICONV_PREFIX}/lib/libiconv.a ${ICONV_PREFIX}/lib/libcharset.a
    )
    file(MAKE_DIRECTORY ${ICONV_PREFIX}/include)
    target_include_directories(kiln_iconv SYSTEM INTERFACE ${ICONV_PREFIX}/include)
    target_link_libraries(kiln_iconv INTERFACE ${ICONV_PREFIX}/lib/libiconv.a ${ICONV_PREFIX}/lib/libcharset.a)
    add_dependencies(kiln_iconv iconv_ext)
    # what libcdio's and hivex's configure scripts need to find it
    set(KILN_ICONV_FLAGS --with-libiconv-prefix=${ICONV_PREFIX}
        "CPPFLAGS=-I${ICONV_PREFIX}/include" "LDFLAGS=-L${ICONV_PREFIX}/lib")
endif()

option(KILN_SYSTEM_LIBCDIO "Use the system's libcdio (Linux)" OFF)
if(KILN_LINUX AND KILN_SYSTEM_LIBCDIO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CDIO REQUIRED IMPORTED_TARGET libudf libcdio)
    add_library(cdio ALIAS PkgConfig::CDIO)
else()
    set(CDIO_PREFIX ${CMAKE_BINARY_DIR}/cdio)
    ExternalProject_Add(cdio_ext
        URL ${KILN_CDIO_URL}
        URL_HASH SHA256=${KILN_CDIO_SHA256}
        DOWNLOAD_DIR ${KILN_DOWNLOAD_DIR}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        BUILD_IN_SOURCE ${KILN_IN_SOURCE}
        CONFIGURE_COMMAND ${KILN_CONFIGURE} --prefix=<INSTALL_DIR> --disable-shared --enable-static
                          --disable-cxx --disable-example-progs --without-cd-drive --without-cd-info
                          --without-cdda-player --without-cd-read --without-iso-info --without-iso-read
                          --disable-dependency-tracking CC=${CMAKE_C_COMPILER} ${KILN_ICONV_FLAGS}
        BUILD_COMMAND make -j${NPROC} -C lib
        INSTALL_COMMAND make -C lib install && make -C include install
        INSTALL_DIR ${CDIO_PREFIX}
        BUILD_BYPRODUCTS ${CDIO_PREFIX}/lib/libudf.a ${CDIO_PREFIX}/lib/libiso9660.a ${CDIO_PREFIX}/lib/libcdio.a
    )
    file(MAKE_DIRECTORY ${CDIO_PREFIX}/include)
    add_library(cdio INTERFACE)
    target_include_directories(cdio SYSTEM INTERFACE ${CDIO_PREFIX}/include)
    target_link_libraries(cdio INTERFACE ${CDIO_PREFIX}/lib/libudf.a ${CDIO_PREFIX}/lib/libiso9660.a
                          ${CDIO_PREFIX}/lib/libcdio.a)
    target_link_libraries(cdio INTERFACE kiln_iconv)
    if(KILN_MACOS)
        target_link_libraries(cdio INTERFACE "-framework IOKit" "-framework CoreFoundation")
    elseif(KILN_WINDOWS)
        target_link_libraries(cdio INTERFACE winmm)
        add_dependencies(cdio_ext iconv_ext)
    endif()
    add_dependencies(cdio cdio_ext)
endif()

# FatFs (vendored, BSD-1-clause): FAT32 written straight onto the drive.
add_library(fatfs STATIC third_party/fatfs/ff.c third_party/fatfs/ffunicode.c)
target_include_directories(fatfs PUBLIC third_party/fatfs)
target_compile_options(fatfs PRIVATE -w)  # upstream code, kept unmodified

# wimlib 1.14.5 + Kiln's read-callback patch, static, no FUSE/NTFS-3G.
set(WIMLIB_PREFIX ${CMAKE_BINARY_DIR}/wimlib)
ExternalProject_Add(wimlib_ext
    URL ${KILN_WIMLIB_URL}
    URL_HASH SHA256=${KILN_WIMLIB_SHA256}
    DOWNLOAD_DIR ${KILN_DOWNLOAD_DIR}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE  # tarball times, or make wants to rerun automake
    BUILD_IN_SOURCE ${KILN_IN_SOURCE}
    PATCH_COMMAND patch -p1 --forward --no-backup-if-mismatch
                  -i ${CMAKE_SOURCE_DIR}/third_party/wimlib/kiln-read-callback.patch
    # full_read/full_write are also gnulib's (inside hivex): rename wimlib's internal ones
    CONFIGURE_COMMAND ${KILN_CONFIGURE} --prefix=<INSTALL_DIR> --disable-shared --enable-static
                      --without-fuse --without-ntfs-3g --disable-dependency-tracking
                      CC=${CMAKE_C_COMPILER} "CPPFLAGS=-Dfull_read=wimlib_full_read -Dfull_write=wimlib_full_write"
    BUILD_COMMAND make -j${NPROC}
    INSTALL_COMMAND make install
    INSTALL_DIR ${WIMLIB_PREFIX}
    BUILD_BYPRODUCTS ${WIMLIB_PREFIX}/lib/libwim.a
)
file(MAKE_DIRECTORY ${WIMLIB_PREFIX}/include)
add_library(wimlib STATIC IMPORTED)
set_target_properties(wimlib PROPERTIES
    IMPORTED_LOCATION ${WIMLIB_PREFIX}/lib/libwim.a
    INTERFACE_INCLUDE_DIRECTORIES ${WIMLIB_PREFIX}/include)
add_dependencies(wimlib wimlib_ext)
if(KILN_WINDOWS)
    # wimlib's Windows code calls the native API (ntdll) and the security functions.
    set_target_properties(wimlib PROPERTIES INTERFACE_COMPILE_DEFINITIONS WIMLIB_STATIC
                          INTERFACE_LINK_LIBRARIES "ntdll;advapi32")
endif()

# hivex 1.3.24 (LGPL-2.1+): the library only, static, to edit boot.wim's registry hive.
set(HIVEX_PREFIX ${CMAKE_BINARY_DIR}/hivex)
ExternalProject_Add(hivex_ext
    URL ${KILN_HIVEX_URL}
    URL_HASH SHA256=${KILN_HIVEX_SHA256}
    DOWNLOAD_DIR ${KILN_DOWNLOAD_DIR}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    BUILD_IN_SOURCE ${KILN_IN_SOURCE}
    CONFIGURE_COMMAND ${KILN_CONFIGURE} --prefix=<INSTALL_DIR> --disable-shared --enable-static
                      --disable-ocaml --disable-perl --disable-python --disable-ruby --disable-nls
                      --disable-dependency-tracking CC=${CMAKE_C_COMPILER} ${KILN_ICONV_FLAGS}
                      # only the library is built: its man page and HTML (pod2man, pod2html) never are
                      POD2MAN=false POD2TEXT=false
    BUILD_COMMAND make -j${NPROC} -C gnulib/lib && make -j${NPROC} -C lib libhivex.la
    INSTALL_COMMAND make -C lib install-libLTLIBRARIES && make -C include install
    INSTALL_DIR ${HIVEX_PREFIX}
    BUILD_BYPRODUCTS ${HIVEX_PREFIX}/lib/libhivex.a
)
file(MAKE_DIRECTORY ${HIVEX_PREFIX}/include)
add_library(hivex STATIC IMPORTED)
set_target_properties(hivex PROPERTIES
    IMPORTED_LOCATION ${HIVEX_PREFIX}/lib/libhivex.a
    INTERFACE_INCLUDE_DIRECTORIES ${HIVEX_PREFIX}/include)
add_dependencies(hivex hivex_ext)
set_target_properties(hivex PROPERTIES INTERFACE_LINK_LIBRARIES kiln_iconv)
if(KILN_WINDOWS)
    add_dependencies(hivex_ext iconv_ext)
endif()

# XZ Embedded (vendored, 0BSD): unpacks Ventoy's core.img.xz and ventoy.disk.img.xz.
add_library(xzembedded STATIC third_party/xz-embedded/xz_crc32.c third_party/xz-embedded/xz_dec_stream.c
            third_party/xz-embedded/xz_dec_lzma2.c)
target_include_directories(xzembedded PUBLIC third_party/xz-embedded)
target_compile_options(xzembedded PRIVATE -w)  # upstream code, kept unmodified

# Ventoy (GPL-3.0+): its boot code, BIOS stage and EFI partition image, taken
# from the official Linux release (archive pinned by SHA-256) and linked into
# the programs that write Ventoy disks (src/ventoy_write.cpp). Offline builds:
# -DKILN_VENTOY_ARCHIVE=/path/to/ventoy-<version>-linux.tar.gz.
set(KILN_VENTOY_ARCHIVE "" CACHE FILEPATH "ventoy-${KILN_VENTOY_VERSION}-linux.tar.gz, already downloaded")
set(VENTOY_DIR ${CMAKE_BINARY_DIR}/ventoy)
set(VENTOY_ARCHIVE ${KILN_VENTOY_ARCHIVE})
if(NOT VENTOY_ARCHIVE)
    set(VENTOY_ARCHIVE ${KILN_DOWNLOAD_DIR}/ventoy-${KILN_VENTOY_VERSION}-linux.tar.gz)
    if(EXISTS ${VENTOY_ARCHIVE})
        file(SHA256 ${VENTOY_ARCHIVE} VENTOY_GOT)
    endif()
    if(NOT VENTOY_GOT STREQUAL KILN_VENTOY_SHA256)
        message(STATUS "Downloading Ventoy ${KILN_VENTOY_VERSION}")
        file(DOWNLOAD ${KILN_VENTOY_URL} ${VENTOY_ARCHIVE} EXPECTED_HASH SHA256=${KILN_VENTOY_SHA256} TLS_VERIFY ON STATUS VENTOY_STATUS)
        list(GET VENTOY_STATUS 0 VENTOY_RC)
        if(NOT VENTOY_RC EQUAL 0)
            message(FATAL_ERROR "Downloading Ventoy failed: ${VENTOY_STATUS}")
        endif()
    endif()
endif()
file(SHA256 ${VENTOY_ARCHIVE} VENTOY_GOT)
if(NOT VENTOY_GOT STREQUAL KILN_VENTOY_SHA256)
    message(FATAL_ERROR "${VENTOY_ARCHIVE}: SHA-256 ${VENTOY_GOT}, expected ${KILN_VENTOY_SHA256}")
endif()
set(VENTOY_FILES ${VENTOY_DIR}/files)
set(VENTOY_ROOT ${VENTOY_FILES}/ventoy-${KILN_VENTOY_VERSION})
set(VENTOY_STAMP ${VENTOY_FILES}/extracted-from-sha256)
if(EXISTS ${VENTOY_STAMP})
    file(READ ${VENTOY_STAMP} VENTOY_EXTRACTED)
endif()
if(NOT VENTOY_EXTRACTED STREQUAL KILN_VENTOY_SHA256)  # (the files keep the archive's dates)
    file(REMOVE_RECURSE ${VENTOY_FILES})
    file(ARCHIVE_EXTRACT INPUT ${VENTOY_ARCHIVE} DESTINATION ${VENTOY_FILES}
         PATTERNS */boot/boot.img */boot/core.img.xz */ventoy/ventoy.disk.img.xz */ventoy/version)
    file(WRITE ${VENTOY_STAMP} ${KILN_VENTOY_SHA256})
endif()
file(READ ${VENTOY_ROOT}/ventoy/version VENTOY_FILE_VERSION)
string(STRIP "${VENTOY_FILE_VERSION}" VENTOY_FILE_VERSION)
if(NOT VENTOY_FILE_VERSION STREQUAL KILN_VENTOY_VERSION)
    message(FATAL_ERROR "the Ventoy archive says version ${VENTOY_FILE_VERSION}, expected ${KILN_VENTOY_VERSION}")
endif()
set(VENTOY_BLOBS ${VENTOY_ROOT}/boot/boot.img ${VENTOY_ROOT}/boot/core.img.xz ${VENTOY_ROOT}/ventoy/ventoy.disk.img.xz)
set(VENTOY_BLOBS_C ${CMAKE_BINARY_DIR}/ventoy_blobs.c)
set(VENTOY_BLOBS_SRC "/* Generated by CMakeLists.txt: Ventoy ${KILN_VENTOY_VERSION}'s files (GPL-3.0+). */\n#include <stddef.h>\n")
foreach(blob boot_img:boot/boot.img core_img_xz:boot/core.img.xz disk_img_xz:ventoy/ventoy.disk.img.xz)
    string(REPLACE ":" ";" blob ${blob})
    list(GET blob 0 sym)
    list(GET blob 1 rel)
    string(APPEND VENTOY_BLOBS_SRC "const unsigned char kiln_ventoy_${sym}[] = {\n#embed \"${VENTOY_ROOT}/${rel}\"\n};\n"
                                   "const size_t kiln_ventoy_${sym}_size = sizeof kiln_ventoy_${sym};\n")
endforeach()
file(CONFIGURE OUTPUT ${VENTOY_BLOBS_C} CONTENT "${VENTOY_BLOBS_SRC}" @ONLY)
add_library(ventoyblobs STATIC ${VENTOY_BLOBS_C})
set_target_properties(ventoyblobs PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)  # #embed
set_source_files_properties(${VENTOY_BLOBS_C} PROPERTIES OBJECT_DEPENDS "${VENTOY_BLOBS}")

# --- Kiln -----------------------------------------------------------------

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
                    $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic> $<$<COMPILE_LANGUAGE:CXX>:-Wshadow>
                    $<$<COMPILE_LANGUAGE:CXX>:-Wconversion> $<$<COMPILE_LANGUAGE:CXX>:-Wno-sign-conversion>)

# Everything that is not the command line lives in the library, so the GUI and
# the privileged helper (later phases) link the same code the CLI tests.
add_library(kilncore STATIC
    src/device_common.cpp
    src/fat.cpp
    src/gpt.cpp
    src/image.cpp
    src/job.cpp
    src/sha256.cpp
    src/helper_protocol.cpp
    src/udf.cpp
    src/unattend.cpp
    src/ventoy.cpp
    src/ventoy_write.cpp
    src/windows.cpp
    src/writer.cpp
)
if(KILN_LINUX)
    target_sources(kilncore PRIVATE src/blockio.cpp src/device_linux.cpp)
elseif(KILN_MACOS)
    target_sources(kilncore PRIVATE src/blockio.cpp src/device_macos.cpp src/authopen_macos.cpp)
    target_link_libraries(kilncore PRIVATE "-framework DiskArbitration" "-framework IOKit" "-framework CoreFoundation")
else()
    target_sources(kilncore PRIVATE src/blockio_windows.cpp src/device_windows.cpp src/win32_util.cpp)
    # Windows 10 1903+: the manifest's UTF-8 code page and the newer storage APIs.
    # No UNICODE: Kiln calls the ...W functions by name, and FatFs has its own TCHAR (char).
    # _FILE_OFFSET_BITS: MinGW's off_t is 32 bits unless asked, while libcdio, hivex and
    # wimlib are all built with 64: without it every position past 2 GiB in an ISO is
    # cut short, and libcdio's off_t arguments do not even match (seen: "The XML
    # data of the WIM is invalid", install.wim's XML being at its 7 GB end).
    target_compile_definitions(kilncore PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00 _FILE_OFFSET_BITS=64)
endif()
target_include_directories(kilncore PUBLIC src)
target_link_libraries(kilncore PRIVATE fatfs wimlib hivex cdio xzembedded ventoyblobs Threads::Threads)
target_compile_definitions(kilncore PUBLIC KILN_VERSION="${PROJECT_VERSION}")
set_source_files_properties(src/ventoy.cpp PROPERTIES COMPILE_DEFINITIONS KILN_VENTOY_VERSION="${KILN_VENTOY_VERSION}")

# Windows: every executable carries the manifest that makes UTF-8 its code page
# (argv, fopen and std::filesystem then agree with Kiln's UTF-8 strings;
# without it libstdc++ throws on a Polish file name).
if(KILN_WINDOWS)
    configure_file(data/kiln.rc.in ${CMAKE_BINARY_DIR}/kiln.rc @ONLY)
endif()
function(kiln_executable target)
    if(KILN_WINDOWS)
        target_sources(${target} PRIVATE ${CMAKE_BINARY_DIR}/kiln.rc)
    endif()
endfunction()

add_executable(kiln src/cli/main.cpp)
target_link_libraries(kiln PRIVATE kilncore)
kiln_executable(kiln)

set(KILN_LIBEXEC_DIR ${CMAKE_INSTALL_FULL_LIBEXECDIR}/kiln)

# The privileged writer behind the GUI (Linux: pkexec; Windows: UAC). Kept
# separate and small: the only Kiln code that ever runs privileged for the GUI.
# macOS has none: Kiln runs as the user and gets the drive from authopen.
if(KILN_LINUX)
    add_executable(kiln-helper src/helper/main.cpp)
elseif(KILN_WINDOWS)
    add_executable(kiln-helper src/helper/main_windows.cpp)
endif()
if(TARGET kiln-helper)
    target_link_libraries(kiln-helper PRIVATE kilncore)
    kiln_executable(kiln-helper)
endif()

option(KILN_GUI "Build the Qt6 GUI" ON)
if(KILN_GUI)
    set(KILN_QT_COMPONENTS Widgets Concurrent)
    if(KILN_WINDOWS)
        list(APPEND KILN_QT_COMPONENTS Network)  # QLocalServer: the helper's pipe
    endif()
    find_package(Qt6 6.4 COMPONENTS ${KILN_QT_COMPONENTS})
    if(Qt6_FOUND)
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        add_executable(kiln-gui src/gui/main.cpp src/gui/mainwindow.cpp src/gui/mainwindow.hpp data/icons/icons.qrc)
        target_link_libraries(kiln-gui PRIVATE kilncore Qt6::Widgets Qt6::Concurrent)
        kiln_executable(kiln-gui)
        if(KILN_WINDOWS)
            target_link_libraries(kiln-gui PRIVATE Qt6::Network)
            set_target_properties(kiln-gui PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
        target_compile_definitions(kiln-gui PRIVATE KILN_LIBEXEC_DIR="${KILN_LIBEXEC_DIR}"
                                   QT_NO_CAST_FROM_ASCII QT_NO_URL_CAST_FROM_STRING)
        if(KILN_MACOS)
            # Kiln.app: the icon (an .icns made from the PNGs by iconutil) and Kiln's own Info.plist.
            set(KILN_ICONSET ${CMAKE_BINARY_DIR}/kiln.iconset)
            set(KILN_ICNS ${CMAKE_BINARY_DIR}/kiln.icns)
            set(icon_copies)
            foreach(pair 16:16x16 32:16x16@2x 32:32x32 64:32x32@2x 128:128x128 256:128x128@2x
                         256:256x256 512:256x256@2x 512:512x512 1024:512x512@2x)
                string(REPLACE ":" ";" pair ${pair})
                list(GET pair 0 px)
                list(GET pair 1 name)
                list(APPEND icon_copies COMMAND ${CMAKE_COMMAND} -E copy
                     ${CMAKE_SOURCE_DIR}/data/icons/kiln-${px}.png ${KILN_ICONSET}/icon_${name}.png)
            endforeach()
            add_custom_command(OUTPUT ${KILN_ICNS}
                COMMAND ${CMAKE_COMMAND} -E rm -rf ${KILN_ICONSET}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${KILN_ICONSET}
                ${icon_copies}
                COMMAND iconutil -c icns -o ${KILN_ICNS} ${KILN_ICONSET}
                DEPENDS data/icons/kiln-16.png data/icons/kiln-32.png data/icons/kiln-64.png data/icons/kiln-128.png
                        data/icons/kiln-256.png data/icons/kiln-512.png data/icons/kiln-1024.png
                COMMENT "Making kiln.icns")
            target_sources(kiln-gui PRIVATE ${KILN_ICNS})
            set_source_files_properties(${KILN_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
            set(KILN_MACOS_MINIMUM ${CMAKE_OSX_DEPLOYMENT_TARGET})
            if(NOT KILN_MACOS_MINIMUM)
                set(KILN_MACOS_MINIMUM 14.4)  # the official Qt 6.12's own minimum
            endif()
            set_target_properties(kiln-gui PROPERTIES MACOSX_BUNDLE TRUE OUTPUT_NAME Kiln
                                  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/data/Info.plist.in
                                  MACOSX_BUNDLE_BUNDLE_NAME Kiln
                                  MACOSX_BUNDLE_GUI_IDENTIFIER io.github.trycoffee.kiln
                                  MACOSX_BUNDLE_ICON_FILE kiln.icns
                                  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
                                  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
                                  # the app's own Frameworks/, and (until macdeployqt has copied Qt
                                  # in and packaging/macos/make-dmg.sh drops it) Qt's lib/
                                  INSTALL_RPATH "@executable_path/../Frameworks"
                                  INSTALL_RPATH_USE_LINK_PATH ON)
        endif()
        install(TARGETS kiln-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
    else()
        message(STATUS "Qt6 Widgets not found: building without the GUI")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    set(probes image_probe writer_probe gpt_probe fat_probe unattend_probe ventoy_probe)
    if(KILN_LINUX)
        list(APPEND probes device_probe helper_probe)  # a fake sysfs; the pkexec helper
    elseif(KILN_MACOS)
        list(APPEND probes mac_device_probe)  # needs root: skipped under a plain ctest
    else()
        list(APPEND probes helper_probe win_device_probe)  # the UAC helper; a VHD (needs an administrator)
    endif()
    foreach(probe ${probes})
        add_executable(${probe} tests/${probe}.cpp)
        target_link_libraries(${probe} PRIVATE kilncore)
        kiln_executable(${probe})
        add_test(NAME ${probe} COMMAND ${probe})
        # 77 = an independent checker (sfdisk, fsck.fat, mtools, ...) is missing: shown as skipped, not passed
        set_tests_properties(${probe} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
    target_compile_definitions(ventoy_probe PRIVATE KILN_VENTOY_FILES="${VENTOY_ROOT}"
                               KILN_VENTOY_VERSION_EXPECTED="${KILN_VENTOY_VERSION}")
    if(TARGET helper_probe)
        target_compile_definitions(helper_probe PRIVATE KILN_HELPER_PATH="$<TARGET_FILE:kiln-helper>")
        add_dependencies(helper_probe kiln-helper)
    endif()
    # wimlib and hivex's gnulib both define full_read/full_write; a static link would
    # silently send hivex's calls into wimlib (seen: segfault in hivex_open). wimlib
    # is built with them renamed; this keeps it that way.
    add_test(NAME no_symbol_clash
             COMMAND sh -c "! nm -g --defined-only '${WIMLIB_PREFIX}/lib/libwim.a' | grep -E ' _?full_(read|write)$'")
endif()

if(KILN_MACOS AND TARGET kiln-gui)
    # The command line inside the app (macOS: "kiln" and "Kiln" would be one name in
    # Contents/MacOS on a case-insensitive disk): Kiln.app/Contents/Helpers/kiln.
    install(TARGETS kiln RUNTIME DESTINATION Kiln.app/Contents/Helpers)
else()
    install(TARGETS kiln RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(KILN_WINDOWS)
    install(TARGETS kiln-helper RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})  # the GUI looks beside itself
endif()
if(KILN_LINUX)
    install(TARGETS kiln-helper RUNTIME DESTINATION ${KILN_LIBEXEC_DIR})
    configure_file(data/io.github.trycoffee.kiln.policy.in io.github.trycoffee.kiln.policy @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/io.github.trycoffee.kiln.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
    install(FILES data/kiln.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/icons/io.github.trycoffee.kiln.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES data/icons/kiln-${size}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
                RENAME io.github.trycoffee.kiln.png)
    endforeach()
endif()

# Licence texts: Kiln's own and those of everything built into it.
if(KILN_LINUX)
    set(KILN_LICENSE_DIR ${CMAKE_INSTALL_DATADIR}/licenses/kiln CACHE STRING "where the licence texts go")
elseif(KILN_MACOS AND TARGET kiln-gui)
    set(KILN_LICENSE_DIR Kiln.app/Contents/Resources/licenses CACHE STRING "where the licence texts go")
else()
    set(KILN_LICENSE_DIR licenses CACHE STRING "where the licence texts go")
endif()
install(FILES LICENSE THIRD-PARTY-NOTICES.md DESTINATION ${KILN_LICENSE_DIR})
install(FILES third_party/fatfs/LICENSE.txt DESTINATION ${KILN_LICENSE_DIR}/fatfs)
install(FILES third_party/xz-embedded/LICENSE.txt DESTINATION ${KILN_LICENSE_DIR}/xz-embedded)
install(DIRECTORY third_party/ventoy/License/ DESTINATION ${KILN_LICENSE_DIR}/ventoy)
ExternalProject_Get_Property(wimlib_ext SOURCE_DIR)
install(FILES ${SOURCE_DIR}/COPYING ${SOURCE_DIR}/COPYING.GPLv3 ${SOURCE_DIR}/COPYING.LGPL
        DESTINATION ${KILN_LICENSE_DIR}/wimlib)
ExternalProject_Get_Property(hivex_ext SOURCE_DIR)
install(FILES ${SOURCE_DIR}/LICENSE DESTINATION ${KILN_LICENSE_DIR}/hivex)
if(TARGET cdio_ext)
    ExternalProject_Get_Property(cdio_ext SOURCE_DIR)
    install(FILES ${SOURCE_DIR}/COPYING DESTINATION ${KILN_LICENSE_DIR}/libcdio)
endif()
if(TARGET iconv_ext)
    ExternalProject_Get_Property(iconv_ext SOURCE_DIR)
    install(FILES ${SOURCE_DIR}/COPYING.LIB DESTINATION ${KILN_LICENSE_DIR}/libiconv)
endif()
option(KILN_DEBIAN_COPYRIGHT "Also write every licence into one Debian copyright file" OFF)
if(KILN_DEBIAN_COPYRIGHT)  # last: it reads the licence texts installed above
    install(CODE "set(KILN_LICENSE_ROOT \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${KILN_LICENSE_DIR}\")
                  include(\"${CMAKE_SOURCE_DIR}/cmake/debian-copyright.cmake\")")
endif()

# Packages: cpack -G DEB on Debian/Ubuntu, cpack -G RPM on Fedora (see packaging/linux/).
set(CPACK_PACKAGE_NAME kiln)
set(CPACK_PACKAGE_VENDOR "Kiln")
set(CPACK_PACKAGE_CONTACT "TryCoffee <108859537+TryCoffee@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/TryCoffee/kiln")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "USB flasher: verified image writes, Windows install sticks, Ventoy disks")
set(CPACK_PACKAGE_DESCRIPTION "Kiln writes bootable images to USB drives and SD cards and verifies them,
builds Windows install sticks from a Windows ISO (with optional Rufus-style
setup tweaks) and makes Ventoy disks. Only removable drives are offered.")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION admin)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "pkexec")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "polkitd")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_REQUIRES "polkit")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/libexec /usr/share/applications /usr/share/licenses /usr/share/polkit-1 /usr/share/polkit-1/actions
    /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/scalable /usr/share/icons/hicolor/scalable/apps
    /usr/share/icons/hicolor/16x16 /usr/share/icons/hicolor/16x16/apps /usr/share/icons/hicolor/24x24 /usr/share/icons/hicolor/24x24/apps
    /usr/share/icons/hicolor/32x32 /usr/share/icons/hicolor/32x32/apps /usr/share/icons/hicolor/48x48 /usr/share/icons/hicolor/48x48/apps
    /usr/share/icons/hicolor/64x64 /usr/share/icons/hicolor/64x64/apps /usr/share/icons/hicolor/128x128 /usr/share/icons/hicolor/128x128/apps
    /usr/share/icons/hicolor/256x256 /usr/share/icons/hicolor/256x256/apps /usr/share/icons/hicolor/512x512 /usr/share/icons/hicolor/512x512/apps)
include(CPack)
