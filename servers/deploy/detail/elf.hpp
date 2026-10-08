#pragma once

#include <concepts>
#include <stddef.h>
#include <stdint.h>

#include <libk/checked_arithmetic.hpp>
#include <libk/inplace_vector.hpp>
#include <optional>
#include <utility>
#include <servers/deploy/format.h>
#include <uapi/cap.h>
#include <uapi/abi.h>
#include <uapi/mem.h>
#include <servers/deploy/bundle.hpp>
#include <servers/deploy/format.hpp>

namespace deploy {

// A loader may provide already-backed segment objects. Each returned selector
// is a new owner, scoped to the segment's native object-page range and access.
// first identifies that range's origin; attenuation never rebases a MemoryObject.
// task adopts it before constructing the mapping, including on failure.
struct ElfSrc final {
    void* context{};
    // Only complete file pages are passed here. The caller owns boundary and
    // zero pages; a writable segment maps this read-only source privately.
    sys::SysResult (*create)(void*, const boot::Segment&, word_t& first) noexcept{};
};

template<size_t SegmentCapacity = 32, size_t StackCapacity = 64>
struct ElfMap final {
    struct Mapping final {
        sys::cap::CapRef memory{};
        sys::cap::CapRef region{};
        uintptr_t address{};
        word_t size{};
        word_t access{};
        word_t first{};
    };

    struct Stack final {
        Mapping mapping{};
        word_t top{};
    };

    libk::InplaceVector<Mapping, SegmentCapacity> segments{};
    libk::InplaceVector<Stack, StackCapacity> stacks{};
    uintptr_t entry{};

