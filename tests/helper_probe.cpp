// helper_probe.cpp — the GUI <-> kiln-helper protocol, and the helper's refusal
// to run unprivileged. (Its privileged checks need root or an administrator and
// a drive: tested by hand.)
#include "blockio.hpp"
#include "check.hpp"
#include "helper_protocol.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <clocale>
#include <cstdio>
#include <locale>

using namespace kiln;
using probe::check;
using K = HelperEvent::Kind;

int main() {
    HelperRequest r;
    r.device = "sdc";
    r.image = "/home/u/My ISOs/Win 11 (pl).iso";
    r.verify = false;
    r.custom.bypass_checks = r.custom.no_online_account = r.custom.no_bitlocker = true;
    r.custom.local_account = "Jan Kowalski";
    r.temp_dir = "C:\\Users\\Zażółć\\AppData\\Local\\Temp";
    const auto args = helper_args(r);
    const HelperRequest back = parse_helper_args(args);
    check(back.device == "sdc" && back.image == r.image, "device and a path with spaces survive the round trip");
    check(back.temp_dir == r.temp_dir, "temp dir survives (Windows helper)");
    check(!back.verify && !back.raw && !back.force, "flags survive");
    check(back.custom.bypass_checks && back.custom.no_online_account && back.custom.no_bitlocker &&
              !back.custom.declutter && !back.custom.no_data_collection && back.custom.local_account == "Jan Kowalski",
          "tweaks survive, account name with a space intact");
    for (const auto& bad : std::vector<std::vector<std::string>>{
             {}, {"write"}, {"write", "sdc"}, {"read", "sdc", "x.iso"}, {"write", "sdc", "x.iso", "--to-file"},
             {"write", "sdc", "x.iso", "--local-account"}, {"write", "sdc", "x.iso", "--temp-dir"}, {"write", "", "x.iso"}}) {
        bool threw = false;
        try {
            parse_helper_args(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        std::string joined;
        for (const auto& b : bad) joined += b + " ";
        check(threw, "refused: [" + joined + "]");
    }

    HelperRequest v;
    v.ventoy = true;
    v.device = "sdd";
    v.verify = false;
    v.ventoy_setup.gpt = true;
    v.ventoy_setup.secure_boot = false;
    v.ventoy_setup.label = "Mój pendrive";
    v.ventoy_setup.reserve_mib = 512;
    v.temp_dir = r.temp_dir;
    const auto vargs = helper_args(v);
    check(vargs[0] == "ventoy" && vargs[1] == "sdd", "Ventoy request: ventoy <device>, no image");
    const HelperRequest vb = parse_helper_args(vargs);
    check(vb.ventoy && vb.device == "sdd" && vb.image.empty() && !vb.verify, "Ventoy request survives the round trip");
    check(vb.ventoy_setup.gpt && !vb.ventoy_setup.secure_boot && vb.ventoy_setup.label == "Mój pendrive" &&
              vb.ventoy_setup.reserve_mib == 512 && vb.temp_dir == r.temp_dir,
          "Ventoy options survive (GPT, no Secure Boot, a label with a space and a Polish letter, reserve)");
    const HelperRequest vd = parse_helper_args(helper_args([] {
        HelperRequest d;
        d.ventoy = true;
        d.device = "sdd";
        return d;
    }()));
    check(vd.ventoy && !vd.ventoy_setup.gpt && vd.ventoy_setup.secure_boot && vd.ventoy_setup.label == "Ventoy" &&
              vd.ventoy_setup.reserve_mib == 0 && vd.verify,
          "Ventoy defaults: MBR, Secure Boot support, label Ventoy, nothing reserved, verify");
    check(!parse_helper_args(args).ventoy, "an image write is not a Ventoy request");
    for (const auto& bad : std::vector<std::vector<std::string>>{
             {"ventoy"}, {"ventoy", ""}, {"ventoy", "sdd", "x.iso"}, {"ventoy", "sdd", "--bypass-checks"},
             {"ventoy", "sdd", "--reserve"}, {"ventoy", "sdd", "--reserve", "-5"}, {"ventoy", "sdd", "--reserve", "1e3"},
             {"ventoy", "sdd", "--reserve", "9999999999"}, {"ventoy", "sdd", "--label"}, {"write", "sdc", "x.iso", "--gpt"}}) {
        bool threw = false;
        try {
            parse_helper_args(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        std::string joined;
        for (const auto& b : bad) joined += b + " ";
        check(threw, "refused: [" + joined + "]");
    }

    // Windows: the GUI passes the arguments through ShellExecuteEx as one string.
    const std::vector<std::pair<std::vector<std::string>, std::string>> quoting = {
        {{"write", "PhysicalDrive2"}, "write PhysicalDrive2"},
        {{"C:\\My ISOs\\Win 11.iso"}, "\"C:\\My ISOs\\Win 11.iso\""},
        {{"C:\\dir with space\\"}, "\"C:\\dir with space\\\\\""},
        {{"C:\\plain\\"}, "C:\\plain\\"},
        {{"say \"hi\""}, "\"say \\\"hi\\\"\""},
        {{"a\\\"b"}, "\"a\\\\\\\"b\""},
        {{""}, "\"\""},
    };
    for (const auto& [in, want] : quoting) {
        const std::string got = quote_windows_args(in);
        check(got == want, "windows quoting: " + got);
    }
#ifdef _WIN32
    {
        const std::vector<std::string> tricky = {"write", "PhysicalDrive2", "C:\\Users\\Zażółć\\ISO dir\\",
                                                 "x\"y", "", "back\\\\", "\\\"", "--local-account", "Jan Kowalski"};
        const std::string line = "kiln-helper.exe " + quote_windows_args(tricky);
        const int wn = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(wn), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), wn);
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(wide.c_str(), &argc);
        std::vector<std::string> parsed;
        for (int i = 1; argv && i < argc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string u(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, u.data(), n, nullptr, nullptr);
            u.resize(static_cast<size_t>(n > 0 ? n - 1 : 0));
            parsed.push_back(u);
        }
        LocalFree(argv);
        check(parsed == tricky, "CommandLineToArgvW gives every argument back unchanged");
    }
#endif

    for (Stage s : {Stage::hashing, Stage::writing, Stage::syncing, Stage::verifying, Stage::splitting,
                    Stage::customizing, Stage::partitioning, Stage::copying})
        check(stage_from_key(stage_key(s)) == s, std::string("stage key round trip: ") + stage_key(s));

    // The GUI parses under the user's locale; Polish uses a decimal comma.
    if (std::setlocale(LC_ALL, "pl_PL.UTF-8")) {
        try {
            std::locale::global(std::locale("pl_PL.UTF-8"));
        } catch (const std::exception&) {
        }
    }
    HelperEvent ok;
    ok.kind = K::ok;
    ok.direct = true;
    ok.write_seconds = 83.4;
    ok.verify_seconds = 9.25;
    ok.sha256 = "ab12";
    const auto pok = parse_event(format_event(ok));
    check(pok && pok->kind == K::ok && pok->direct && pok->write_seconds > 83.3 && pok->write_seconds < 83.5 &&
              pok->verify_seconds > 9.1 && pok->sha256 == "ab12",
          "result ok round trip under a decimal-comma locale (" + format_event(ok) + ")");
    std::setlocale(LC_ALL, "C");
    std::locale::global(std::locale::classic());

    HelperEvent st;
    st.kind = K::stage;
    st.progress = {Stage::copying, 123456789012ull, 8000000000ull};
    const auto pst = parse_event(format_event(st));
    check(pst && pst->kind == K::stage && pst->progress.stage == Stage::copying && pst->progress.done == 123456789012ull &&
              pst->progress.total == 8000000000ull,
          "stage event with 64-bit counts");
    HelperEvent m;
    m.kind = K::error;
    m.text = "two\nlines";
    const auto pm = parse_event(format_event(m));
    check(pm && pm->kind == K::error && pm->text == "two lines", "a newline in text cannot break the line protocol");
    HelperEvent c;
    c.kind = K::cancelled;
    check(parse_event(format_event(c))->kind == K::cancelled, "cancelled");
    HelperEvent sha_less = ok;
    sha_less.sha256.clear();
    check(parse_event(format_event(sha_less))->sha256.empty(), "Windows result: no SHA-256 ('-')");
    for (const char* junk : {"", "stage", "stage writing x y", "stage nosuch 1 2", "result", "result ok 1", "hello"})
        check(!parse_event(junk), std::string("garbage refused: '") + junk + "'");

    // The real helper, run unprivileged, must refuse before touching anything.
#ifdef _WIN32
    // cmd /c runs it: no quotes around the path (the build directory has no spaces)
    const char* cmd = KILN_HELPER_PATH " --pipe kiln-probe-none write PhysicalDrive99 C:\\Windows\\win.ini 2>&1 < NUL";
    const char* expect = "result error kiln-helper must run as administrator";
#else
    const char* cmd = KILN_HELPER_PATH " write sdz /etc/hostname 2>&1 < /dev/null";
    const char* expect = "result error kiln-helper must run as root";
#endif
    std::string out;
#ifdef _WIN32
    if (running_as_admin()) {  // OpenSSH hands an administrator the elevated token
        std::printf("SKIP  helper refuses when not elevated: this session is elevated\n");
        return probe::finish("helper_probe");
    }
#endif
    if (FILE* p = popen(cmd, "r")) {
        char buf[512];
        while (fgets(buf, sizeof buf, p)) out += buf;
        const int rc = pclose(p);
        check(rc != 0 && out.find(expect) == 0, "helper refuses when not privileged: " + out);
    } else {
        check(false, "could not run kiln-helper");
    }
    return probe::finish("helper_probe");
}
