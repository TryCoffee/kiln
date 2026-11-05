// version_probe.cpp — release version parsing and ordering (the GUI's update check).
#include "check.hpp"
#include "version_compare.hpp"

#include <string>

using namespace kiln;
using probe::check;

int main() {
    const auto v = [](const char* s) { return *parse_version(s); };
    check(parse_version("v0.1.1") == std::vector<unsigned>{0, 1, 1}, "v0.1.1 parses");
    check(parse_version("0.2") == std::vector<unsigned>{0, 2}, "two parts parse");
    check(parse_version("1.2.3.4").has_value(), "four parts parse");
    for (const char* bad : {"", "v", "1", "1.", ".1", "1..2", "0.2.0-beta", "0.2.0 ", "x1.2", "1.2.3.4.5", "123456.1", "1.-2"})
        check(!parse_version(bad), std::string("refused: '") + bad + "'");
    check(version_newer(v("0.2.0"), v("0.1.1")), "0.2.0 is newer than 0.1.1");
    check(version_newer(v("0.10.0"), v("0.9.9")), "0.10.0 is newer than 0.9.9 (numbers, not text)");
    check(version_newer(v("1.0"), v("0.99.99")), "1.0 is newer than 0.99.99");
    check(version_newer(v("0.1.1.1"), v("0.1.1")), "an extra part counts");
    check(!version_newer(v("0.1.1"), v("0.1.1")), "the same version is not newer");
    check(!version_newer(v("0.2"), v("0.2.0")), "0.2 and 0.2.0 are the same");
    check(!version_newer(v("0.1.0"), v("0.1.1")), "an older version is not newer");
    check(parse_version(KILN_VERSION).has_value(), std::string("this build's version parses: ") + KILN_VERSION);
    return probe::finish("version_probe");
}
