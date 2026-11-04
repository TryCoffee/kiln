// sha256.hpp — incremental SHA-256.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kiln {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::string hex();  // finishes; call once

private:
    void block(const unsigned char* p);
    uint32_t h_[8];
    unsigned char buf_[64] = {};
    size_t fill_ = 0;
    uint64_t bytes_ = 0;
};

// True when `a` and `b` are the same hex digest, ignoring case and surrounding blanks.
bool same_digest(const std::string& a, const std::string& b);

}  // namespace kiln
