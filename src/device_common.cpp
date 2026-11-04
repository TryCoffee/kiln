// device_common.cpp — the parts of the drive survey every platform shares.
#include "device.hpp"

#include <array>
#include <cstdio>

namespace kiln {

std::vector<Device> list_devices(const SystemView& v) {
    std::vector<Device> out;
    for (auto& st : survey_disks(v))
        if (st.eligibility == Eligibility::eligible) out.push_back(std::move(st.device));
    return out;
}

std::string device_name_from_arg(const std::string& arg) {
    for (const char* prefix : {"/dev/", "\\\\.\\", "//./"})
        if (arg.rfind(prefix, 0) == 0) return arg.substr(std::char_traits<char>::length(prefix));
    return arg;
}

std::string format_size(uint64_t bytes) {
    static const std::array<const char*, 5> units = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    char buf[32];
    if (unit == 0)
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else
        std::snprintf(buf, sizeof buf, "%.1f %s", value, units[unit]);
    return buf;
}

}  // namespace kiln
