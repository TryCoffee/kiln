// writer_probe.cpp — raw write + verify against regular-file targets.
//
// Runs every case twice: in the system temp dir (often tmpfs: no O_DIRECT, the
// buffered path) and in the current directory (the build tree on a disk
// filesystem: the O_DIRECT path with 4096-byte padding). Block-device
// specifics (O_EXCL, BLKGETSIZE64, BLKRRPART) need a real drive and root.
#include "check.hpp"
#include "sha256.hpp"
#include "writer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace kiln;
using probe::check;

namespace {

using Bytes = std::vector<unsigned char>;

Bytes random_bytes(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    Bytes b(n);
    for (auto& c : b) c = static_cast<unsigned char>(rng());
    return b;
}

void save(const fs::path& p, const Bytes& b) {
    std::ofstream(p, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

Bytes load(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

std::string sha_of(const Bytes& b) {
    Sha256 h;
    h.update(b.data(), b.size());
    return h.hex();
}

bool all_equal(const Bytes& b, size_t from, size_t to, unsigned char v) {
    return std::all_of(b.begin() + static_cast<long>(from), b.begin() + static_cast<long>(to),
                       [v](unsigned char c) { return c == v; });
}

void run_cases(const fs::path& dir, const std::string& where) {
    const size_t mib = 1 << 20;
    const size_t size = 9 * mib + 123;  // spans chunks, not block-aligned
    const Bytes image = random_bytes(size, 42);
    const fs::path img = dir / "image.iso";
    save(img, image);
    const std::string tag = " [" + where + "]";

    {  // New file: becomes exactly the image.
        const fs::path out = dir / "new.img";
        std::vector<Stage> stages;
        uint64_t last_write = 0, last_verify = 0;
        WriteOptions o;
        o.progress = [&](const Progress& p) {
            if (stages.empty() || stages.back() != p.stage) stages.push_back(p.stage);
            if (p.stage == Stage::writing) last_write = p.done;
            if (p.stage == Stage::verifying) last_verify = p.done;
        };
        const WriteResult r = write_image(img, out, o);
        check(r.ok && !r.cancelled && !r.mismatch_at, "new file: write + verify ok" + tag);
        check(load(out) == image, "new file: byte-identical, exact size" + tag);
        check(r.sha256 == sha_of(image), "new file: SHA-256 computed on the way" + tag);
        check(last_write == size && last_verify == size, "progress reaches the total for write and verify" + tag);
        check((stages == std::vector<Stage>{Stage::writing, Stage::syncing, Stage::verifying}),
              "stages in order: writing, syncing, verifying" + tag);
    }
    {  // Existing bigger file = a "drive": image in front, middle untouched, last MiB zeroed.
        const fs::path out = dir / "drive.img";
        save(out, Bytes(32 * mib, 0xEE));
        const WriteResult r = write_image(img, out, {});
        const Bytes got = load(out);
        const size_t padded = (size + 4095) / 4096 * 4096;
        check(r.ok && got.size() == 32 * mib, "drive file: size kept" + tag);
        check(std::equal(image.begin(), image.end(), got.begin()), "drive file: image at the front" + tag);
        check(all_equal(got, padded, 31 * mib, 0xEE), "drive file: between image and tail untouched" + tag);
        check(all_equal(got, 31 * mib, 32 * mib, 0x00), "drive file: last MiB wiped" + tag);
    }
    {  // Same, without the tail wipe.
        const fs::path out = dir / "drive2.img";
        save(out, Bytes(32 * mib, 0xEE));
        WriteOptions o;
        o.wipe_tail = false;
        const WriteResult r = write_image(img, out, o);
        const Bytes got = load(out);
        check(r.ok && all_equal(got, 31 * mib, 32 * mib, 0xEE), "wipe_tail=false leaves the tail alone" + tag);
    }
    {  // Existing smaller file: becomes exactly the image.
        const fs::path out = dir / "small.img";
        save(out, Bytes(4 * mib, 0xEE));
        const WriteResult r = write_image(img, out, {});
        check(r.ok && load(out) == image, "smaller file: replaced by exactly the image" + tag);
    }
    {  // Wrong expected SHA-256: refused before anything is written.
        const fs::path out = dir / "never.img";
        WriteOptions o;
        o.expected_sha256 = std::string(64, '0');
        bool threw = false;
        try {
            write_image(img, out, o);
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("nothing was written") != std::string::npos;
        }
        check(threw && !fs::exists(out), "wrong SHA-256: refused, target never created" + tag);
    }
    {  // Right expected SHA-256 in upper case.
        const fs::path out = dir / "sha.img";
        WriteOptions o;
        std::string upper = sha_of(image);
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        o.expected_sha256 = "  " + upper + "\n";
        const WriteResult r = write_image(img, out, o);
        check(r.ok && load(out) == image, "matching SHA-256 (any case, blanks) accepted" + tag);
    }
    {  // Verify finds the exact corrupted byte, and a short target.
        const fs::path out = dir / "corrupt.img";
        write_image(img, out, {});
        Bytes got = load(out);
        got[5000000] ^= 0x01;
        save(out, got);
        WriteResult r = verify_image(img, out);
        check(!r.ok && r.mismatch_at && *r.mismatch_at == 5000000, "verify: first differing byte reported" + tag);
        got.resize(7 * mib + 5);
        got[5000000] ^= 0x01;
        save(out, got);
        r = verify_image(img, out);
        check(!r.ok && r.mismatch_at && *r.mismatch_at == 7 * mib + 5, "verify: short target fails at its end" + tag);
    }
    {  // Cancel after the first chunk.
        const fs::path out = dir / "cancel.img";
        std::atomic<bool> cancel{false};
        WriteOptions o;
        o.cancel = &cancel;
        o.progress = [&](const Progress& p) {
            if (p.stage == Stage::writing && p.done > 0) cancel = true;
        };
        const WriteResult r = write_image(img, out, o);
        check(r.cancelled && !r.ok, "cancel stops the write" + tag);
        check(fs::file_size(out) < size, "cancelled write left only part of the image" + tag);
    }
    {  // The image as its own target.
        bool threw = false;
        try {
            write_image(img, img, {});
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw && load(img) == image, "image as its own target: refused, image intact" + tag);
    }
    {  // Empty image.
        const fs::path empty = dir / "empty.iso";
        save(empty, {});
        bool threw = false;
        try {
            write_image(empty, dir / "e.img", {});
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "empty image refused" + tag);
    }
}

}  // namespace

int main() {
    // SHA-256 against FIPS 180-4 / NIST CAVS test vectors.
    auto sha = [](const std::string& s) {
        Sha256 h;
        h.update(s.data(), s.size());
        return h.hex();
    };
    check(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of ''");
    check(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of 'abc'");
    check(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256, two-block message");
    check(sha(std::string(55, 'x')).size() == 64 && sha(std::string(56, 'x')) != sha(std::string(55, 'x')),
          "padding edge (55 vs 56 bytes)");
    {
        Sha256 h;  // a million 'a', fed in uneven pieces to exercise the buffering
        const std::string chunk(997, 'a');
        size_t left = 1000000;
        while (left) {
            const size_t n = std::min(left, chunk.size());
            h.update(chunk.data(), n);
            left -= n;
        }
        check(h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 of a million 'a'");
    }
    {
        probe::TempDir tmp;
        run_cases(tmp.path(), "temp dir");
    }
    {
        const fs::path here = fs::current_path() / "writer-probe-work";
        fs::remove_all(here);
        fs::create_directories(here);
        run_cases(here, "build dir");
        fs::remove_all(here);
    }
    return probe::finish("writer_probe");
}
