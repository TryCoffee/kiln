// gpt_probe.cpp — the GPT builder, checked by its own CRCs and by sfdisk (util-linux).
//
// sfdisk is an independent GPT implementation; when it is missing the probe
// exits 77 (ctest: skipped) instead of passing on the internal checks alone.
#include "check.hpp"
#include "gpt.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace kiln;
using probe::check;

namespace {

uint64_t get64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}
uint32_t get32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24; }

bool header_ok(const unsigned char* h) {
    std::vector<unsigned char> c(h, h + 92);
    std::memset(c.data() + 16, 0, 4);
    return std::memcmp(h, "EFI PART", 8) == 0 && get32(h + 16) == crc32(c.data(), c.size());
}

std::string run(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, p)) > 0;) out.append(buf, n);
        if (pclose(p) != 0) out = "EXIT-FAILED " + out;
    }
    return out;
}

}  // namespace

int main() {
    check(crc32("123456789", 9) == 0xCBF43926u, "CRC-32 check value");

    const uint64_t disk = 16ull << 30;
    for (uint32_t sector : {512u, 4096u}) {
        const std::string tag = " [" + std::to_string(sector) + " B sectors]";
        const GptLayout g = make_gpt(disk, sector, "Kiln", 42);
        const uint64_t entry_sectors = 16384 / sector;
        check(g.sectors == disk / sector, "sector count" + tag);
        check(g.part_offset() == 1u << 20, "partition starts at 1 MiB" + tag);
        check((g.part_last + 1) * sector % (1u << 20) == 0, "partition ends on a MiB boundary" + tag);
        check(g.part_last <= g.sectors - 2 - entry_sectors, "partition ends before the backup table" + tag);
        check(g.head.size() == (2 + entry_sectors) * sector, "head = MBR + header + entries" + tag);
        check(g.head[510] == 0x55 && g.head[511] == 0xAA && g.head[446 + 4] == 0xEE, "protective MBR" + tag);
        const unsigned char* h = g.head.data() + sector;
        const unsigned char* b = g.tail.data() + entry_sectors * sector;
        check(header_ok(h) && header_ok(b), "both header CRCs valid" + tag);
        check(get64(h + 24) == 1 && get64(h + 32) == g.sectors - 1, "primary: my LBA 1, alternate = last" + tag);
        check(get64(b + 24) == g.sectors - 1 && get64(b + 32) == 1, "backup: my LBA last, alternate = 1" + tag);
        check(get64(b + 72) == g.tail_lba && g.tail_lba == g.sectors - 1 - entry_sectors,
              "backup entries just before the backup header" + tag);
        check(get32(h + 88) == crc32(g.head.data() + 2 * sector, 16384) &&
                  get32(b + 88) == crc32(g.tail.data(), 16384),
              "entry-array CRCs valid" + tag);
        const unsigned char* e = g.head.data() + 2 * sector;
        check(get64(e + 32) == g.part_first && get64(e + 40) == g.part_last, "entry 0 = the partition" + tag);
        check(e[0] == 0xA2 && e[3] == 0xEB && e[15] == 0xC7, "type = Microsoft basic data" + tag);
        check(e[56] == 'K' && e[57] == 0 && e[62] == 'n', "name 'Kiln' in UTF-16LE" + tag);
    }

    const GptLayout a = make_gpt(disk, 512, "x", 0), b = make_gpt(disk, 512, "x", 0);
    check(std::memcmp(a.head.data() + 512 + 56, b.head.data() + 512 + 56, 16) != 0, "random disk GUIDs differ");

    // Independent reader: sfdisk on an image file.
    if (std::system("command -v sfdisk >/dev/null 2>&1") != 0) {
        std::printf("SKIP  sfdisk not installed: GPT not checked by an independent tool\n");
        probe::finish("gpt_probe");
        return probe::failures ? 1 : 77;
    }
    probe::TempDir tmp;
    const fs::path img = tmp.path() / "disk.img";
    const GptLayout g = make_gpt(disk, 512, "Kiln", 7);
    {
        std::ofstream f(img, std::ios::binary);
        f.write(reinterpret_cast<const char*>(g.head.data()), static_cast<std::streamsize>(g.head.size()));
        f.seekp(static_cast<std::streamoff>(g.tail_lba * 512));
        f.write(reinterpret_cast<const char*>(g.tail.data()), static_cast<std::streamsize>(g.tail.size()));
    }
    fs::resize_file(img, disk);
    const std::string verify = run("LC_ALL=C sfdisk --verify " + img.string() + " 2>&1");
    check(verify.find("EXIT-FAILED") == std::string::npos && verify.find("No errors detected") != std::string::npos,
          "sfdisk --verify: no errors (" + verify.substr(0, verify.find('\n')) + ")");
    const std::string dump = run("LC_ALL=C sfdisk --dump " + img.string() + " 2>&1");
    check(dump.find("label: gpt") != std::string::npos, "sfdisk sees a GPT");
    check(dump.find("start=        2048") != std::string::npos, "sfdisk: partition at sector 2048");
    check(dump.find("type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7") != std::string::npos, "sfdisk: basic data type");
    check(dump.find("name=\"Kiln\"") != std::string::npos, "sfdisk: partition name");

    // Two placed partitions, the second with an attribute bit (Ventoy's VTOYEFI).
    const uint64_t last = gpt_last_usable(disk, 512);
    check(gpt_first_usable(512) == 34 && last == disk / 512 - 34, "usable area: LBA 34 to N-34");
    const GptLayout two = make_gpt(disk, 512, {{"Ventoy", 2048, last - 65536 - 7, 0}, {"VTOYEFI", last - 65535 - 7, last - 7, 1ull << 63}}, 9);
    const unsigned char* e1 = two.head.data() + 2 * 512 + 128;
    check(get64(e1 + 32) == last - 65535 - 7 && get64(e1 + 48) == 1ull << 63, "entry 1: placed, attribute bit 63");
    check(header_ok(two.head.data() + 512) && get32(two.head.data() + 512 + 88) == crc32(two.head.data() + 1024, 16384),
          "two partitions: header and entry CRCs valid");
    {
        std::fstream f(img, std::ios::binary | std::ios::in | std::ios::out);
        f.write(reinterpret_cast<const char*>(two.head.data()), static_cast<std::streamsize>(two.head.size()));
        f.seekp(static_cast<std::streamoff>(two.tail_lba * 512));
        f.write(reinterpret_cast<const char*>(two.tail.data()), static_cast<std::streamsize>(two.tail.size()));
    }
    const std::string verify2 = run("LC_ALL=C sfdisk --verify " + img.string() + " 2>&1");
    check(verify2.find("No errors detected") != std::string::npos, "sfdisk --verify: two partitions, no errors");
    const std::string dump2 = run("LC_ALL=C sfdisk --dump " + img.string() + " 2>&1");
    check(dump2.find("name=\"VTOYEFI\", attrs=\"GUID:63\"") != std::string::npos, "sfdisk: VTOYEFI with attrs GUID:63");
    for (const auto& bad : std::vector<std::vector<GptPartition>>{
             {{"low", 33, 4096, 0}}, {{"high", 2048, last + 1, 0}}, {{"a", 2048, 9000, 0}, {"b", 9000, 10000, 0}},
             {{"backwards", 5000, 4000, 0}}, {}}) {
        bool threw = false;
        try {
            make_gpt(disk, 512, bad, 1);
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "refused: partitions out of place (" + std::string(bad.empty() ? "none" : bad[0].name) + ")");
    }
    return probe::finish("gpt_probe");
}
