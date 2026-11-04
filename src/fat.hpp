// fat.hpp — FAT32 and exFAT filesystems built straight onto a region of a
// drive, with FatFs, so nothing is ever mounted: the same code on Linux,
// Windows and macOS. A FAT image held in memory can be mounted too.
//
// FatFs keeps global state (its disk I/O layer is free functions), so only one
// FatVolume may exist at a time; the constructor enforces it.
#pragma once

#include "blockio.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace kiln {

enum class FatType { fat32, exfat };

class FatVolume {
public:
    // Formats [offset, offset + length) of `t` with no partition table inside
    // (the caller wrote it); the data area is aligned to 1 MiB. `cluster_bytes`
    // 0: FatFs picks the cluster size.
    static void format(Target& t, uint64_t offset, uint64_t length, const std::string& label,
                       FatType type = FatType::fat32, uint32_t cluster_bytes = 0);

    // Mounts the FAT volume (FAT12/16/32 or exFAT) at [offset, offset + length) of `t`.
    FatVolume(Target& t, uint64_t offset, uint64_t length);
    // Mounts the FAT image of `bytes` bytes at `image` (512-byte sectors); changes
    // are made in that memory.
    FatVolume(unsigned char* image, size_t bytes);
    ~FatVolume();
    FatVolume(const FatVolume&) = delete;
    FatVolume& operator=(const FatVolume&) = delete;

    void make_dir(const std::string& path);
    bool exists(const std::string& path);
    // Deletes the file; false when there was none.
    bool remove(const std::string& path);

    // Fills `buf` with exactly n bytes of the file at `offset`.
    using ReadFn = std::function<void(void* buf, size_t n, uint64_t offset)>;
    // Called after each chunk with the bytes just written (progress, cancel by throwing).
    using ChunkFn = std::function<void(size_t)>;
    // Creates `path` (must not exist) from `size` bytes of `source`, allocated contiguously.
    void write_file(const std::string& path, uint64_t size, const ReadFn& source, const ChunkFn& on_chunk = {});

    // Streams the file to `sink` in order and returns its size.
    using SinkFn = std::function<void(const unsigned char* data, size_t n)>;
    uint64_t read_file(const std::string& path, const SinkFn& sink);

    std::string label();
    bool exfat() const;
    uint64_t free_bytes();

    // Flushes FatFs's buffers and the device.
    void sync();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kiln
