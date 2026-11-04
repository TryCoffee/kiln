// check.hpp — the probes' tiny check harness: prints every check, exits 1 on any failure.
#pragma once

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

namespace probe {

inline int failures = 0;
inline int passes = 0;

inline void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? passes : failures)++;
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
