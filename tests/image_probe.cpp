// image_probe.cpp — image classification on synthetic images.
#include "check.hpp"
#include "image.hpp"

#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace kiln;
using probe::check;

namespace {

using Bytes = std::vector<unsigned char>;

void at(Bytes& b, size_t off, const std::string& s) { std::memcpy(b.data() + off, s.data(), s.size()); }

void descriptor(Bytes& b, int sector, unsigned char type, const char* id) {
    const size_t off = static_cast<size_t>(sector) * 2048;
    b[off] = type;
    at(b, off + 1, id);
    b[off + 6] = 1;  // version
}

void pvd(Bytes& b, int sector, const std::string& label) {
    descriptor(b, sector, 1, "CD001");
    std::string padded = label;
    padded.resize(32, ' ');
    at(b, static_cast<size_t>(sector) * 2048 + 40, padded);
}

void mbr(Bytes& b) {
    b[510] = 0x55;
    b[511] = 0xAA;
}

fs::path save(const fs::path& dir, const std::string& name, const Bytes& b) {
    const fs::path p = dir / name;
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p;
}

}  // namespace

int main() {
    probe::TempDir tmp;
    const size_t mib = 1 << 20;

    {  // Linux-style hybrid ISO: isohybrid MBR in the system area.
        Bytes b(mib);
        mbr(b);
        pvd(b, 16, "ARCH_202610");
        descriptor(b, 17, 255, "CD001");
        const ImageInfo i = inspect_image(save(tmp.path(), "hybrid.iso", b));
        check(i.kind == ImageKind::hybrid_iso && raw_write_boots(i.kind), "hybrid ISO: raw write boots");
        check(i.iso9660 && !i.udf && i.mbr_signature, "hybrid ISO: ISO 9660 + MBR");
        check(i.volume_label == "ARCH_202610", "hybrid ISO label, padding trimmed");
        check(i.size_bytes == mib, "size");
    }
    {  // Windows-style UDF bridge: empty system area, ISO 9660 + UDF descriptors.
        Bytes b(mib);
        pvd(b, 16, "CCCOMA_X64FRE_EN-US_DV9");
        descriptor(b, 17, 0, "CD001");    // El Torito boot record
        descriptor(b, 18, 255, "CD001");  // terminator
        descriptor(b, 19, 0, "BEA01");
        descriptor(b, 20, 0, "NSR02");
        descriptor(b, 21, 0, "TEA01");
        const ImageInfo i = inspect_image(save(tmp.path(), "windows.iso", b));
        check(i.kind == ImageKind::plain_iso && !raw_write_boots(i.kind), "Windows-style ISO: raw write does NOT boot");
        check(i.iso9660 && i.udf && !i.mbr_signature && !i.gpt, "Windows-style ISO: ISO 9660 + UDF, no table");
        check(i.volume_label == "CCCOMA_X64FRE_EN-US_DV9", "Windows-style ISO label");
    }
    {  // UDF only (no ISO 9660 descriptor at all).
        Bytes b(mib);
        descriptor(b, 16, 0, "BEA01");
        descriptor(b, 17, 0, "NSR03");
        descriptor(b, 18, 0, "TEA01");
        const ImageInfo i = inspect_image(save(tmp.path(), "udf.iso", b));
        check(i.kind == ImageKind::plain_iso && i.udf && !i.iso9660, "UDF-only image is a plain ISO");
    }
    {  // Disk image with an MBR.
        Bytes b(mib);
        mbr(b);
        const ImageInfo i = inspect_image(save(tmp.path(), "rpi.img", b));
        check(i.kind == ImageKind::disk_image && raw_write_boots(i.kind), "MBR disk image boots raw");
    }
    {  // GPT without protective-MBR signature, 512-byte sectors.
        Bytes b(mib);
        at(b, 512, "EFI PART");
        const ImageInfo i = inspect_image(save(tmp.path(), "gpt512.img", b));
        check(i.gpt && i.kind == ImageKind::disk_image, "GPT at LBA 1 (512 B sectors)");
    }
    {  // GPT on 4096-byte sectors.
        Bytes b(mib);
        at(b, 4096, "EFI PART");
        const ImageInfo i = inspect_image(save(tmp.path(), "gpt4k.img", b));
        check(i.gpt && i.kind == ImageKind::disk_image, "GPT at LBA 1 (4096 B sectors)");
    }
    {  // Compressed / junk.
        Bytes b(mib, 0x5A);
        at(b, 0, "\xFD" "7zXZ");
        const ImageInfo i = inspect_image(save(tmp.path(), "image.img.xz", b));
        check(i.kind == ImageKind::unknown && !raw_write_boots(i.kind), "xz-compressed image is unknown, not bootable raw");
    }
    {  // Tiny files must not read past their end.
        const ImageInfo i = inspect_image(save(tmp.path(), "tiny.bin", Bytes(300, 0)));
        check(i.kind == ImageKind::unknown && i.size_bytes == 300, "300-byte file: unknown, no crash");
        const ImageInfo j = inspect_image(save(tmp.path(), "short-iso.bin", Bytes(32768 + 100, 0)));
        check(j.kind == ImageKind::unknown, "file ending inside the first descriptor: unknown");
    }
    {
        bool threw = false;
        try {
            inspect_image(tmp.path() / "missing.iso");
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "missing file throws");
    }
    return probe::finish("image_probe");
}
