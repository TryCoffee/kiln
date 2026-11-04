# Run at install time (install(CODE), see CMakeLists.txt) when KILN_DEBIAN_COPYRIGHT
# is on: writes KILN_LICENSE_ROOT/copyright, the notices and every licence text
# in one file. Debian's convention, and the one file in /usr/share/doc that
# minimised Ubuntu and Debian systems keep (dpkg path-exclude /usr/share/doc/*,
# path-include */copyright).
file(GLOB_RECURSE texts RELATIVE ${KILN_LICENSE_ROOT} ${KILN_LICENSE_ROOT}/*)
list(REMOVE_ITEM texts copyright THIRD-PARTY-NOTICES.md)
list(SORT texts)
file(READ ${KILN_LICENSE_ROOT}/THIRD-PARTY-NOTICES.md out)
string(PREPEND out "Kiln: https://github.com/TryCoffee/kiln\nKiln is licensed under the GNU GPL v3 or later (LICENSE below).\n\n")
foreach(t ${texts})
    file(READ ${KILN_LICENSE_ROOT}/${t} body)
    string(APPEND out "\n\n======== ${t} ========\n\n${body}")
endforeach()
file(WRITE ${KILN_LICENSE_ROOT}/copyright "${out}")
message(STATUS "Installing: ${KILN_LICENSE_ROOT}/copyright")
