// udf.cpp
#include "udf.hpp"

#include <cdio/logging.h>
#include <cdio/udf.h>
#include <sys/types.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>

// libcdio is built with a 64-bit off_t (its udf_setpos takes one): ours must be
// the same, or positions past 2 GiB are cut short (MinGW's default is 32 bits).
static_assert(sizeof(off_t) == 8, "build with _FILE_OFFSET_BITS=64");

namespace kiln {
namespace {

constexpr size_t kMaxBlocks = 512;  // libcdio refuses more per udf_read_block()

// A fresh root for each lookup: udf_fopen() leaves it to the caller to free.
struct Root {
    explicit Root(udf_t* u) : d(udf_get_root(u, true, 0)) {
        if (!d) throw std::runtime_error("the UDF image has no root directory");
    }
    ~Root() { udf_dirent_free(d); }
    udf_dirent_t* d;
};

// libcdio logs a warning to stderr every time it shortens a read to the end of
// an extent, which read_at() expects and handles; keep only its errors.
udf_t* open_quiet(const std::filesystem::path& iso) {
    cdio_loglevel_default = CDIO_LOG_ERROR;
    return udf_open(iso.string().c_str());  // Windows: UTF-8, the process code page (manifest)
}

}  // namespace

UdfImage::UdfImage(const std::filesystem::path& iso) : udf_(open_quiet(iso)), iso_(iso.string()) {
    if (!udf_) throw std::runtime_error(iso_ + " is not a UDF image (or cannot be read)");
}

UdfImage::~UdfImage() { udf_close(udf_); }

std::string UdfImage::volume_id() const {
    char id[UDF_VOLID_SIZE + 1] = {};
    if (udf_get_volume_id(udf_, id, sizeof id) <= 0) return {};
    return id;
}

std::vector<UdfEntry> UdfImage::list() const {
    std::vector<UdfEntry> out;
    // udf_readdir() frees the dirent when it reaches the end; udf_opendir()
    // returns a new one for each subdirectory, consumed the same way.
    std::function<void(udf_dirent_t*, const std::string&)> walk = [&](udf_dirent_t* d, const std::string& prefix) {
        while ((d = udf_readdir(d)) != nullptr) {
            const char* name = udf_get_filename(d);
            if (!name || !*name) continue;  // the parent entry
            const std::string path = prefix + name;
            if (udf_is_dir(d)) {
                out.push_back({path, true, 0});
                if (udf_dirent_t* sub = udf_opendir(d)) walk(sub, path + "/");
            } else {
                out.push_back({path, false, udf_get_file_length(d)});
            }
        }
    };
    walk(udf_get_root(udf_, true, 0), "");
    return out;
}

bool UdfImage::exists(const std::string& path) const {
    Root root(udf_);
    udf_dirent_t* f = udf_fopen(root.d, ("/" + path).c_str());
    if (!f) return false;
    udf_dirent_free(f);
    return true;
}

std::unique_ptr<UdfImage::File> UdfImage::open(const std::string& path) const {
    Root root(udf_);
    udf_dirent_t* f = udf_fopen(root.d, ("/" + path).c_str());
    if (!f) throw std::runtime_error(path + " is not in " + iso_);
    return std::unique_ptr<File>(new File(f, udf_get_file_length(f), path));
}

UdfImage::File::File(udf_dirent_s* f, uint64_t size, std::string path)
    : f_(f), size_(size), path_(std::move(path)), window_(kMaxBlocks * UDF_BLOCKSIZE) {}

UdfImage::File::~File() { udf_dirent_free(f_); }

void UdfImage::File::read_at(void* buf, size_t n, uint64_t offset) {
    if (offset > size_ || n > size_ - offset)
        throw std::runtime_error("read past the end of " + path_ + " in the image");
    auto out = static_cast<unsigned char*>(buf);
    while (n > 0) {
        if (window_off_ == UINT64_MAX || offset < window_off_ || offset >= window_off_ + window_len_) {
            const uint64_t base = offset / UDF_BLOCKSIZE * UDF_BLOCKSIZE;
            if (!udf_setpos(f_, static_cast<off_t>(base)))
                throw std::runtime_error("cannot seek in " + path_ + " to byte " + std::to_string(base));
            const uint64_t blocks_left = (size_ - base + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE;
            const size_t blocks = static_cast<size_t>(std::min<uint64_t>(kMaxBlocks, blocks_left));
            const ssize_t r = udf_read_block(f_, window_.data(), blocks);
            if (r <= 0) throw std::runtime_error("cannot read " + path_ + " at byte " + std::to_string(base));
            window_off_ = base;
            window_len_ = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(r), size_ - base));
            if (window_len_ == 0) throw std::runtime_error("short read in " + path_);
        }
        const size_t at = static_cast<size_t>(offset - window_off_);
        const size_t take = std::min(n, window_len_ - at);
        std::memcpy(out, window_.data() + at, take);
        out += take;
        offset += take;
        n -= take;
    }
}

}  // namespace kiln
