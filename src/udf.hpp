// udf.hpp — reading files out of a UDF image (every Windows ISO) through libcdio.
//
// libcdio reads at most 512 blocks (1 MiB) per call and keeps a read position
// inside each open file, so File keeps a 1 MiB window and is not thread-safe.
// Multi-extent files (install.wim is 7+ GB) read correctly: checked 2026-10-18
// against 7-Zip's extraction of the Win11 25H2 ISO, identical SHA-256.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct udf_s;
struct udf_dirent_s;

namespace kiln {

struct UdfEntry {
    std::string path;  // "sources/install.wim" (no leading slash)
    bool is_dir = false;
    uint64_t size = 0;
};

class UdfImage {
public:
    explicit UdfImage(const std::filesystem::path& iso);  // throws when not UDF
    ~UdfImage();
    UdfImage(const UdfImage&) = delete;
    UdfImage& operator=(const UdfImage&) = delete;

    std::string volume_id() const;
    // Whole tree, each directory listed before its contents.
    std::vector<UdfEntry> list() const;

    class File {
    public:
        ~File();
        File(const File&) = delete;
        File& operator=(const File&) = delete;
        uint64_t size() const { return size_; }
        // Exactly n bytes at `offset`; throws on a short read or past the end.
        void read_at(void* buf, size_t n, uint64_t offset);

    private:
        friend class UdfImage;
        File(udf_dirent_s* f, uint64_t size, std::string path);
        udf_dirent_s* f_;
        uint64_t size_;
        std::string path_;
        std::vector<unsigned char> window_;
        uint64_t window_off_ = UINT64_MAX;
        size_t window_len_ = 0;
    };

    std::unique_ptr<File> open(const std::string& path) const;  // throws when missing
    bool exists(const std::string& path) const;

private:
    udf_s* udf_;
    std::string iso_;
};

}  // namespace kiln
