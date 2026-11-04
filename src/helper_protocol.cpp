// helper_protocol.cpp
#include "helper_protocol.hpp"

#include <array>
#include <cstdio>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace kiln {
namespace {

const std::array<std::pair<Stage, const char*>, 8> kStages = {{{Stage::hashing, "hashing"},
                                                               {Stage::writing, "writing"},
                                                               {Stage::syncing, "syncing"},
                                                               {Stage::verifying, "verifying"},
                                                               {Stage::splitting, "splitting"},
                                                               {Stage::customizing, "customizing"},
                                                               {Stage::partitioning, "partitioning"},
                                                               {Stage::copying, "copying"}}};

std::string one_line(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

uint64_t parse_mib(const std::string& s) {
    if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("--reserve takes a whole number of MiB, not " + s);
    return std::stoull(s);
}

}  // namespace

std::vector<std::string> helper_args(const HelperRequest& r) {
    if (r.ventoy) {
        std::vector<std::string> a = {"ventoy", r.device, "--label", r.ventoy_setup.label};
        if (!r.verify) a.push_back("--no-verify");
        if (r.ventoy_setup.gpt) a.push_back("--gpt");
        if (!r.ventoy_setup.secure_boot) a.push_back("--no-secure-boot");
        if (r.ventoy_setup.reserve_mib) {
            a.push_back("--reserve");
            a.push_back(std::to_string(r.ventoy_setup.reserve_mib));
        }
        if (!r.temp_dir.empty()) {
            a.push_back("--temp-dir");
            a.push_back(r.temp_dir);
        }
        return a;
    }
    std::vector<std::string> a = {"write", r.device, r.image};
    if (!r.verify) a.push_back("--no-verify");
    if (r.raw) a.push_back("--raw");
    if (r.force) a.push_back("--force");
    if (!r.temp_dir.empty()) {
        a.push_back("--temp-dir");
        a.push_back(r.temp_dir);
    }
    if (r.custom.bypass_checks) a.push_back("--bypass-checks");
    if (r.custom.no_online_account) a.push_back("--no-ms-account");
    if (!r.custom.local_account.empty()) {
        a.push_back("--local-account");
        a.push_back(r.custom.local_account);
    }
    if (r.custom.no_data_collection) a.push_back("--no-data-collection");
    if (r.custom.no_bitlocker) a.push_back("--no-bitlocker");
    if (r.custom.declutter) a.push_back("--declutter");
    return a;
}

std::string quote_windows_args(const std::vector<std::string>& args) {
    std::string out;
    for (const std::string& a : args) {
        if (!out.empty()) out += ' ';
        if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) {
            out += a;
            continue;
        }
        // Inside quotes, backslashes are literal except before a quote: there 2n
        // backslashes give n, 2n+1 give n and a literal quote.
        out += '"';
        size_t backslashes = 0;
        for (char c : a) {
            if (c == '\\') {
                ++backslashes;
                continue;
            }
            if (c == '"') out.append(2 * backslashes + 1, '\\');
            else out.append(backslashes, '\\');
            backslashes = 0;
            out += c;
        }
        out.append(2 * backslashes, '\\');  // before the closing quote
        out += '"';
    }
    return out;
}

