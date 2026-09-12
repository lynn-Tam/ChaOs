#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <servers/files/fat32.hpp>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "fat32: line %d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace {
using namespace myos::files::fat32;
void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
struct Fixture {
    std::vector<uint8_t> disk = std::vector<uint8_t>(64 * 1024 * 1024);
    Geometry geometry{};
    Volume volume{};
    std::vector<uint8_t> fat, visited;
    std::vector<uint32_t> index;
    Fixture() {
        auto* boot = disk.data();
        put16(boot + 11, 512); boot[13] = 1; put16(boot + 14, 32); boot[16] = 2;
        put32(boot + 32, disk.size() / 512); put32(boot + 36, 1024);
        put32(boot + 44, 2); put16(boot + 510, 0xaa55);
        CHECK(parse());
        fat.resize(geometry.fat_bytes()); visited.resize(geometry.bitmap_bytes());
        index.resize(geometry.clusters);
        link(2, 0x0fffffff);
        // One non-aligned file crossing a fragmented chain: 3 -> 5 -> 6.
        entry(0, "DATA    BIN", 3, 1301);
        link(3, 5); link(5, 6); link(6, 0x0fffffff);
        for (uint32_t ordinal = 0; ordinal < 3; ++ordinal) {
            const uint32_t cluster = ordinal == 0 ? 3 : ordinal + 4;
            for (size_t i = 0; i < 512; ++i)
                disk[geometry.cluster_offset(cluster) + i] = (ordinal * 512 + i) % 251;
        }
    }
    bool parse() { return Geometry::parse(*reinterpret_cast<const uint8_t (*)[512]>(disk.data()), disk.size(), geometry); }
    void link(uint32_t a, uint32_t b) { put32(disk.data() + geometry.fat_offset + a * 4, b); }
    void entry(size_t i, const char* name, uint32_t cluster, uint32_t size) {
        auto* p = disk.data() + geometry.data_offset + i * 32;
        std::memcpy(p, name, 11); p[11] = 0x20;
        put16(p + 20, cluster >> 16); put16(p + 26, cluster); put32(p + 28, size);
    }
    auto mount() {
        return volume.mount(geometry, fat.data(), index.data(), visited.data(),
            [&](uint64_t offset, size_t size, uint8_t* output) -> myos_status_t {
                CHECK(offset % 512 == 0 && size % 512 == 0);
                CHECK(offset <= disk.size() && size <= disk.size() - offset);
                std::memcpy(output, disk.data() + offset, size);
                return MYOS_STATUS_OK;
            });
    }
};
}

int main() {
    Fixture f;
    CHECK(f.mount() == MYOS_STATUS_OK);
    CHECK(f.volume.count() == 1 && f.volume.file(0).size == 1301);
    CHECK(f.volume.find("/data.bin", 9) == 0);
    CHECK(f.volume.find("data.bin\0", 9) == 1);
    CHECK(f.volume.find("sub/data.bin", 12) == 1);
    // Reconstruct actual content through the same extent translator as the server.
    size_t offset = 17;
    while (offset < 1301) {
        const auto extent = f.volume.extent(0, offset, 4096);
        CHECK(extent.size <= 4096 && extent.bytes != 0);
        for (size_t i = 0; i < extent.bytes; ++i)
            CHECK(f.disk[extent.offset + extent.skip + i] == (offset + i) % 251);
        offset += extent.bytes;
    }
    CHECK(f.volume.extent(0, UINT64_MAX, 1).bytes == 0);
    CHECK(f.volume.extent(0, 1300, 4096).bytes == 1);
    f.link(6, 3); CHECK(f.mount() == MYOS_STATUS_BACKING_FAILED); // cycle
    f.link(6, 0x0ffffff7); CHECK(f.mount() == MYOS_STATUS_BACKING_FAILED); // bad cluster
    f.link(6, f.geometry.clusters + 2); CHECK(f.mount() == MYOS_STATUS_BACKING_FAILED);
    f.link(6, 0x0fffffff);
    f.link(3, 0x0fffffff); CHECK(f.mount() == MYOS_STATUS_BACKING_FAILED); // short chain
    f.link(3, 5);
    f.entry(1, "OTHER   BIN", 5, 512); CHECK(f.mount() == MYOS_STATUS_BACKING_FAILED); // overlap
    f.entry(1, "OTHER   BIN", 2, 512); CHECK(f.mount() == MYOS_STATUS_BACKING_FAILED); // root overlap
    f.entry(1, "DATA    BIN", 7, 512); f.link(7, 0x0fffffff);
    CHECK(f.mount() == MYOS_STATUS_BACKING_FAILED); // ambiguous name
    f.disk[f.geometry.data_offset + 32] = 0;
    f.link(2, 2); CHECK(f.mount() == MYOS_STATUS_BACKING_FAILED);
    f.link(2, 0x0fffffff); CHECK(f.mount() == MYOS_STATUS_OK);
    auto* boot = f.disk.data();
    put32(boot + 36, UINT32_MAX); CHECK(!f.parse());
    put32(boot + 36, 1024); boot[13] = 3; CHECK(!f.parse());
    boot[13] = 1; put32(boot + 32, UINT32_MAX); CHECK(!f.parse());
    put32(boot + 32, 64 * 1024 * 1024 / 512); put32(boot + 44, UINT32_MAX); CHECK(!f.parse());
    put32(boot + 44, 2); put16(boot + 40, 0x82); CHECK(!f.parse());
    put16(boot + 40, 0); CHECK(f.parse());
    std::puts("[fat32] ok: geometry, fragmented byte reads, EOF, cycles, bounds, overlapping chains");
}
