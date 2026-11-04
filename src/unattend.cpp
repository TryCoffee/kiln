// unattend.cpp — the answer file.
//
// Ported from Rufus, src/wue.c (CreateUnattendXml), Copyright © 2022-2026 Pete
// Batard <pete@akeo.ie>, GPL-3.0-or-later. The settings, their passes and the
// commands are Rufus's; the windowsPE pass is left out because Kiln applies the
// hardware bypass to boot.wim's registry hive directly, and Rufus's regional
// options, offline internal drives, S Mode, silent install and 2023 boot loaders
// are not ported (yet).
#include "unattend.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace kiln {
namespace {

constexpr const char* kInvalidChars = "/\\[]:;|=.,+*?<>%@&\"";  // Rufus USERNAME_INVALID_CHARS
constexpr size_t kMaxAccount = 127;                              // Rufus MAX_USERNAME_LENGTH - 1

// From https://learn.microsoft.com/en-us/archive/technet-wiki/13813.localized-names-for-administrator-account-in-windows
const std::array<const char*, 15> kReserved = {
    "Administrator", "Järjestelmänvalvoja", "Administrateur", "Rendszergazda", "Administrador",
    "Администратор", "Administratör",       "Guest",          "DefaultAccount", "WDAGUtilityAccount",
    "HelpAssistant", "KRBTGT",              "Local",          "NONE",           "SYSTEM"};

bool iequals_ascii(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string component(const char* name, const std::string& arch) {
    return std::string("    <component name=\"") + name + "\" processorArchitecture=\"" + arch +
           "\" language=\"neutral\" xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" "
           "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" publicKeyToken=\"31bf3856ad364e35\" "
           "versionScope=\"nonSxS\">\n";
}

}  // namespace

bool Customization::any() const { return bypass_checks || needs_answer_file(); }

bool Customization::needs_answer_file() const {
    return no_online_account || !local_account.empty() || no_data_collection || no_bitlocker || declutter;
}

std::string sanitize_account_name(const std::string& name, std::string* why) {
    std::string s = name;
    for (char& c : s)
        if (std::strchr(kInvalidChars, c) || static_cast<unsigned char>(c) < 0x20) c = '_';
    const auto first = s.find_first_not_of(" \t");
    const auto last = s.find_last_not_of(" \t");
    s = first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
    if (s.size() > kMaxAccount) s.resize(kMaxAccount);
    if (s.empty()) {
        if (why) *why = "the account name is empty";
        return {};
    }
    for (const char* r : kReserved)
        if (iequals_ascii(s, r)) {
            if (why) *why = "'" + s + "' is reserved by Windows";
            return {};
        }
    return s;
}

std::string unattend_xml(const Customization& c, const std::string& arch) {
    if (arch != "amd64" && arch != "arm64") throw std::invalid_argument("unknown architecture " + arch);
    std::string user;
    if (!c.local_account.empty()) {
        std::string why;
        user = sanitize_account_name(c.local_account, &why);
        if (user.empty()) throw std::invalid_argument("local account: " + why);
    }

    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n";

    // specialize: Microsoft-Windows-Deployment RunSynchronous
    std::vector<std::string> specialize;
    if (c.no_online_account)  // from https://github.com/AveYo/MediaCreationTool.bat (bypass11/AutoUnattend.xml), via Rufus
        specialize.push_back("reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\OOBE\" /v BypassNRO /t REG_DWORD /d 1 /f");
    if (c.declutter) {
        specialize.push_back("reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\OneDrive\" /v DisableFileSyncNGSC /t REG_DWORD /d 1 /f");
        specialize.push_back("PowerShell -NonInteractive -WindowStyle Hidden -Command \"Remove-Item -Path $env:SystemRoot\\System32\\OneDriveSetup.exe -Force -Confirm:$false; Remove-Item -Path $env:SystemRoot\\SysWOW64\\OneDriveSetup.exe -Force -Confirm:$false;\"");
        specialize.push_back("PowerShell -NonInteractive -WindowStyle Hidden -Command \"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Outlook*'} | Remove-AppxProvisionedPackage -Online\"");
        specialize.push_back("PowerShell -NonInteractive -WindowStyle Hidden -Command \"Get-AppxPackage -AllUsers *Outlook* | Remove-AppxPackage -AllUsers\"");
        specialize.push_back("PowerShell -NonInteractive -WindowStyle Hidden -Command \"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Teams*'} | Remove-AppxProvisionedPackage -Online\"");
        specialize.push_back("PowerShell -NonInteractive -WindowStyle Hidden -Command \"Get-AppxPackage -AllUsers *Teams* | Remove-AppxPackage -AllUsers\"");
    }
    if (!specialize.empty()) {
        x += "  <settings pass=\"specialize\">\n" + component("Microsoft-Windows-Deployment", arch);
        x += "      <RunSynchronous>\n";
        for (size_t i = 0; i < specialize.size(); ++i)
            x += "        <RunSynchronousCommand wcm:action=\"add\">\n          <Order>" + std::to_string(i + 1) +
                 "</Order>\n          <Path>" + xml_escape(specialize[i]) + "</Path>\n        </RunSynchronousCommand>\n";
        x += "      </RunSynchronous>\n    </component>\n  </settings>\n";
    }

    // oobeSystem
    std::vector<std::string> first_logon;
    std::string shell;
    if (c.no_data_collection)
        // "ProtectYourPC" 3 = only the required diagnostics; the name is Microsoft's.
        shell += "      <OOBE>\n        <HideEULAPage>true</HideEULAPage>\n        <ProtectYourPC>3</ProtectYourPC>\n      </OOBE>\n";
    if (!user.empty()) {
        // A local account in the answer file also makes Windows 11 skip the
        // Microsoft-account step while online. Empty password: the base64 of
        // UTF-16 "Password" is how unattend spells it; changed at first logon.
        const std::string u = xml_escape(user);
        shell += "      <UserAccounts>\n        <LocalAccounts>\n          <LocalAccount wcm:action=\"add\">\n"
                 "            <Name>" + u + "</Name>\n            <DisplayName>" + u + "</DisplayName>\n"
                 "            <Group>Administrators;Power Users</Group>\n"
                 "            <Password>\n              <Value>UABhAHMAcwB3AG8AcgBkAA==</Value>\n"
                 "              <PlainText>false</PlainText>\n            </Password>\n"
                 "          </LocalAccount>\n        </LocalAccounts>\n      </UserAccounts>\n";
        first_logon.push_back("net user \"" + user + "\" /logonpasswordchg:yes");
        first_logon.push_back("net accounts /maxpwage:unlimited");  // `net user` can reset expiry to 90 days
    }
    if (c.declutter) {
        const char* reg[] = {
            "reg add \"HKLM\\System\\CurrentControlSet\\Control\\Session Manager\\Power\" /v HiberbootEnabled /t REG_DWORD /d 0 /f",
            "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v ShowCopilotButton /t REG_DWORD /d 0 /f",
            "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\WindowsCopilot\" /v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f",
            "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" /v SearchboxTaskbarMode /t REG_DWORD /d 1 /f",
            "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" /v SearchboxTaskbarModeCache /t REG_DWORD /d 1 /f",
            "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f",
            "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager\" /v SystemPaneSuggestionsEnabled /t REG_DWORD /d 0 /f",
            "reg add \"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Search\" /v BingSearchEnabled /t REG_DWORD /d 0 /f",
            "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Device Metadata\" /v PreventDeviceMetadataFromNetwork /t REG_DWORD /d 1 /f",
            "reg add \"HKLM\\Software\\Policies\\Microsoft\\Dsh\" /v AllowNewsAndInterests /t REG_DWORD /d 0 /f",
            "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Windows Feeds\" /v EnableFeeds /t REG_DWORD /d 0 /f",
            "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Communications\" /v ConfigureChatAutoInstall /t REG_DWORD /d 0 /f",
            "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" /v DisableCloudOptimizedContent /t REG_DWORD /d 1 /f",
            "reg add \"HKLM\\Software\\Policies\\Microsoft\\Edge\" /v HideFirstRunExperience /t REG_DWORD /d 1 /f",
            "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v Start_Layout /t REG_DWORD /d 1 /f",
            // Documents, Downloads, Network, Personal Folder, File Explorer, Settings on Start (Rufus: reg add of
            // the binary does not take effect from unattend, PowerShell does)
            "PowerShell -NonInteractive -WindowStyle Hidden -Command \"Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Start' -Name 'VisiblePlaces' -Value $([convert]::FromBase64String('ztU0LVr6Q0WC8iLm6vd3PC+zZ+PeiVVDv85h83sYqTe8JIoUDNaJQqCAbtm7okiCRIF1/g0IrkKL2jTtl7ZjlEqwvXRK+WhPi9ZDmAcdqLyGCHNSqlFDQp97J3ZYRlnU')) -Type 'Binary'\"",
            "reg add \"HKCU\\Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32\" /ve /t REG_SZ /d \"\" /f",
        };
        for (const char* r : reg) first_logon.push_back(r);
    }
    if (!first_logon.empty()) {
        // Only one FirstLogonCommands section is allowed (Rufus found out the hard way).
        shell += "      <FirstLogonCommands>\n";
        for (size_t i = 0; i < first_logon.size(); ++i)
            shell += "        <SynchronousCommand wcm:action=\"add\">\n          <Order>" + std::to_string(i + 1) +
                     "</Order>\n          <CommandLine>" + xml_escape(first_logon[i]) +
                     "</CommandLine>\n        </SynchronousCommand>\n";
        shell += "      </FirstLogonCommands>\n";
    }
    std::string oobe;
    if (!shell.empty()) oobe += component("Microsoft-Windows-Shell-Setup", arch) + shell + "    </component>\n";
    if (c.no_bitlocker) {
        oobe += component("Microsoft-Windows-SecureStartup-FilterDriver", arch) +
                "      <PreventDeviceEncryption>true</PreventDeviceEncryption>\n    </component>\n";
        oobe += component("Microsoft-Windows-EnhancedStorage-Adm", arch) +
                "      <TCGSecurityActivationDisabled>1</TCGSecurityActivationDisabled>\n    </component>\n";
    }
    if (!oobe.empty()) x += "  <settings pass=\"oobeSystem\">\n" + oobe + "  </settings>\n";

    x += "</unattend>\n";
    return x;
}

}  // namespace kiln
