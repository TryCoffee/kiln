// version_compare.hpp — release versions as Kiln's tags write them ("v0.1.1"), for the
// GUI's update check.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace kiln {

/// "v0.2.10" or "0.2.10" -> {0, 2, 10}. Two to four dot-separated numbers of at most
/// five digits; anything else (a pre-release suffix, garbage) -> nullopt.
inline std::optional<std::vector<unsigned>> parse_version(std::string_view s) {
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.remove_prefix(1);
    std::vector<unsigned> parts;
    unsigned n = 0;
    int digits = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (digits == 0) return std::nullopt;
            parts.push_back(n);
            n = 0;
            digits = 0;
        } else if (s[i] >= '0' && s[i] <= '9' && digits < 5) {
            n = n * 10 + static_cast<unsigned>(s[i] - '0');
            ++digits;
        } else {
            return std::nullopt;
        }
    }
    if (parts.size() < 2 || parts.size() > 4) return std::nullopt;
    return parts;
}

/// a is a later release than b (missing parts count as 0: 0.2 == 0.2.0).
inline bool version_newer(const std::vector<unsigned>& a, const std::vector<unsigned>& b) {
    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
        const unsigned x = i < a.size() ? a[i] : 0;
        const unsigned y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

}  // namespace kiln
