// gpt.cpp
#include "gpt.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <stdexcept>

namespace kiln {
namespace {

constexpr uint32_t kEntries = 128;
constexpr uint32_t kEntrySize = 128;
constexpr uint64_t kAlign = 1u << 20;

using Guid = std::array<unsigned char, 16>;

// EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, in GPT's mixed-endian byte order.
constexpr Guid kBasicData = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                             0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};

void put16(unsigned char* p, uint16_t v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
}
void put32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}
void put64(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}

Guid random_guid(std::mt19937_64& rng) {
    Guid g;
    for (size_t i = 0; i < g.size(); i += 8) {
        const uint64_t r = rng();
        for (size_t j = 0; j < 8; ++j) g[i + j] = static_cast<unsigned char>(r >> (8 * j));
    }
    g[7] = static_cast<unsigned char>((g[7] & 0x0F) | 0x40);  // version 4 (little-endian third field)
    g[8] = static_cast<unsigned char>((g[8] & 0x3F) | 0x80);  // RFC 4122 variant
    return g;
}

void header(unsigned char* h, uint64_t my_lba, uint64_t alt_lba, uint64_t first_usable, uint64_t last_usable,
            const Guid& disk, uint64_t entries_lba, uint32_t entries_crc) {
    std::memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);  // revision 1.0
    put32(h + 12, 92);         // header size
    put32(h + 16, 0);          // header CRC, filled below
    put64(h + 24, my_lba);
    put64(h + 32, alt_lba);
    put64(h + 40, first_usable);
    put64(h + 48, last_usable);
    std::memcpy(h + 56, disk.data(), 16);
    put64(h + 72, entries_lba);
    put32(h + 80, kEntries);
    put32(h + 84, kEntrySize);
    put32(h + 88, entries_crc);
    put32(h + 16, crc32(h, 92));
}

}  // namespace

uint32_t crc32(const void* data, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    auto p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint64_t gpt_first_usable(uint32_t sector) { return 2 + kEntries * kEntrySize / sector; }

uint64_t gpt_last_usable(uint64_t disk_bytes, uint32_t sector) {
    return disk_bytes / sector - 2 - kEntries * kEntrySize / sector;
}

GptLayout make_gpt(uint64_t disk_bytes, uint32_t sector, const std::string& partition_name, uint64_t seed) {
    if (sector != 512 && sector != 4096) throw std::invalid_argument("sector size must be 512 or 4096");
    const uint64_t sectors = disk_bytes / sector;
    const uint64_t align = kAlign / sector;
    GptPartition p{partition_name, align, (gpt_last_usable(disk_bytes, sector) + 1) / align * align - 1, 0};
    if (sectors < 4 * align || p.last <= p.first) throw std::runtime_error("the drive is too small for a partition table");
    return make_gpt(disk_bytes, sector, std::vector<GptPartition>{p}, seed);
}

GptLayout make_gpt(uint64_t disk_bytes, uint32_t sector, const std::vector<GptPartition>& parts, uint64_t seed) {
    if (sector != 512 && sector != 4096) throw std::invalid_argument("sector size must be 512 or 4096");
    if (parts.empty() || parts.size() > kEntries) throw std::invalid_argument("a GPT holds 1 to 128 partitions");
    GptLayout g;
    g.sector = sector;
    g.sectors = disk_bytes / sector;
    const uint64_t entry_sectors = kEntries * kEntrySize / sector;  // 32 or 4
    const uint64_t first_usable = gpt_first_usable(sector);
    if (g.sectors < first_usable + entry_sectors + 2) throw std::runtime_error("the drive is too small for a partition table");
    const uint64_t last_usable = gpt_last_usable(disk_bytes, sector);
    uint64_t next = first_usable;
    for (const auto& p : parts) {
        if (p.first < next || p.last < p.first || p.last > last_usable)
            throw std::invalid_argument("GPT partition " + p.name + " is out of place");
        next = p.last + 1;
    }
    g.part_first = parts[0].first;
    g.part_last = parts[0].last;

    std::mt19937_64 rng(seed ? seed : std::random_device{}() ^ (uint64_t{std::random_device{}()} << 32));
    const Guid disk = random_guid(rng);

    std::vector<unsigned char> entries(kEntries * kEntrySize, 0);
    for (size_t i = 0; i < parts.size(); ++i) {
        unsigned char* e = entries.data() + i * kEntrySize;
        const Guid part = random_guid(rng);
        std::memcpy(e, kBasicData.data(), 16);
        std::memcpy(e + 16, part.data(), 16);
        put64(e + 32, parts[i].first);
        put64(e + 40, parts[i].last);
        put64(e + 48, parts[i].attributes);
        for (size_t c = 0; c < parts[i].name.size() && c < 36; ++c)
            put16(e + 56 + 2 * c, static_cast<unsigned char>(parts[i].name[c]));  // ASCII -> UTF-16LE
    }
    const uint32_t entries_crc = crc32(entries.data(), entries.size());

    // LBA 0: protective MBR, one 0xEE partition covering the disk (capped at 2^32-1 sectors).
    g.head.assign(first_usable * sector, 0);
    unsigned char* mbr = g.head.data();
    unsigned char* pe = mbr + 446;
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;  // CHS of LBA 1
    pe[4] = 0xEE;
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;
    put32(pe + 8, 1);
    put32(pe + 12, static_cast<uint32_t>(std::min<uint64_t>(g.sectors - 1, 0xFFFFFFFFu)));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    header(g.head.data() + sector, 1, g.sectors - 1, first_usable, last_usable, disk, 2, entries_crc);
    std::memcpy(g.head.data() + 2 * sector, entries.data(), entries.size());

    g.tail_lba = g.sectors - 1 - entry_sectors;
    g.tail.assign((entry_sectors + 1) * sector, 0);
    std::memcpy(g.tail.data(), entries.data(), entries.size());
    header(g.tail.data() + entry_sectors * sector, g.sectors - 1, 1, first_usable, last_usable, disk, g.tail_lba,
           entries_crc);
    return g;
}

}  // namespace kiln
