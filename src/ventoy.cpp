// ventoy.cpp — the layout and the checks; the writer (with Ventoy's files built
// in) is ventoy_write.cpp, so a program that only asks questions stays small.
#include "ventoy.hpp"

#include <stdexcept>

namespace kiln {
namespace {

constexpr uint64_t kPart1First = 2048;  // 1 MiB
constexpr uint64_t kPart2Sectors = 65536;  // 32 MiB: the size of Ventoy's image
constexpr uint64_t kGptBackup = 33;      // backup entries + header
constexpr uint64_t kMinData = 131072;    // 64 MiB of exFAT, at least

}  // namespace

VentoyLayout ventoy_layout(uint64_t disk_bytes, bool gpt, uint64_t reserve_mib) {
    VentoyLayout l;
    l.sectors = disk_bytes / 512;
    if (!gpt && l.sectors > 0x100000000ull)
        throw std::runtime_error("the drive is larger than 2 TiB, which an MBR partition table cannot address: use GPT");
    if (reserve_mib > l.sectors / 2048) throw std::runtime_error("the space to reserve is larger than the drive");
    const uint64_t tail = reserve_mib * 2048 + (gpt ? kGptBackup : 0) + kPart2Sectors;
    if (l.sectors < kPart1First + kMinData + tail + 8)
        throw std::runtime_error("the drive is too small for Ventoy" +
                                 std::string(reserve_mib ? " with that much space reserved" : ""));
    // As VentoyWorker.sh: partition 2 takes the last 32 MiB before the reserved
    // space (and the backup GPT), its start rounded down to 4 KiB.
    l.part1_first = kPart1First;
    l.part1_last = l.sectors - tail - 1;
    l.part2_first = l.part1_last + 1;
    l.part1_last -= l.part2_first % 8;
    l.part2_first = l.part1_last + 1;
    l.part2_last = l.part2_first + kPart2Sectors - 1;
    return l;
}

uint64_t ventoy_min_bytes(uint64_t reserve_mib) {
    return (kPart1First + kMinData + reserve_mib * 2048 + kGptBackup + kPart2Sectors + 8) * 512;
}

std::string ventoy_label_problem(const std::string& label) {
    if (label.empty()) return "the label is empty";
    size_t chars = 0;
    for (unsigned char c : label) {
        if ((c & 0xC0) != 0x80) ++chars;  // not a UTF-8 continuation byte
        if (c >= 0xF0) return "the label has a character exFAT cannot store";
        if (c < 0x20 || c == 0x7F) return "the label has a control character";
        for (char bad : std::string("\"*/:<>?\\|"))
            if (c == static_cast<unsigned char>(bad)) return std::string("the label cannot contain ") + bad;
    }
    if (chars > 11) return "the label is longer than 11 characters";
    return {};
}

const char* ventoy_version() { return KILN_VENTOY_VERSION; }

}  // namespace kiln
