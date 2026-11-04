// image.hpp — what kind of image a file is, and whether a raw write of it boots.
//
// A raw (dd-style) write only boots when the image carries its own partition
// table: hybrid ISOs (most Linux distributions) and disk images. A plain ISO —
// every Windows ISO among them — is a CD layout with an empty system area, so
// written raw it is a stick no firmware will boot. Those need a stick BUILT
// from their files (the Windows mode, phase 2).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace kiln {

enum class ImageKind {
    hybrid_iso,  // ISO 9660 + MBR/GPT in the system area: raw write boots
    plain_iso,   // ISO 9660/UDF with no partition table: raw write will not boot
    disk_image,  // MBR/GPT, no ISO 9660 (.img, Raspberry Pi images, ...)
    unknown,     // none of the above (a bare filesystem, compressed, garbage)
};

struct ImageInfo {
    uint64_t size_bytes = 0;
    bool iso9660 = false;
    bool udf = false;
    bool mbr_signature = false;  // 0x55AA at byte 510
    bool gpt = false;            // "EFI PART" at LBA 1 (512- or 4096-byte sectors)
    std::string volume_label;    // ISO 9660 primary volume descriptor
    ImageKind kind = ImageKind::unknown;
};

ImageInfo inspect_image(const std::filesystem::path& path);

const char* kind_name(ImageKind kind);
bool raw_write_boots(ImageKind kind);

}  // namespace kiln
