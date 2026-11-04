// check.hpp — the probes' tiny check harness: prints every check, exits 1 on any failure.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace probe {

inline int failures = 0;
inline int passes = 0;

inline void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? passes : failures)++;
}

// An exFAT volume's cluster size in bytes, from its boot sector (BytesPerSectorShift
// at 0x6C, SectorsPerClusterShift at 0x6D), or 0 if the file is not exFAT. Used
// beside fsck.exfat's exit status: exfat-utils and exfatprogs word their reports
// differently, and only exfatprogs prints the cluster size (with -v).
inline uint64_t exfat_cluster_bytes(const std::filesystem::path& volume, uint64_t offset = 0) {
    unsigned char bs[512] = {};
    std::ifstream f(volume, std::ios::binary);  // a path, not c_str(): wchar_t on Windows
    f.seekg(static_cast<std::streamoff>(offset));
    const bool got = static_cast<bool>(f.read(reinterpret_cast<char*>(bs), sizeof bs));
    if (!got || std::memcmp(bs + 3, "EXFAT   ", 8) != 0 || bs[0x6C] > 12 || bs[0x6D] > 25) return 0;
    return uint64_t{1} << (bs[0x6C] + bs[0x6D]);
}

inline int finish(const char* name) {
    std::printf("%s: %d passed, %d failed\n", name, passes, failures);
    return failures ? 1 : 0;
}

// A fresh directory under the system temp dir, removed on destruction.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() / ("kiln-probe-" + std::to_string(rd()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace probe