    void clear() noexcept {
        segments.clear();
        stacks.clear();
        entry = 0;
    }
};

template<typename B>
concept MaterializerBackend = requires(
        sys::cap::CapRef pool,
        sys::cap::CapRef memory,
        void* destination,
        const uint8_t* source,
        word_t size,
        word_t access) {
    { B::memory_create(pool, size, access) } -> std::same_as<sys::SysResult>;
    { B::memory_seal(memory) } -> std::same_as<status_t>;
    { B::memory_populate(memory, size) } -> std::same_as<status_t>;
    { B::memory_write(destination, source, size) }
        -> std::same_as<status_t>;
};

/*
 * ElfLoader maps one complete destination object through
 * Map for each populate/write operation.  Derive that
 * single-window requirement from the selected, already decoded task and the
 * same BootBundle consumed by ElfLoader.  This query owns no mapping,
 * capability, or deployment policy; Map remains the sole lease
 * owner.
 */
[[nodiscard]] inline auto required_scratch_size(
    const TaskSpec& task,
    const boot::Bundle& bundle) noexcept -> std::optional<word_t> {
    const auto row = task.row();
    constexpr word_t page_size = DEPLOY_PAGE_SIZE;
    constexpr word_t max_word = ~word_t{};
    if (!row || !bundle || page_size == 0
        || page_size > max_word
        || row->image_count > DEPLOY_TASK_IMAGE_MAX
        || row->mapping_count > DEPLOY_TASK_MAPPING_MAX
        || row->image_first > UINT32_MAX - row->image_count
        || row->mapping_first > UINT32_MAX - row->mapping_count) {
        return std::nullopt;
    }

    word_t required = page_size;
    uint32_t image_segments[DEPLOY_TASK_IMAGE_MAX]{};
    const auto include = [&required](word_t size) noexcept -> bool {
        if (size == 0 || (size % DEPLOY_PAGE_SIZE) != 0) {
            return false;
        }
        if (size > required) {
            required = size;
        }
        return true;
    };
    const auto same_name = [](boot::Bytes candidate,
                              ByteView requested) noexcept -> bool {
        if (!candidate || !requested
            || candidate.size() != requested.size()) {
            return false;
        }
        for (size_t index = 0; index < requested.size(); ++index) {
            uint64_t value{};
            if (!candidate.read(index, 1, value)
                || value != requested[index]) {
                return false;
            }
        }
        return true;
    };

    for (uint32_t image_index = 0; image_index < row->image_count;
         ++image_index) {
        const auto image = task.image(image_index);
        if (!image
            || image->source_kind != DEPLOY_IMAGE_SOURCE_BOOT_BUNDLE) {
            return std::nullopt;
        }
        const ByteView name = task.string(image->source);
        if (!name) {
            return std::nullopt;
        }
        boot::Module module{};
        size_t matches = 0;
        for (size_t module_index = 0; module_index < bundle.module_count();
             ++module_index) {
            boot::Module candidate{};
            if (!bundle.module(module_index, candidate)
                || !same_name(candidate.name(), name)) {
                continue;
            }
            module = candidate;
            ++matches;
        }
        if (matches != 1 || !module.bootable()
            || module.segment_count() == 0
            || module.segment_count() > 32) {
            return std::nullopt;
        }
        image_segments[image_index] =
            static_cast<uint32_t>(module.segment_count());
        for (size_t segment_index = 0;
             segment_index < module.segment_count(); ++segment_index) {
            boot::Segment segment{};
            if (!module.segment(segment_index, segment)
                || segment.memory_size == 0
                || static_cast<uintmax_t>(segment.memory_size)
                    > static_cast<uintmax_t>(max_word)
                || static_cast<uintmax_t>(segment.address)
                    > static_cast<uintmax_t>(max_word)) {
                return std::nullopt;
            }
            const word_t rounded = Window::round_size(
                static_cast<word_t>(segment.memory_size));
            const Window destination{
                static_cast<word_t>(segment.address), rounded};
            if (rounded == 0 || !destination.valid()
                || !include(rounded)) {
                return std::nullopt;
            }
        }
    }

    for (uint32_t mapping_index = 0; mapping_index < row->mapping_count;
         ++mapping_index) {
        const auto mapping = task.mapping(mapping_index);
        if (!mapping) {
            return std::nullopt;
        }
        switch (mapping->source) {
        case DEPLOY_MAPPING_SOURCE_IMAGE_SEGMENT: {
            if (mapping->image == DEPLOY_NO_INDEX
                || mapping->image < row->image_first
                || mapping->image - row->image_first >= row->image_count
                || mapping->segment == DEPLOY_NO_INDEX) {
                return std::nullopt;
            }
            const uint32_t image_index = mapping->image - row->image_first;
            if (mapping->segment >= image_segments[image_index]) {
                return std::nullopt;
            }
            break;
        }
        case DEPLOY_MAPPING_SOURCE_ZERO: {
            if (mapping->size > static_cast<uint64_t>(max_word)
                || mapping->address > static_cast<uint64_t>(max_word)) {
                return std::nullopt;
            }
            const word_t address =
                static_cast<word_t>(mapping->address);
            const word_t size = static_cast<word_t>(mapping->size);
            const word_t rounded = Window::round_size(size);
            const Window destination{address, rounded};
            if (rounded == 0 || !destination.valid()
                || !include(rounded)) {
                return std::nullopt;
            }
            break;
        }
        case DEPLOY_MAPPING_SOURCE_PAGER:
            /* Pager-backed mappings are installed without scratch population. */
            break;
        default:
            return std::nullopt;
        }
    }
    return required;
}

// ElfLoader borrows all three deployment views.  It never retains a
// Bundle byte view or a capability owner: task is the sole selector
// owner, while BundleMap and Map delimit temporary mappings.
template<
    typename Task,
    size_t SegmentCapacity = 32,
    size_t StackCapacity = 64>
class ElfLoader final {
public:
    using B = typename Task::backend_type;
    using Image = ElfMap<SegmentCapacity, StackCapacity>;
    using BundleLease = BundleMap<B>;
    using Scratch = Map<B>;
    using owner_type = typename Task::owner_type;

    ElfLoader(
        Task& task,
        BundleLease& bundle,
        Scratch& scratch,
        ElfSrc source = {}) noexcept
        : task_(task), bundle_(bundle), scratch_(scratch), source_(source) {}

    [[nodiscard]] auto materialize(
        size_t module_index,
        Image& output) noexcept -> status_t {
        const boot::Bundle* const bundle = bundle_.view();
        if (!bundle) {
            return STATUS_BAD_ARGS;
        }
        boot::Module module{};
        if (!bundle->module(module_index, module)) {
            return STATUS_BAD_ARGS;
        }
        return materialize_module(module, output);
    }

