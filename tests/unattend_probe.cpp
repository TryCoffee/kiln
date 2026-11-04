// unattend_probe.cpp — the answer file: options -> XML, account-name rules, and
// well-formedness checked by xmllint (libxml2). Without xmllint: exit 77 (skipped).
#include "check.hpp"
#include "unattend.hpp"

#include <cstdlib>
#include <fstream>

using namespace kiln;
using probe::check;

namespace {

size_t count(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
    return n;
}

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

}  // namespace

int main() {
    Customization none;
    check(!none.any() && !none.needs_answer_file(), "default: nothing to do");
    Customization only_bypass;
    only_bypass.bypass_checks = true;
    check(only_bypass.any() && !only_bypass.needs_answer_file(), "bypass alone needs no answer file (it is a hive edit)");

    check(sanitize_account_name("John Doe") == "John Doe", "a plain name is kept");
    check(sanitize_account_name("  kiln  ") == "kiln", "blanks trimmed");
    check(sanitize_account_name("a/b:c<d>&\"e") == "a_b_c_d___e", "Rufus's invalid characters become _");
    std::string why;
    check(sanitize_account_name("administrator", &why).empty() && contains(why, "reserved"), "Administrator refused (any case)");
    check(sanitize_account_name("Järjestelmänvalvoja").empty(), "localised Administrator refused");
    check(sanitize_account_name("   ", &why).empty() && contains(why, "empty"), "empty refused");
    check(sanitize_account_name(std::string(300, 'x')).size() == 127, "length capped at 127");

    Customization all;
    all.bypass_checks = all.no_online_account = all.no_data_collection = all.no_bitlocker = all.declutter = true;
    all.local_account = "kiln";
    const std::string x = unattend_xml(all, "amd64");
    check(!contains(x, "windowsPE"), "no windowsPE pass (bypass goes into the hive)");
    check(count(x, "<settings pass=\"specialize\">") == 1 && count(x, "<settings pass=\"oobeSystem\">") == 1,
          "one specialize and one oobeSystem pass");
    check(contains(x, "BypassNRO /t REG_DWORD /d 1"), "BypassNRO in specialize");
    check(contains(x, "<Name>kiln</Name>") && contains(x, "UABhAHMAcwB3AG8AcgBkAA=="), "local account, empty password");
    check(contains(x, "net user &quot;kiln&quot; /logonpasswordchg:yes"), "password change forced at first logon");
    check(contains(x, "<ProtectYourPC>3</ProtectYourPC>") && contains(x, "<HideEULAPage>true</HideEULAPage>"),
          "data collection off, EULA page hidden");
    check(contains(x, "<PreventDeviceEncryption>true</PreventDeviceEncryption>") &&
              contains(x, "<TCGSecurityActivationDisabled>1</TCGSecurityActivationDisabled>"),
          "BitLocker off (both components)");
    check(count(x, "<FirstLogonCommands>") == 1, "exactly one FirstLogonCommands section");
    check(contains(x, "TurnOffWindowsCopilot") && contains(x, "Remove-AppxPackage -AllUsers"), "declutter commands");
    check(count(x, "processorArchitecture=\"amd64\"") == 4 && !contains(x, "arm64"), "architecture on every component");
    // Orders count 1..n inside each list.
    check(contains(x, "<Order>1</Order>") && contains(x, "<Order>7</Order>") && contains(x, "<Order>19</Order>") &&
              !contains(x, "<Order>0</Order>"),
          "commands numbered from 1");

    Customization msa;
    msa.no_online_account = true;
    const std::string y = unattend_xml(msa, "arm64");
    check(contains(y, "BypassNRO") && !contains(y, "oobeSystem") && contains(y, "\"arm64\""), "no-MS-account alone");

    bool threw = false;
    try {
        Customization bad;
        bad.local_account = "Guest";
        unattend_xml(bad, "amd64");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "a reserved local account name is refused by the generator too");
    threw = false;
    try {
        unattend_xml(msa, "x86");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "unknown architecture refused");

    if (std::system("command -v xmllint >/dev/null 2>&1") != 0) {
        std::printf("SKIP  xmllint not installed: XML not checked by an independent parser\n");
        probe::finish("unattend_probe");
        return probe::failures ? 1 : 77;
    }
    probe::TempDir tmp;
    for (const auto& [name, xml] : {std::pair{"all", x}, std::pair{"msa", y}}) {
        const auto p = tmp.path() / (std::string(name) + ".xml");
        std::ofstream(p) << xml;
        check(std::system(("xmllint --noout " + p.string() + " 2>/dev/null").c_str()) == 0,
              std::string("xmllint: well-formed [") + name + "]");
    }
    return probe::finish("unattend_probe");
}
