#include <boot/firmware/devicetree/fdt.hpp>

namespace kernel::boot::fdt {

bool init_view(FDT_View& out, const void* dtb) {
    if (dtb == nullptr) {
        return false;
    }

    ByteReader header{static_cast<const uint8_t*>(dtb), 40};
    uint32_t header_magic{};
    uint32_t total_size{};
    uint32_t structure_offset{};
    uint32_t strings_offset{};
    uint32_t reservations_offset{};
    uint32_t version{};
    uint32_t last_compatible_version{};
    uint32_t boot_cpu_id{};
    uint32_t strings_size{};
    uint32_t structure_size{};
    if (!header.read_be32(header_magic)
        || !header.read_be32(total_size)
        || !header.read_be32(structure_offset)
        || !header.read_be32(strings_offset)
        || !header.read_be32(reservations_offset)
        || !header.read_be32(version)
        || !header.read_be32(last_compatible_version)
        || !header.read_be32(boot_cpu_id)
        || !header.read_be32(strings_size)
        || !header.read_be32(structure_size)) {
        return false;
    }
    (void)version;
    (void)last_compatible_version;
    (void)boot_cpu_id;

    auto within = [total_size](uint32_t offset, uint32_t size) {
        return offset <= total_size && size <= total_size - offset;
    };
    if (header_magic != magic
        || total_size < 40
        || structure_size < sizeof(uint32_t)
        || !within(structure_offset, structure_size)
        || !within(strings_offset, strings_size)
        || reservations_offset % alignof(uint64_t) != 0
        || !within(reservations_offset, 2 * sizeof(uint64_t))) {
        return false;
    }

    uint32_t reservations_end = total_size;
    if (structure_offset > reservations_offset && structure_offset < reservations_end) {
        reservations_end = structure_offset;
    }
    if (strings_offset > reservations_offset && strings_offset < reservations_end) {
        reservations_end = strings_offset;
    }
    if (reservations_end - reservations_offset < 2 * sizeof(uint64_t)) {
        return false;
    }

    const auto* base = static_cast<const uint8_t*>(dtb);
    out = FDT_View{
        .base = base,
        .size = total_size,
        .dt_struct = base + structure_offset,
        .dt_struct_size = structure_size,
        .dt_strings = reinterpret_cast<const char*>(base + strings_offset),
        .dt_strings_size = strings_size,
        .mem_rsvmap = base + reservations_offset,
        .mem_rsvmap_size = reservations_end - reservations_offset,
    };
    return true;
}

} // namespace kernel::boot::fdt