    template<size_t N>
    [[nodiscard]] auto materialize(
        const char (&name)[N],
        Image& output) noexcept -> status_t {
        const boot::Bundle* const bundle = bundle_.view();
        if (!bundle) {
            return STATUS_BAD_ARGS;
        }
        boot::Module module{};
        if (!bundle->find(name, module)) {
            return STATUS_BAD_ARGS;
        }
        return materialize_module(module, output);
    }

    [[nodiscard]] auto materialize(
        ByteView name,
        Image& output) noexcept -> status_t {
        const boot::Bundle* const bundle = bundle_.view();
        if (!bundle || !name) {
            return STATUS_BAD_ARGS;
        }
        boot::Module module{};
        for (size_t index = 0; index < bundle->module_count(); ++index) {
            boot::Module candidate{};
            if (!bundle->module(index, candidate)
                || candidate.name().size() != name.size()) {
                continue;
            }
            bool equal = true;
            for (size_t byte = 0; byte < name.size(); ++byte) {
                uint64_t value{};
                if (!candidate.name().read(byte, 1, value)
                    || value != name[byte]) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                module = candidate;
                break;
            }
        }
        if (module.segment_count() == 0) {
            return STATUS_BAD_ARGS;
        }
        return materialize_module(module, output);
    }

    // Source MemoryObjects remain armed until retire_sources().  The named
    // form makes that construction lifetime explicit at callers that consume
    // selectors in later descriptor snapshots.
    template<size_t N>
    [[nodiscard]] auto materialize_retained(
        const char (&name)[N],
        Image& output) noexcept -> status_t {
        return materialize(name, output);
    }

    [[nodiscard]] auto materialize_stacks(
        size_t count,
        uintptr_t base,
        word_t stride,
        word_t size,
        Image& output) noexcept -> status_t {
        output.stacks.clear();
        if (count > StackCapacity || count == 0
            || !valid_range(base, size)
            || stride == 0
            || (stride % DEPLOY_PAGE_SIZE) != 0
            || stride < size) {
            return STATUS_BAD_ARGS;
        }

        for (size_t index = 0; index < count; ++index) {
            const auto offset = libk::checked_multiply(
                static_cast<word_t>(index), stride);
            const auto address = offset.has_value()
                ? libk::checked_add(
                    static_cast<word_t>(base), offset.value())
                : std::optional<word_t>{};
            if (!address.has_value() || !valid_range(address.value(), size)) {
                output.stacks.clear();
                return STATUS_BAD_ARGS;
            }

            sys::cap::CapRef memory{};
            status_t status = create_memory(
                size, VM_READ | VM_WRITE, memory);
            if (status != STATUS_OK) {
                output.stacks.clear();
                return status;
            }
            sys::cap::CapRef region{};
            status = create_region(
                address.value(), size,
                VM_READ | VM_WRITE, region);
            if (status != STATUS_OK) {
                output.stacks.clear();
                return status;
            }
            status = map(region, memory, address.value(), size,
                VM_READ | VM_WRITE);
            if (status != STATUS_OK) {
                output.stacks.clear();
                return status;
            }
            const auto top = libk::checked_add(
                address.value(), static_cast<word_t>(size));
            if (!top.has_value()
                || !output.stacks.try_push_back(typename Image::Stack{
                    .mapping = typename Image::Mapping{
                        .memory = memory,
                        .region = region,
                        .address = static_cast<uintptr_t>(address.value()),
                        .size = size,
                        .access = VM_READ | VM_WRITE},
                    .top = top.value()})) {
                output.stacks.clear();
                return STATUS_NO_MEMORY;
            }
        }
        return STATUS_OK;
    }

    [[nodiscard]] auto materialize_stacks_retained(
        size_t count,
        uintptr_t base,
        word_t stride,
        word_t size,
        Image& output) noexcept -> status_t {
        return materialize_stacks(count, base, stride, size, output);
    }

