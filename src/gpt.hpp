// gpt.hpp — a GPT built in memory (UEFI spec 2.10, ch. 5).
//
// Windows install sticks: one "Microsoft basic data" partition from 1 MiB to
// the last whole MiB before the backup table. Ventoy disks: two basic data
// partitions the caller places.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kiln {

struct GptLayout {
    uint32_t sector = 512;
    uint64_t sectors = 0;            // whole disk
    uint64_t part_first = 0;         // LBA (the first partition)
    uint64_t part_last = 0;          // LBA, inclusive
    std::vector<unsigned char> head; // written at LBA 0: protective MBR, header, entries
    std::vector<unsigned char> tail; // written at tail_lba: backup entries, backup header
    uint64_t tail_lba = 0;

    uint64_t part_offset() const { return part_first * sector; }
    uint64_t part_bytes() const { return (part_last - part_first + 1) * sector; }
};

// `seed` 0 = random GUIDs; anything else makes the layout reproducible (tests).
GptLayout make_gpt(uint64_t disk_bytes, uint32_t sector, const std::string& partition_name, uint64_t seed = 0);

struct GptPartition {
    std::string name;  // ASCII
    uint64_t first = 0, last = 0;  // LBA, inclusive
    uint64_t attributes = 0;
};
// Basic data partitions at the caller's places (inside the usable area, in
// order, not overlapping; throws otherwise).
GptLayout make_gpt(uint64_t disk_bytes, uint32_t sector, const std::vector<GptPartition>& parts, uint64_t seed = 0);

// The usable LBAs of a GPT disk of that size: [first, last].
uint64_t gpt_first_usable(uint32_t sector);
uint64_t gpt_last_usable(uint64_t disk_bytes, uint32_t sector);

uint32_t crc32(const void* data, size_t n);

}  // namespace kiln
