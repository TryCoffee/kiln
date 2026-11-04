// unattend.hpp — Rufus-style Windows setup customisation: the answer file and
// the options behind it.
//
// The TPM / Secure Boot / RAM bypass is NOT in the answer file: Kiln writes it
// straight into boot.wim's offline SYSTEM hive (as Rufus does when it can),
// which avoids the console window a windowsPE-pass `reg add` shows. Everything
// else goes into an answer file that Setup carries into the installed system
// (sources/$OEM$/$$/Panther/unattend.xml).
#pragma once

#include <string>

namespace kiln {

struct Customization {
    bool bypass_checks = false;       // TPM 2.0, Secure Boot and 4 GB RAM checks off (fresh installs)
    bool no_online_account = false;   // BypassNRO: Setup accepts an offline/local account
    std::string local_account;        // create this local administrator, empty password, change at first logon
    bool no_data_collection = false;  // privacy questions answered "no", EULA page skipped
    bool no_bitlocker = false;        // no automatic device encryption
    bool declutter = false;           // OneDrive/Outlook/Teams/Copilot/news/ads off, classic context menu

    bool any() const;
    // Anything that needs the answer file (everything but bypass_checks).
    bool needs_answer_file() const;
};

// Rufus's rules: invalid characters become '_', surrounding blanks trimmed,
// Windows' reserved account names refused. Empty result = not allowed.
std::string sanitize_account_name(const std::string& name, std::string* why_refused = nullptr);

// `arch`: "amd64" or "arm64". Throws on an invalid local account name.
std::string unattend_xml(const Customization& c, const std::string& arch);

}  // namespace kiln