    /* Materialize one anonymous mapping while retaining its MemoryObject
     * selector in task.  The scratch window writes the zero-filled
     * backing before an executable object is sealed, so a critical mapping is
     * resident before construction proceeds. */
    [[nodiscard]] auto materialize_zero(
        word_t address,
        word_t size,
        word_t access,
        typename Image::Mapping& output) noexcept -> status_t {
        output = {};
        if (!valid_range(address, size)
            || access == 0
            || (access & ~(VM_READ | VM_WRITE | VM_EXECUTE))
                != 0
            || ((access & VM_WRITE) != 0
                && (access & VM_READ) == 0)
            || ((access & VM_WRITE) != 0
                && (access & VM_EXECUTE) != 0)) {
            return STATUS_BAD_ARGS;
        }
        const word_t load_access = access
            | VM_READ | VM_WRITE;
        sys::cap::CapRef memory{};
        status_t status = create_memory(size, load_access, memory);
        if (status != STATUS_OK) {
            return status;
        }
        status = populate_bytes(memory, nullptr, 0, size);
        if (status != STATUS_OK) {
            return status;
        }
        if ((access & VM_EXECUTE) != 0) {
            const auto reference = memory;
            if (!reference) {
                return STATUS_INVALID_CAP;
            }
            status = B::memory_seal(reference);
            if (status != STATUS_OK) {
                return status;
            }
        }
        sys::cap::CapRef region{};
        status = create_region(address, size, access, region);
        if (status != STATUS_OK) {
            return status;
        }
        status = map(region, memory, address, size, access);
        if (status != STATUS_OK) {
            return status;
        }
        output = typename Image::Mapping{
            .memory = memory,
            .region = region,
            .address = static_cast<uintptr_t>(address),
            .size = size,
            .access = access};
        return STATUS_OK;
    }

    template<typename T = B>
    requires requires(
        sys::cap::CapRef pool,
        sys::cap::CapRef pager,
        word_t size,
        word_t access) {
        { T::memory_create_pager(pool, size, access, pager) }
            -> std::same_as<sys::SysResult>;
    }
    [[nodiscard]] auto materialize_paged(
        sys::cap::CapRef pager,
        word_t address,
        word_t size,
        word_t access,
        typename Image::Mapping& output) noexcept -> status_t {
        output = {};
        if (!pager || pager.cspace != 0 || !valid_range(address, size)
            || access == 0
            || (access & ~(VM_READ | VM_WRITE | VM_EXECUTE))
                != 0
            || ((access & VM_WRITE) != 0
                && (access & VM_READ) == 0)
            || ((access & VM_WRITE) != 0
                && (access & VM_EXECUTE) != 0)) {
            return STATUS_BAD_ARGS;
        }
        const auto pool = task_.pool();
        if (!pool) {
            return STATUS_BAD_ARGS;
        }
        const sys::SysResult created = T::memory_create_pager(
            pool, size, access, pager);
        if (created.value == 0) {
            return created.status == STATUS_OK
                ? STATUS_INVALID_CAP : created.status;
        }
        owner_type memory_owner{sys::cap::CapRef{created.value, 0}};
        if (created.status != STATUS_OK) {
            const status_t closed = memory_owner.close();
            if (closed != STATUS_OK) {
                B::ownership_fault(closed);
            }
            return created.status;
        }
        const auto memory = task_.keep(std::move(memory_owner));
        if (!memory) {
            return STATUS_NO_MEMORY;
        }
        sys::cap::CapRef region{};
        status_t status = create_region(
            address, size, access, region);
        if (status != STATUS_OK) {
            return status;
        }
        status = map(region, *memory, address, size, access);
        if (status != STATUS_OK) {
            return status;
        }
        output = typename Image::Mapping{
            .memory = *memory,
            .region = region,
            .address = static_cast<uintptr_t>(address),
            .size = size,
            .access = access};
        return STATUS_OK;
    }

    [[nodiscard]] auto materialize_zero_readonly(
        word_t address,
        word_t size,
        typename Image::Mapping& output) noexcept -> status_t {
        const status_t status = materialize_zero(
            address, size, VM_READ, output);
        if (status != STATUS_OK) {
            return status;
        }
        const status_t closed = task_.drop(output.memory);
        if (closed != STATUS_OK) {
            return closed;
        }
        output.memory = {};
        return STATUS_OK;
    }

