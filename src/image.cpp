// image.cpp — reads the first 64 KiB of an image and classifies it.
#include "image.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace kiln {
namespace {

constexpr size_t kIsoSector = 2048;
constexpr size_t kFirstDescriptor = 16 * kIsoSector;  // system area = sectors 0..15
constexpr size_t kHead = 64 * 1024;                   // sectors 16..31 cover every descriptor we look at

bool bytes_at(const std::vector<unsigned char>& b, size_t off, const char* s) {
    const size_t n = std::strlen(s);
    return off + n <= b.size() && std::memcmp(b.data() + off, s, n) == 0;
}

}  // namespace

ImageInfo inspect_image(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    ImageInfo info;
    std::error_code ec;
    info.size_bytes = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot read the size of " + path.string() + ": " + ec.message());

    std::vector<unsigned char> head(kHead);
    f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(f.gcount()));

    info.mbr_signature = head.size() >= 512 && head[510] == 0x55 && head[511] == 0xAA;
    info.gpt = bytes_at(head, 512, "EFI PART") || bytes_at(head, 4096, "EFI PART");

    // Volume descriptors: type byte + "CD001" (ISO 9660) until the terminator
    // (type 255); UDF adds "BEA01", "NSR02"/"NSR03", "TEA01" in the same run.
    for (size_t off = kFirstDescriptor; off + kIsoSector <= head.size(); off += kIsoSector) {
        if (bytes_at(head, off + 1, "CD001")) {
            info.iso9660 = true;
            if (head[off] == 1 && info.volume_label.empty()) {
                std::string label(reinterpret_cast<const char*>(head.data() + off + 40), 32);
                while (!label.empty() && (label.back() == ' ' || label.back() == '\0')) label.pop_back();
                info.volume_label = label;
            }
        } else if (bytes_at(head, off + 1, "NSR02") || bytes_at(head, off + 1, "NSR03")) {
            info.udf = true;
        } else if (!bytes_at(head, off + 1, "BEA01") && !bytes_at(head, off + 1, "TEA01") &&
                   !bytes_at(head, off + 1, "BOOT2")) {
            break;
        }
    }

    const bool has_table = info.mbr_signature || info.gpt;
    if (info.iso9660 || info.udf)
        info.kind = has_table ? ImageKind::hybrid_iso : ImageKind::plain_iso;
    else
        info.kind = has_table ? ImageKind::disk_image : ImageKind::unknown;
    return info;
}

const char* kind_name(ImageKind kind) {
    switch (kind) {
        case ImageKind::hybrid_iso: return "hybrid ISO";
        case ImageKind::plain_iso: return "plain ISO (no partition table)";
        case ImageKind::disk_image: return "disk image";
        case ImageKind::unknown: return "unknown";
    }
    return "unknown";
}

bool raw_write_boots(ImageKind kind) {
    return kind == ImageKind::hybrid_iso || kind == ImageKind::disk_image;
}

}  // namespace kiln
