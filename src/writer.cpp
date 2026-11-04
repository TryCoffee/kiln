// writer.cpp — raw write + read-back.
#include "writer.hpp"

#include "blockio.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace kiln {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr size_t kChunk = 4u << 20;
constexpr uint64_t kTailWipe = 1u << 20;

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

struct Source {
    Fd fd;
    uint64_t size = 0;
};

Source open_source(const fs::path& image) {
    std::error_code ec;
    if (!fs::is_regular_file(image, ec)) {
        if (!fs::exists(image, ec)) throw std::runtime_error(image.string() + " does not exist");
        throw std::runtime_error(image.string() + " is not a regular file");
    }
    Source s;
    s.fd = open_file(image, Access::read);
    s.size = file_size(s.fd.get(), image.string());
    if (s.size == 0) throw std::runtime_error(image.string() + " is empty");
    advise_sequential(s.fd.get());
    return s;
}

bool is_cancelled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

// Compares the target's first `size` bytes with the source. Fills mismatch_at,
// cancelled and verify_seconds of `r`.
void verify_into(WriteResult& r, const Source& src, const TargetSpec& target_path,
                 const std::atomic<bool>* cancel, const ProgressFn& progress) {
    const auto start = Clock::now();
    Target t = open_target(target_path, Access::read);
    drop_cached(t);

    AlignedBuffer a(kChunk);
    AlignedBuffer b(kChunk);
    for (uint64_t off = 0; off < src.size;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, src.size - off));
        if (pread_all(src.fd.get(), a.get(), n, off, "the image", false) != n)
            throw std::runtime_error("the image changed size while being read");
        const size_t want = t.align > 1 ? static_cast<size_t>(round_up(n, t.align)) : n;
        const size_t got = pread_all(t.fd.get(), b.get(), want, off, t.name, t.direct);
        const size_t same = std::min(got, n);
        const auto diff = std::mismatch(a.get(), a.get() + same, b.get());
        if (diff.first != a.get() + same || got < n) {
            r.mismatch_at = off + static_cast<uint64_t>(diff.first - a.get());
            break;
        }
        off += n;
        if (progress) progress({Stage::verifying, off, src.size});
        if (is_cancelled(cancel)) {
            r.cancelled = true;
            break;
        }
    }
    r.verify_seconds = seconds_since(start);
}

}  // namespace

WriteResult write_image(const fs::path& image, const TargetSpec& target, const WriteOptions& o) {
    WriteResult r;
    Source src = open_source(image);
    AlignedBuffer buf(kChunk);
    auto report = [&](Stage s, uint64_t done, uint64_t total) {
        if (o.progress) o.progress({s, done, total});
    };

    if (o.expected_sha256) {
        Sha256 h;
        for (uint64_t off = 0; off < src.size;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, src.size - off));
            if (pread_all(src.fd.get(), buf.get(), n, off, image.string(), false) != n)
                throw std::runtime_error("the image changed size while being read");
            h.update(buf.get(), n);
            off += n;
            report(Stage::hashing, off, src.size);
            if (is_cancelled(o.cancel)) {
                r.cancelled = true;
                return r;
            }
        }
        r.sha256 = h.hex();
        if (!same_digest(r.sha256, *o.expected_sha256))
            throw std::runtime_error("the image's SHA-256 is " + r.sha256 + ", not the expected " +
                                     *o.expected_sha256 + ": nothing was written");
    }

    if (same_file(src.fd.get(), target)) throw std::runtime_error("the target is the image itself");

    const auto start = Clock::now();
    bool block = false;
    {
        Target t = open_target(target, Access::write);
        block = t.block;
        r.direct = t.direct;
        const uint64_t padded = round_up(src.size, t.align);
        if (t.block && padded > t.size)
            throw std::runtime_error("the image (" + std::to_string(src.size) + " bytes) does not fit on " +
                                     t.name + " (" + std::to_string(t.size) + " bytes)");
        // A drive, or an existing file big enough to stand in for one.
        const bool fixed = t.block || t.size >= padded;

        if (fixed && o.wipe_tail) {
            const uint64_t end = round_down(t.size, t.align);
            const uint64_t from = std::max(padded, end > kTailWipe ? round_down(end - kTailWipe, t.align) : 0);
            std::memset(buf.get(), 0, kChunk);
            for (uint64_t off = from; off < end;) {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, end - off));
                pwrite_all(t.fd.get(), buf.get(), n, off, t.name);
                off += n;
            }
        }

        Sha256 h;
        const bool hash_now = r.sha256.empty();
        for (uint64_t off = 0; off < src.size;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, src.size - off));
            if (pread_all(src.fd.get(), buf.get(), n, off, image.string(), false) != n)
                throw std::runtime_error("the image changed size while being read");
            if (hash_now) h.update(buf.get(), n);
            size_t wn = n;
            if (t.align > 1) {
                wn = static_cast<size_t>(round_up(n, t.align));
                std::memset(buf.get() + n, 0, wn - n);
            }
            pwrite_all(t.fd.get(), buf.get(), wn, off, t.name);
            off += n;
            report(Stage::writing, off, src.size);
            if (is_cancelled(o.cancel)) {
                try {
                    sync_fd(t.fd.get(), t.name);
                } catch (const std::exception&) {
                }
                r.cancelled = true;
                r.write_seconds = seconds_since(start);
                return r;
            }
        }

        report(Stage::syncing, 0, 1);
        sync_fd(t.fd.get(), t.name);
        if (!fixed && padded != src.size) truncate_file(t.fd.get(), src.size, t.name);
        report(Stage::syncing, 1, 1);
        if (hash_now) r.sha256 = h.hex();
    }
    r.write_seconds = seconds_since(start);

    if (o.verify) verify_into(r, src, target, o.cancel, o.progress);
    if (block) r.replug = !reread_partition_table(target);
    r.ok = !r.cancelled && !r.mismatch_at;
    return r;
}

WriteResult verify_image(const fs::path& image, const TargetSpec& target, const std::atomic<bool>* cancel,
                         const ProgressFn& progress) {
    WriteResult r;
    Source src = open_source(image);
    verify_into(r, src, target, cancel, progress);
    r.ok = !r.cancelled && !r.mismatch_at;
    return r;
}

}  // namespace kiln