    // Construction consumers may need the source MemoryObject after its
    // those unpublished slots in task until the caller has completed
    // every consumer, then retire them in place.  A failed close leaves the
    // exact slot armed so a later call resumes at the same source.
    [[nodiscard]] auto retire_sources(Image& image) noexcept -> status_t {
        for (auto& mapping : image.segments) {
            if (!bool(mapping.memory)) {
                continue;
            }
            const status_t status = task_.drop(mapping.memory);
            if (status != STATUS_OK) {
                return status;
            }
            mapping.memory = {};
        }
        for (auto& stack : image.stacks) {
            if (!bool(stack.mapping.memory)) {
                continue;
            }
            const status_t status = task_.drop(
                stack.mapping.memory);
            if (status != STATUS_OK) {
                return status;
            }
            stack.mapping.memory = {};
        }
        return STATUS_OK;
    }

    // Populate a writable descriptor object through the same scratch window
    // used for ELF bytes.  The caller owns the returned task slot and
    // may consume it in a typed constructor before closing that slot.
    [[nodiscard]] auto materialize_descriptor(
        const void* source,
        size_t source_size,
        sys::cap::CapRef& output) noexcept -> status_t {
        output = {};
        const auto size = rounded(source_size);
        if (!source || !size.has_value()) {
            return STATUS_BAD_ARGS;
        }
        status_t status = create_memory(
            size.value(), VM_READ | VM_WRITE, output);
        if (status != STATUS_OK) {
            output = {};
            return status;
        }
        status = populate_bytes(
            output, static_cast<const uint8_t*>(source), source_size,
            size.value());
        if (status != STATUS_OK) {
            output = {};
        }
        return status;
    }

    /* Write an already-adopted descriptor carrier through the shared scratch
     * mapping.  The carrier remains task-owned; this operation only
     * snapshots caller bytes and never manufactures a second capability. */
    [[nodiscard]] auto write(
        sys::cap::CapRef memory,
        word_t memory_size,
        word_t offset,
        const void* source,
        size_t source_size) noexcept -> status_t {
        if (!bool(memory) || memory.cspace != 0
            || !source || memory_size == 0
            || (memory_size % DEPLOY_PAGE_SIZE) != 0
            || offset > memory_size || source_size > memory_size - offset) {
            return STATUS_BAD_ARGS;
        }
        const auto reference = memory;
        if (!reference) {
            return STATUS_INVALID_CAP;
        }
        status_t status = populate_backing(reference, memory_size);
        if (status != STATUS_OK) return status;
        status = scratch_.map(
            reference, 0, memory_size,
            VM_READ | VM_WRITE);
        if (status != STATUS_OK) {
            return status;
        }
        auto* const destination = reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(scratch_.address()));
        status = B::memory_write(
            destination + offset,
            static_cast<const uint8_t*>(source), source_size);
        const status_t unmapped = scratch_.unmap();
        return status != STATUS_OK ? status : unmapped;
    }

    // Build a readonly child mapping from a bounded caller-owned snapshot.
    // The writable MemoryObject selector is closed after the mapping commits;
    // kernel MappingAuthority/ObjectRef retains the object lifetime.
    [[nodiscard]] auto materialize_readonly(
        word_t address,
        const void* source,
        size_t source_size,
        typename Image::Mapping& output) noexcept -> status_t {
        output = {};
        const auto size = rounded(source_size);
        if (!source || !size.has_value()
            || !valid_range(address, size.value())) {
            return STATUS_BAD_ARGS;
        }
        sys::cap::CapRef memory{};
        status_t status = create_memory(
            size.value(), VM_READ | VM_WRITE, memory);
        if (status != STATUS_OK) {
            return status;
        }
        status = populate_bytes(
            memory, static_cast<const uint8_t*>(source), source_size,
            size.value());
        if (status != STATUS_OK) {
            return status;
        }
        sys::cap::CapRef region{};
        status = create_region(
            address, size.value(), VM_READ, region);
        if (status != STATUS_OK) {
            return status;
        }
        status = map(
            region, memory, address, size.value(), VM_READ);
        if (status != STATUS_OK) {
            return status;
        }
        status = close_memory(memory);
        if (status != STATUS_OK) {
            return status;
        }
        output = typename Image::Mapping{
            .region = region,
            .address = address,
            .size = size.value(),
            .access = VM_READ};
        return STATUS_OK;
    }

private:
    [[nodiscard]] static auto valid_range(
        uintptr_t address,
        word_t size) noexcept -> bool {
        return address != 0 && size != 0
            && (address % DEPLOY_PAGE_SIZE) == 0
            && (size % DEPLOY_PAGE_SIZE) == 0
            && libk::checked_add(
                static_cast<word_t>(address), size).has_value();
    }

