#pragma once

#include <stddef.h>
#include <stdint.h>
#include <uapi/status.h>

namespace myos::files::fat32 {

inline constexpr size_t SectorSize = 512;
inline constexpr size_t MaxFiles = 128;
// Mount memory is bounded independently of hostile on-disk geometry.
inline constexpr uint32_t MaxClusters = 1U << 20;

inline auto u16(const uint8_t* p) noexcept -> uint16_t {
    return static_cast<uint16_t>(p[0] | (uint16_t{p[1]} << 8));
}
inline auto u32(const uint8_t* p) noexcept -> uint32_t {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

struct Geometry final {
    uint64_t fat_offset{};
    uint64_t data_offset{};
    uint32_t clusters{};
    uint32_t cluster_bytes{};
    uint32_t root{};

    [[nodiscard]] static auto parse(const uint8_t (&boot)[SectorSize], uint64_t capacity,
        Geometry& result) noexcept -> bool {
        const uint32_t reserved = u16(boot + 14);
        const uint32_t copies = boot[16];
        const uint32_t sectors = u32(boot + 32);
        const uint32_t fat_sectors = u32(boot + 36);
        const uint32_t flags = u16(boot + 40);
        const uint32_t per_cluster = boot[13];
        const uint32_t active = flags & 0x80 ? flags & 15 : 0;
        const uint64_t data_sector = reserved + uint64_t{copies} * fat_sectors;
        if (u16(boot + 510) != 0xaa55 || u16(boot + 11) != SectorSize
            || reserved == 0 || copies == 0 || copies > 2 || fat_sectors == 0
            || u16(boot + 17) != 0 || u16(boot + 19) != 0 || u16(boot + 22) != 0
            || u16(boot + 42) != 0 || (flags & ~0x8fU) != 0 || active >= copies
            || per_cluster == 0 || per_cluster > 64 || (per_cluster & (per_cluster - 1)) != 0
            || data_sector >= sectors || uint64_t{sectors} * SectorSize > capacity)
            return false;
        const uint32_t clusters = (sectors - data_sector) / per_cluster;
        const uint32_t root = u32(boot + 44);
        if (clusters < 65525 || clusters > MaxClusters
            || uint64_t{clusters + 2} * 4 > uint64_t{fat_sectors} * SectorSize
            || root < 2 || root >= clusters + 2) return false;
        result = {(reserved + uint64_t{active} * fat_sectors) * SectorSize,
            data_sector * SectorSize, clusters, per_cluster * uint32_t{SectorSize}, root};
        return true;
    }
    [[nodiscard]] auto fat_bytes() const noexcept -> size_t {
        return (size_t{clusters + 2} * 4 + SectorSize - 1) & ~(SectorSize - 1);
    }
    [[nodiscard]] auto bitmap_bytes() const noexcept -> size_t { return (clusters + 7) / 8; }
    [[nodiscard]] auto cluster_offset(uint32_t cluster) const noexcept -> uint64_t {
        return data_offset + uint64_t{cluster - 2} * cluster_bytes;
    }
};

struct File final {
    char name[13]{};
    uint32_t size{};
    uint32_t first{}; // index into the mount's immutable cluster array
};

struct Extent final {
    uint64_t offset{}; // sector-aligned backend read
    size_t size{};
    size_t skip{};
    size_t bytes{};   // authorized file bytes within that read
};

// The owner supplies mount-lifetime storage sized from validated geometry.
// Reader reads an exact aligned range or returns an error. Only mount uses it;
// established reads translate through extent() without blocking or FAT walks.
class Volume final {
public:
    template<class Reader>
    [[nodiscard]] auto mount(const Geometry& geometry, uint8_t* fat, uint32_t* index,
        uint8_t* visited, Reader&& read) noexcept -> myos_status_t {
        geometry_ = geometry;
        fat_ = fat;
        index_ = index;
        visited_ = visited;
        count_ = 0;
        used_ = 0;
        for (size_t i = 0; i < geometry_.bitmap_bytes(); ++i) visited_[i] = 0;
        auto status = read(geometry_.fat_offset, geometry_.fat_bytes(), fat_);
        if (status != MYOS_STATUS_OK) return status;
        uint32_t root = geometry_.root;
        // Claim the entire root chain, even the tail after an end marker.
        for (;;) {
            if (!claim(root)) return MYOS_STATUS_BACKING_FAILED;
            const uint32_t next = link(root);
            if (end(next)) break;
            root = next;
        }
        bool finished{};
        root = geometry_.root;
        uint32_t starts[MaxFiles]{};
        do {
            for (size_t offset = 0; offset < geometry_.cluster_bytes && !finished; offset += SectorSize) {
                uint8_t sector[SectorSize]{};
                status = read(geometry_.cluster_offset(root) + offset, SectorSize, sector);
                if (status != MYOS_STATUS_OK) return status;
                for (size_t position = 0; position < SectorSize; position += 32) {
                    const auto* entry = sector + position;
                    if (entry[0] == 0) { finished = true; break; }
                    if (entry[0] == 0xe5 || entry[11] == 0x0f || (entry[11] & 0x18) != 0) continue;
                    if (count_ == MaxFiles) return MYOS_STATUS_NO_MEMORY;
                    auto& file = files_[count_];
                    file = {};
                    if (!name(entry, file.name)) return MYOS_STATUS_BACKING_FAILED;
                    for (size_t i = 0; i < count_; ++i)
                        if (equal(file.name, files_[i].name)) return MYOS_STATUS_BACKING_FAILED;
                    file.size = u32(entry + 28);
                    starts[count_++] = (uint32_t{u16(entry + 20)} << 16) | u16(entry + 26);
                }
            }
            root = link(root);
        } while (!finished && !end(root));
        for (size_t i = 0; i < count_; ++i) {
            auto& file = files_[i];
            file.first = used_;
            const uint32_t needed = (uint64_t{file.size} + geometry_.cluster_bytes - 1) / geometry_.cluster_bytes;
            uint32_t cluster = starts[i];
            if (cluster == 0 && needed == 0) continue;
            uint32_t length{};
            for (;;) {
                if (!claim(cluster)) return MYOS_STATUS_BACKING_FAILED;
                if (length < needed) index_[used_++] = cluster;
                ++length;
                const auto next = link(cluster);
                if (end(next)) break;
                cluster = next;
            }
            if (length < needed) return MYOS_STATUS_BACKING_FAILED;
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto count() const noexcept -> size_t { return count_; }
    [[nodiscard]] auto file(size_t index) const noexcept -> const File& { return files_[index]; }
    [[nodiscard]] auto find(const char* path, size_t size) const noexcept -> size_t {
        if (size != 0 && *path == '/') { ++path; --size; }
        if (size == 0 || size > 12) return count_;
        for (size_t i = 0; i < size; ++i)
            if (path[i] == 0 || path[i] == '/' || path[i] == '\\') return count_;
        for (size_t i = 0; i < count_; ++i) {
            size_t j{};
            while (j < size && j < 12 && upper(path[j]) == files_[i].name[j]) ++j;
            if (j == size && files_[i].name[j] == 0) return i;
        }
        return count_;
    }

    // Caller has resolved the file index and bounded length by its size.
    // Coalesce adjacent clusters, keeping each downstream read within 4 KiB.
    [[nodiscard]] auto extent(size_t file_index, uint64_t offset, size_t length) const noexcept -> Extent {
        const auto& file = files_[file_index];
        if (offset >= file.size || length == 0) return {};
        const size_t ordinal = offset / geometry_.cluster_bytes;
        const size_t within = offset % geometry_.cluster_bytes;
        const auto cluster = index_[file.first + ordinal];
        const uint64_t position = geometry_.cluster_offset(cluster) + within;
        const size_t skip = position % SectorSize;
        size_t available = geometry_.cluster_bytes - within;
        const size_t maximum = 4096 - skip;
        size_t next = ordinal + 1;
        const size_t clusters = (uint64_t{file.size} + geometry_.cluster_bytes - 1) / geometry_.cluster_bytes;
        while (available < maximum && next < clusters
            && index_[file.first + next] == index_[file.first + next - 1] + 1) {
            available += geometry_.cluster_bytes;
            ++next;
        }
        if (available > maximum) available = maximum;
        if (available > length) available = length;
        if (available > file.size - offset) available = file.size - offset;
        return {position - skip, (skip + available + SectorSize - 1) & ~(SectorSize - 1), skip, available};
    }

private:
    static auto upper(char c) noexcept -> char { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }
    static auto equal(const char* a, const char* b) noexcept -> bool {
        while (*a && *a == *b) { ++a; ++b; }
        return *a == *b;
    }
    static auto name(const uint8_t* entry, char (&output)[13]) noexcept -> bool {
        size_t length{};
        for (size_t part = 0; part < 2; ++part) {
            const size_t start = part == 0 ? 0 : 8;
            const size_t limit = part == 0 ? 8 : 11;
            size_t end = limit;
            while (end > start && entry[end - 1] == ' ') --end;
            if (part == 0 && end == start) return false;
            if (part != 0 && end != start) output[length++] = '.';
            for (size_t i = start; i < end; ++i) {
                const auto c = entry[i];
                if (c < 0x21 || c > 0x7e || c == '"' || c == '*' || c == '+' || c == ','
                    || c == '.' || c == '/' || c == ':' || c == ';' || c == '<' || c == '='
                    || c == '>' || c == '?' || c == '[' || c == '\\' || c == ']' || c == '|') return false;
                output[length++] = upper(c);
            }
        }
        output[length] = 0;
        return true;
    }
    [[nodiscard]] auto claim(uint32_t cluster) noexcept -> bool {
        if (cluster < 2 || cluster >= geometry_.clusters + 2) return false;
        const uint32_t bit = cluster - 2;
        auto& byte = visited_[bit / 8];
        const uint8_t mask = 1U << (bit % 8);
        if ((byte & mask) != 0) return false;
        byte |= mask;
        return true;
    }
    [[nodiscard]] auto link(uint32_t cluster) const noexcept -> uint32_t { return u32(fat_ + size_t{cluster} * 4) & 0x0fffffff; }
    static auto end(uint32_t cluster) noexcept -> bool { return cluster >= 0x0ffffff8; }

    Geometry geometry_{};
    uint8_t* fat_{};
    uint32_t* index_{};
    uint8_t* visited_{};
    File files_[MaxFiles]{};
    size_t count_{};
    uint32_t used_{};
};

} // namespace myos::files::fat32