HelperRequest parse_helper_args(const std::vector<std::string>& a) {
    if (a.size() >= 2 && a[0] == "ventoy") {
        HelperRequest r;
        r.ventoy = true;
        r.device = a[1];
        if (r.device.empty()) throw std::invalid_argument("empty device");
        for (size_t i = 2; i < a.size(); ++i) {
            const std::string& s = a[i];
            if (s == "--no-verify") r.verify = false;
            else if (s == "--gpt") r.ventoy_setup.gpt = true;
            else if (s == "--no-secure-boot") r.ventoy_setup.secure_boot = false;
            else if (s == "--label" && i + 1 < a.size()) r.ventoy_setup.label = a[++i];
            else if (s == "--reserve" && i + 1 < a.size()) r.ventoy_setup.reserve_mib = parse_mib(a[++i]);
            else if (s == "--temp-dir" && i + 1 < a.size()) r.temp_dir = a[++i];
            else throw std::invalid_argument("unknown or incomplete option for a Ventoy disk: " + s);
        }
        return r;
    }
    if (a.size() < 3 || a[0] != "write")
        throw std::invalid_argument("usage: kiln-helper write <device> <image> [options] | ventoy <device> [options]");
    HelperRequest r;
    r.device = a[1];
    r.image = a[2];
    if (r.device.empty() || r.image.empty()) throw std::invalid_argument("empty device or image");
    for (size_t i = 3; i < a.size(); ++i) {
        const std::string& s = a[i];
        if (s == "--no-verify") r.verify = false;
        else if (s == "--raw") r.raw = true;
        else if (s == "--force") r.force = true;
        else if (s == "--bypass-checks") r.custom.bypass_checks = true;
        else if (s == "--no-ms-account") r.custom.no_online_account = true;
        else if (s == "--no-data-collection") r.custom.no_data_collection = true;
        else if (s == "--no-bitlocker") r.custom.no_bitlocker = true;
        else if (s == "--declutter") r.custom.declutter = true;
        else if (s == "--local-account" && i + 1 < a.size()) r.custom.local_account = a[++i];
        else if (s == "--temp-dir" && i + 1 < a.size()) r.temp_dir = a[++i];
        else throw std::invalid_argument("unknown or incomplete option " + s);
    }
    return r;
}

const char* stage_key(Stage s) {
    for (const auto& [st, key] : kStages)
        if (st == s) return key;
    return "?";
}

std::optional<Stage> stage_from_key(const std::string& key) {
    for (const auto& [st, k] : kStages)
        if (key == k) return st;
    return std::nullopt;
}

std::string format_event(const HelperEvent& e) {
    using K = HelperEvent::Kind;
    switch (e.kind) {
        case K::stage:
            return std::string("stage ") + stage_key(e.progress.stage) + " " + std::to_string(e.progress.done) + " " +
                   std::to_string(e.progress.total);
        case K::msg: return "msg " + one_line(e.text);
        case K::ok: {
            std::ostringstream o;
            o.imbue(std::locale::classic());  // "83.4" whatever the process locale says
            o.setf(std::ios::fixed);
            o.precision(1);
            o << "result ok " << (e.direct ? 1 : 0) << ' ' << e.write_seconds << ' ' << e.verify_seconds << ' '
              << (e.sha256.empty() ? "-" : e.sha256);
            return o.str();
        }
        case K::mismatch: return "result mismatch " + one_line(e.text);
        case K::cancelled: return "result cancelled";
        case K::error: return "result error " + one_line(e.text);
    }
    return "msg ?";
}

std::optional<HelperEvent> parse_event(const std::string& line) {
    using K = HelperEvent::Kind;
    HelperEvent e;
    std::istringstream in(line);
    in.imbue(std::locale::classic());  // the GUI runs under the user's locale (decimal comma)
    std::string word;
    in >> word;
    auto rest = [&] {
        std::string r;
        std::getline(in, r);
        if (!r.empty() && r[0] == ' ') r.erase(0, 1);
        return r;
    };
    if (word == "stage") {
        std::string key;
        unsigned long long done = 0, total = 0;
        if (!(in >> key >> done >> total)) return std::nullopt;
        const auto st = stage_from_key(key);
        if (!st) return std::nullopt;
        e.kind = K::stage;
        e.progress = {*st, done, total};
        return e;
    }
    if (word == "msg") {
        e.kind = K::msg;
        e.text = rest();
        return e;
    }
    if (word != "result") return std::nullopt;
    in >> word;
    if (word == "ok") {
        int direct = 0;
        std::string sha;
        if (!(in >> direct >> e.write_seconds >> e.verify_seconds >> sha)) return std::nullopt;
        e.kind = K::ok;
        e.direct = direct != 0;
        e.sha256 = sha == "-" ? "" : sha;
        return e;
    }
    if (word == "mismatch") e.kind = K::mismatch;
    else if (word == "cancelled") e.kind = K::cancelled;
    else if (word == "error") e.kind = K::error;
    else return std::nullopt;
    e.text = rest();
    return e;
}

}  // namespace kiln