    [[nodiscard]] static auto rounded(size_t size) noexcept
        -> std::optional<word_t> {
        if (size == 0 || size > static_cast<size_t>(~word_t{})) {
            return std::nullopt;
        }
        const auto aligned = libk::checked_align_up(
            static_cast<word_t>(size), DEPLOY_PAGE_SIZE);
        return aligned;
    }

    [[nodiscard]] auto create_memory(
        word_t size,
        word_t access,
        sys::cap::CapRef& output) noexcept -> status_t {
        const auto pool = task_.pool();
        if (!pool || !task_.pool()) {
            return STATUS_BAD_ARGS;
        }
        const sys::SysResult created = B::memory_create(
            pool, size, access);
        if (created.value == 0) {
            return created.status == STATUS_OK
                ? STATUS_INVALID_CAP : created.status;
        }
        owner_type owner{sys::cap::CapRef{created.value, 0}};
        if (created.status != STATUS_OK) {
            const status_t closed = owner.close();
            if (closed != STATUS_OK) {
                B::ownership_fault(closed);
            }
            return created.status;
        }
        const auto slot = task_.keep(std::move(owner));
        if (!slot.has_value()) {
            return STATUS_NO_MEMORY;
        }
        output = slot.value();
        return STATUS_OK;
    }

    [[nodiscard]] auto create_region(
        word_t address,
        word_t size,
        word_t access,
        sys::cap::CapRef& output) noexcept -> status_t {
        const auto vspace = task_.vspace();
        if (!bool(vspace)) {
            return STATUS_INVALID_CAP;
        }
        const sys::SysResult created = B::vm_slice(
            vspace, address, size, access,
            RIGHT_DUPLICATE | RIGHT_MAP | RIGHT_UNMAP);
        if (created.value == 0) {
            return created.status == STATUS_OK
                ? STATUS_INVALID_CAP : created.status;
        }
        owner_type owner{sys::cap::CapRef{created.value, 0}};
        if (created.status != STATUS_OK) {
            const status_t closed = owner.close();
            if (closed != STATUS_OK) {
                B::ownership_fault(closed);
            }
            return created.status;
        }
        const auto slot = task_.keep(std::move(owner));
        if (!slot.has_value()) {
            return STATUS_NO_MEMORY;
        }
        output = slot.value();
        return STATUS_OK;
    }

    [[nodiscard]] auto map(
        sys::cap::CapRef region,
        sys::cap::CapRef memory,
        word_t address,
        word_t size,
        word_t access,
        word_t first = 0) noexcept -> status_t {
        const auto region_ref = region;
        const auto memory_ref = memory;
        if (!bool(region_ref) || !bool(memory_ref)) {
            return STATUS_INVALID_CAP;
        }
        const status_t status = B::vm_map(
            region_ref, memory_ref, address, size, first, access);
        return committed(status) ? STATUS_OK : status;
    }

    [[nodiscard]] auto close_memory(sys::cap::CapRef slot) noexcept
        -> status_t {
        return task_.drop(slot);
    }

    [[nodiscard]] static auto populate_backing(
        sys::cap::CapRef memory, word_t size) noexcept -> status_t {
        // Scratch writes must not turn a budget failure into an unhandled
        // fault in the supervisor's own address space.
        for (word_t page = 0; page < size / DEPLOY_PAGE_SIZE; ++page) {
            const auto status = B::memory_populate(memory, page);
            if (status != STATUS_OK) return status;
        }
        return STATUS_OK;
    }

