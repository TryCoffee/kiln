// writer.hpp — raw image write with read-back verification.
//
// Targets: a block device, or a regular file. An existing file at least as big
// as the image is treated like a drive (fixed size, tail wiped, nothing
// truncated) so a pre-sized "virtual stick" can be written and then booted in
// QEMU; a new or smaller file becomes exactly the image.
//
// Writes go around the page cache (O_DIRECT) wherever the target allows it, so
// the read-back reads the medium, not memory. Errors throw; the outcomes a
// caller has to act on (cancelled, verify mismatch) come back in the result.
#pragma once

#include "blockio.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace kiln {

enum class Stage { hashing, writing, syncing, verifying, splitting, customizing, partitioning, copying };

struct Progress {
    Stage stage = Stage::writing;
    uint64_t done = 0;
    uint64_t total = 0;
};

using ProgressFn = std::function<void(const Progress&)>;

struct WriteOptions {
    bool verify = true;
    // Zero the drive's last MiB first: a stale backup GPT from the stick's
    // previous life otherwise makes firmware and partitioners "repair" the new
    // layout from it.
    bool wipe_tail = true;
    // Checked over the whole image BEFORE anything is written.
    std::optional<std::string> expected_sha256;
    const std::atomic<bool>* cancel = nullptr;
    ProgressFn progress;
};

struct WriteResult {
    bool ok = false;
    bool cancelled = false;
    std::optional<uint64_t> mismatch_at;  // first differing byte, when verify failed
    std::string sha256;                   // of the image, computed on the way
    bool direct = false;                  // the write bypassed the page cache (O_DIRECT)
    bool replug = false;                  // the OS did not take the new partition table: replug the drive
    double write_seconds = 0;
    double verify_seconds = 0;
};

WriteResult write_image(const std::filesystem::path& image, const TargetSpec& target,
                        const WriteOptions& options);

// Read-back check on its own (`kiln verify`): same rules as after a write.
WriteResult verify_image(const std::filesystem::path& image, const TargetSpec& target,
                         const std::atomic<bool>* cancel = nullptr, const ProgressFn& progress = {});

}  // namespace kiln