    [[nodiscard]] auto populate(
        sys::cap::CapRef memory,
        const boot::Segment& segment,
        word_t size) noexcept -> status_t {
        if (segment.file_size != 0 && !segment.file) {
            return STATUS_BAD_ARGS;
        }
        return populate_bytes(
            memory, segment.file, segment.file_size, size);
    }

    [[nodiscard]] auto populate_bytes(
        sys::cap::CapRef memory,
        const uint8_t* source,
        size_t source_size,
        word_t size) noexcept -> status_t {
        if (!memory || source_size > size
            || (source_size != 0 && !source)) {
            return STATUS_BAD_ARGS;
        }
        const auto backing = populate_backing(memory, size);
        if (backing != STATUS_OK) return backing;
        const status_t mapped = scratch_.map(
            memory, 0, size,
            VM_READ | VM_WRITE);
        if (mapped != STATUS_OK) {
            return mapped;
        }
        auto* const destination = reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(scratch_.address()));
        status_t status = B::memory_write(
            destination, source, source_size);
        if (status != STATUS_OK) {
            const status_t unmapped = scratch_.unmap();
            if (unmapped != STATUS_OK) {
                B::ownership_fault(unmapped);
            }
            return status;
        }
        status = B::memory_write(
            destination + source_size, nullptr, size - source_size);
        if (status != STATUS_OK) {
            const status_t unmapped = scratch_.unmap();
            if (unmapped != STATUS_OK) {
                B::ownership_fault(unmapped);
            }
            return status;
        }
        return scratch_.unmap();
    }

    [[nodiscard]] auto materialize_source_segment(
        const boot::Segment& segment, word_t size,
        Image& output) noexcept -> status_t {
        constexpr word_t page = DEPLOY_PAGE_SIZE;
        const word_t file_pages = segment.file_size / page * page;
        const word_t boundary = segment.file_size % page != 0 ? page : 0;
        const word_t zero_first = file_pages + boundary;
        const auto pool = task_.pool();
        const auto vspace = task_.vspace();
        if (!pool || !vspace) return STATUS_INVALID_CAP;
        const auto region = B::vm_slice(vspace, segment.address, size,
            segment.access,
            RIGHT_DUPLICATE | RIGHT_MAP | RIGHT_UNMAP);
        owner_type region_owner{sys::cap::CapRef{region.value, 0}};
        if (region.status != STATUS_OK) return region.status;
        if (!region_owner) return STATUS_INVALID_CAP;
        sys::cap::CapRef primary{};
        word_t primary_first{};
        const auto install = [&](word_t offset, word_t length,
                                 bool file, size_t bytes) noexcept -> status_t {
            word_t first{};
            sys::SysResult created{};
            if (file) {
                auto part = segment;
                part.address += offset;
                part.file += offset;
                part.file_size = part.memory_size = length;
                created = source_.create(source_.context, part, first);
            } else {
                created = B::memory_create(pool, length,
                    segment.access | VM_READ | VM_WRITE);
            }
            owner_type memory_owner{sys::cap::CapRef{created.value, 0}};
            if (created.status != STATUS_OK) return created.status;
            if (!memory_owner) return STATUS_INVALID_CAP;
            if (!file && bytes != 0) {
                const auto status = populate_bytes(memory_owner.reference(),
                    segment.file + offset, bytes, length);
                if (status != STATUS_OK) return status;
            }
            if (!file && (segment.access & VM_EXECUTE) != 0) {
                const auto status = B::memory_seal(memory_owner.reference());
                if (status != STATUS_OK) return status;
            }
            const auto address = static_cast<word_t>(segment.address + offset);
            const auto flags = file && (segment.access & VM_WRITE) != 0
                ? VM_MAP_PRIVATE : 0;
            const auto mapped = B::vm_map(region_owner.reference(), memory_owner.reference(),
                address, length, first, segment.access | flags);
            if (!committed(mapped)) return mapped;
            if (bool(primary)) return STATUS_OK;
            const auto memory = task_.keep(std::move(memory_owner));
            if (!memory) return STATUS_NO_MEMORY;
            primary = *memory;
            primary_first = first;
            return STATUS_OK;
        };
        // One region describes the whole ELF segment. Subrange mappings keep
        // their backing through VSpace; only the primary cap enters the plan.
        if (file_pages != 0) {
            const auto status = install(0, file_pages, true, 0);
            if (status != STATUS_OK) return status;
        }
        if (boundary != 0) {
            const auto status = install(file_pages, page, false, segment.file_size - file_pages);
            if (status != STATUS_OK) return status;
        }
        if (zero_first < size) {
            const auto status = install(zero_first, size - zero_first, false, 0);
            if (status != STATUS_OK) return status;
        }
        if (!bool(primary)) return STATUS_INTERNAL;
        const auto saved_region = task_.keep(std::move(region_owner));
        if (!saved_region) return STATUS_NO_MEMORY;
        return output.segments.try_push_back(typename Image::Mapping{
            .memory = primary, .region = *saved_region,
            .address = segment.address, .size = size,
            .access = segment.access, .first = primary_first})
            ? STATUS_OK : STATUS_NO_MEMORY;
    }

    [[nodiscard]] auto materialize_module(
        const boot::Module& module,
        Image& output) noexcept -> status_t {
        output.clear();
        if (module.segment_count() == 0
            || module.segment_count() > SegmentCapacity) {
            return STATUS_BAD_ARGS;
        }
        output.entry = module.entry();
        for (size_t index = 0; index < module.segment_count(); ++index) {
            boot::Segment segment{};
            if (!module.segment(index, segment)) {
                output.clear();
                return STATUS_BAD_ARGS;
            }
            const auto size = rounded(segment.memory_size);
            if (!size.has_value()
                || segment.address == 0
                || (segment.address % DEPLOY_PAGE_SIZE) != 0
                || !valid_range(segment.address, size.value())
                || segment.access == 0
                || (segment.access & ~static_cast<word_t>(
                    VM_READ | VM_WRITE | VM_EXECUTE)) != 0
                || ((segment.access & VM_WRITE) != 0
                    && (segment.access & VM_READ) == 0)
                || ((segment.access & VM_WRITE) != 0
                    && (segment.access & VM_EXECUTE) != 0)) {
                output.clear();
                return STATUS_BAD_ARGS;
            }
            const word_t load_access = segment.access
                | VM_READ | VM_WRITE;
            if (bool(source_.create)) {
                const auto status = materialize_source_segment(segment, size.value(), output);
                if (status != STATUS_OK) output.clear();
                if (status != STATUS_OK) return status;
                continue;
            }
            sys::cap::CapRef memory{};
            status_t status{};
            word_t first{};
            status = create_memory(size.value(), load_access, memory);
            if (status == STATUS_OK) status = populate(memory, segment, size.value());
            if (status != STATUS_OK) {
                output.clear();
                return status;
            }
            if ((segment.access & VM_EXECUTE) != 0) {
                const auto reference = memory;
                if (!bool(reference)) {
                    output.clear();
                    return STATUS_INVALID_CAP;
                }
                status = B::memory_seal(reference);
                if (status != STATUS_OK) {
                    output.clear();
                    return status;
                }
            }
            sys::cap::CapRef region{};
            status = create_region(
                static_cast<word_t>(segment.address), size.value(),
                segment.access, region);
            if (status != STATUS_OK
                || (status = map(
                    region, memory, static_cast<word_t>(segment.address),
                    size.value(), segment.access, first)) != STATUS_OK
            ) {
                output.clear();
                return status;
            }
            if (!output.segments.try_push_back(typename Image::Mapping{
                    .memory = memory,
                    .region = region,
                    .address = segment.address,
                    .size = size.value(),
                    .access = segment.access, .first = first})) {
                output.clear();
                return STATUS_NO_MEMORY;
            }
        }
        return STATUS_OK;
    }

    Task& task_;
    BundleLease& bundle_;
    Scratch& scratch_;
    ElfSrc source_{};
};

} // namespace deploy
