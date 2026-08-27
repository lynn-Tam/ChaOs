#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/checked_arithmetic.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <uapi/deploy.h>
#include <uapi/object.h>
#include <uapi/status.h>
#include <uapi/vm.h>
#include <user/lib/boot_bundle.hpp>
#include <user/lib/deployment.hpp>
#include <user/lib/deploy_manifest.hpp>

namespace myos::deploy {

template<size_t SegmentCapacity = 32, size_t StackCapacity = 64>
struct MaterializedImage final {
    struct Mapping final {
        LocalSlot memory{};
        LocalSlot region{};
        uintptr_t address{};
        myos_word_t size{};
        myos_word_t access{};
    };

    struct Stack final {
        Mapping mapping{};
        myos_word_t top{};
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
concept MaterializerBackend = Backend<B>
    && requires(
        cap::CapRef pool,
        cap::CapRef memory,
        void* destination,
        const uint8_t* source,
        myos_word_t size,
        myos_word_t access) {
    { B::memory_create(pool, size, access) } -> libk::SameAs<SysResult>;
    { B::memory_seal(memory) } -> libk::SameAs<myos_status_t>;
    { B::memory_write(destination, source, size) }
        -> libk::SameAs<myos_status_t>;
};

// ImageMaterializer borrows all three deployment views.  It never retains a
// Bundle byte view or a capability owner: TaskSpace is the sole selector
// owner, while MappedBundle and ScratchWindow delimit temporary mappings.
template<
    size_t LocalCapacity,
    size_t RemoteCapacity,
    typename B,
    size_t SegmentCapacity = 32,
    size_t StackCapacity = 64>
requires MaterializerBackend<B>
class ImageMaterializer final {
public:
    using Task = TaskSpace<LocalCapacity, RemoteCapacity, B>;
    using Image = MaterializedImage<SegmentCapacity, StackCapacity>;
    using BundleLease = MappedBundle<B>;
    using Scratch = ScratchWindow<B>;
    using owner_type = typename Task::owner_type;

    ImageMaterializer(
        Task& task,
        BundleLease& bundle,
        Scratch& scratch) noexcept
        : task_(task), bundle_(bundle), scratch_(scratch) {}

    [[nodiscard]] auto materialize(
        size_t module_index,
        Image& output) noexcept -> myos_status_t {
        const boot::Bundle* const bundle = bundle_.view();
        if (bundle == nullptr) {
            return MYOS_STATUS_BAD_ARGS;
        }
        boot::Module module{};
        if (!bundle->module(module_index, module)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return materialize_module(module, output);
    }

    template<size_t N>
    [[nodiscard]] auto materialize(
        const char (&name)[N],
        Image& output) noexcept -> myos_status_t {
        const boot::Bundle* const bundle = bundle_.view();
        if (bundle == nullptr) {
            return MYOS_STATUS_BAD_ARGS;
        }
        boot::Module module{};
        if (!bundle->find(name, module)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return materialize_module(module, output);
    }

    [[nodiscard]] auto materialize(
        ByteView name,
        Image& output) noexcept -> myos_status_t {
        const boot::Bundle* const bundle = bundle_.view();
        if (bundle == nullptr || !name) {
            return MYOS_STATUS_BAD_ARGS;
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
            return MYOS_STATUS_BAD_ARGS;
        }
        return materialize_module(module, output);
    }

    // Source MemoryObjects remain armed until retire_sources().  The named
    // form makes that construction lifetime explicit at callers that consume
    // selectors in later descriptor snapshots.
    template<size_t N>
    [[nodiscard]] auto materialize_retained(
        const char (&name)[N],
        Image& output) noexcept -> myos_status_t {
        return materialize(name, output);
    }

    [[nodiscard]] auto materialize_stacks(
        size_t count,
        uintptr_t base,
        myos_word_t stride,
        myos_word_t size,
        Image& output) noexcept -> myos_status_t {
        output.stacks.clear();
        if (count > StackCapacity || count == 0
            || !valid_range(base, size)
            || stride == 0
            || (stride % MYOS_DEPLOY_PAGE_SIZE) != 0
            || stride < size) {
            return MYOS_STATUS_BAD_ARGS;
        }

        for (size_t index = 0; index < count; ++index) {
            const auto offset = libk::checked_multiply(
                static_cast<myos_word_t>(index), stride);
            const auto address = offset.has_value()
                ? libk::checked_add(
                    static_cast<myos_word_t>(base), offset.value())
                : libk::optional<myos_word_t>{};
            if (!address.has_value() || !valid_range(address.value(), size)) {
                output.stacks.clear();
                return MYOS_STATUS_BAD_ARGS;
            }

            LocalSlot memory{};
            myos_status_t status = create_memory(
                size, MYOS_VM_READ | MYOS_VM_WRITE, memory);
            if (status != MYOS_STATUS_OK) {
                output.stacks.clear();
                return status;
            }
            LocalSlot region{};
            status = create_region(
                address.value(), size,
                MYOS_VM_READ | MYOS_VM_WRITE, region);
            if (status != MYOS_STATUS_OK) {
                output.stacks.clear();
                return status;
            }
            status = map(region, memory, address.value(), size,
                MYOS_VM_READ | MYOS_VM_WRITE);
            if (status != MYOS_STATUS_OK) {
                output.stacks.clear();
                return status;
            }
            const auto top = libk::checked_add(
                address.value(), static_cast<myos_word_t>(size));
            if (!top.has_value()
                || !output.stacks.try_push_back(typename Image::Stack{
                    .mapping = typename Image::Mapping{
                        .memory = memory,
                        .region = region,
                        .address = static_cast<uintptr_t>(address.value()),
                        .size = size,
                        .access = MYOS_VM_READ | MYOS_VM_WRITE},
                    .top = top.value()})) {
                output.stacks.clear();
                return MYOS_STATUS_NO_MEMORY;
            }
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto materialize_stacks_retained(
        size_t count,
        uintptr_t base,
        myos_word_t stride,
        myos_word_t size,
        Image& output) noexcept -> myos_status_t {
        return materialize_stacks(count, base, stride, size, output);
    }

    /* Materialize one anonymous mapping while retaining its MemoryObject
     * selector in TaskSpace.  The scratch window writes the zero-filled
     * backing before an executable object is sealed, so a critical mapping is
     * resident before construction proceeds. */
    [[nodiscard]] auto materialize_zero(
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        typename Image::Mapping& output) noexcept -> myos_status_t {
        output = {};
        if (!valid_range(address, size)
            || access == 0
            || (access & ~(MYOS_VM_READ | MYOS_VM_WRITE | MYOS_VM_EXECUTE))
                != 0
            || ((access & MYOS_VM_WRITE) != 0
                && (access & MYOS_VM_READ) == 0)
            || ((access & MYOS_VM_WRITE) != 0
                && (access & MYOS_VM_EXECUTE) != 0)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        const myos_word_t load_access = access
            | MYOS_VM_READ | MYOS_VM_WRITE;
        LocalSlot memory{};
        myos_status_t status = create_memory(size, load_access, memory);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        status = populate_bytes(memory, nullptr, 0, size);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        if ((access & MYOS_VM_EXECUTE) != 0) {
            const auto reference = task_.lookup(
                memory, MYOS_OBJECT_KIND_MEMORY);
            if (!reference) {
                return MYOS_STATUS_INVALID_CAP;
            }
            status = B::memory_seal(reference.value());
            if (status != MYOS_STATUS_OK) {
                return status;
            }
        }
        LocalSlot region{};
        status = create_region(address, size, access, region);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        status = map(region, memory, address, size, access);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        output = typename Image::Mapping{
            .memory = memory,
            .region = region,
            .address = static_cast<uintptr_t>(address),
            .size = size,
            .access = access};
        return MYOS_STATUS_OK;
    }

    template<typename T = B>
    requires requires(
        cap::CapRef pool,
        cap::CapRef pager,
        myos_word_t size,
        myos_word_t access) {
        { T::memory_create_pager(pool, size, access, pager) }
            -> libk::SameAs<SysResult>;
    }
    [[nodiscard]] auto materialize_paged(
        cap::CapRef pager,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        typename Image::Mapping& output) noexcept -> myos_status_t {
        output = {};
        if (!pager || pager.cspace != 0 || !valid_range(address, size)
            || access == 0
            || (access & ~(MYOS_VM_READ | MYOS_VM_WRITE | MYOS_VM_EXECUTE))
                != 0
            || ((access & MYOS_VM_WRITE) != 0
                && (access & MYOS_VM_READ) == 0)
            || ((access & MYOS_VM_WRITE) != 0
                && (access & MYOS_VM_EXECUTE) != 0)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        const auto pool = task_.pool();
        if (!pool) {
            return MYOS_STATUS_BAD_ARGS;
        }
        const SysResult created = T::memory_create_pager(
            pool.value(), size, access, pager);
        if (created.value == 0) {
            return created.status == MYOS_STATUS_OK
                ? MYOS_STATUS_INVALID_CAP : created.status;
        }
        owner_type memory_owner{cap::CapRef{created.value, 0}};
        if (created.status != MYOS_STATUS_OK) {
            const myos_status_t closed = memory_owner.close();
            if (closed != MYOS_STATUS_OK) {
                B::ownership_fault(closed);
            }
            return created.status;
        }
        const auto memory = task_.adopt_local(
            libk::move(memory_owner), MYOS_OBJECT_KIND_MEMORY);
        if (!memory) {
            return MYOS_STATUS_NO_MEMORY;
        }
        LocalSlot region{};
        myos_status_t status = create_region(
            address, size, access, region);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        status = map(region, *memory, address, size, access);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        output = typename Image::Mapping{
            .memory = *memory,
            .region = region,
            .address = static_cast<uintptr_t>(address),
            .size = size,
            .access = access};
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto materialize_zero_readonly(
        myos_word_t address,
        myos_word_t size,
        typename Image::Mapping& output) noexcept -> myos_status_t {
        const myos_status_t status = materialize_zero(
            address, size, MYOS_VM_READ, output);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        const myos_status_t closed = task_.close_slot(output.memory);
        if (closed != MYOS_STATUS_OK) {
            return closed;
        }
        output.memory = {};
        return MYOS_STATUS_OK;
    }

    // Construction consumers may need the source MemoryObject after its
    // final mapping (for example Thread/Endpoint/Vproc snapshots).  Keep
    // those unpublished slots in TaskSpace until the caller has completed
    // every consumer, then retire them in place.  A failed close leaves the
    // exact slot armed so a later call resumes at the same source.
    [[nodiscard]] auto retire_sources(Image& image) noexcept -> myos_status_t {
        for (auto& mapping : image.segments) {
            if (!mapping.memory.valid()) {
                continue;
            }
            const myos_status_t status = task_.close_slot(mapping.memory);
            if (status != MYOS_STATUS_OK) {
                return status;
            }
            mapping.memory = {};
        }
        for (auto& stack : image.stacks) {
            if (!stack.mapping.memory.valid()) {
                continue;
            }
            const myos_status_t status = task_.close_slot(
                stack.mapping.memory);
            if (status != MYOS_STATUS_OK) {
                return status;
            }
            stack.mapping.memory = {};
        }
        return MYOS_STATUS_OK;
    }

    // Populate a writable descriptor object through the same scratch window
    // used for ELF bytes.  The caller owns the returned TaskSpace slot and
    // may consume it in a typed constructor before closing that slot.
    [[nodiscard]] auto materialize_descriptor(
        const void* source,
        size_t source_size,
        LocalSlot& output) noexcept -> myos_status_t {
        output = {};
        const auto size = rounded(source_size);
        if (source == nullptr || !size.has_value()) {
            return MYOS_STATUS_BAD_ARGS;
        }
        myos_status_t status = create_memory(
            size.value(), MYOS_VM_READ | MYOS_VM_WRITE, output);
        if (status != MYOS_STATUS_OK) {
            output = {};
            return status;
        }
        status = populate_bytes(
            output, static_cast<const uint8_t*>(source), source_size,
            size.value());
        if (status != MYOS_STATUS_OK) {
            output = {};
        }
        return status;
    }

    /* Write an already-adopted descriptor carrier through the shared scratch
     * mapping.  The carrier remains TaskSpace-owned; this operation only
     * snapshots caller bytes and never manufactures a second capability. */
    [[nodiscard]] auto write(
        LocalSlot memory,
        myos_word_t memory_size,
        myos_word_t offset,
        const void* source,
        size_t source_size) noexcept -> myos_status_t {
        if (!memory.valid() || memory.kind != MYOS_OBJECT_KIND_MEMORY
            || source == nullptr || memory_size == 0
            || (memory_size % MYOS_DEPLOY_PAGE_SIZE) != 0
            || offset > memory_size || source_size > memory_size - offset) {
            return MYOS_STATUS_BAD_ARGS;
        }
        const auto reference = task_.lookup(memory, MYOS_OBJECT_KIND_MEMORY);
        if (!reference) {
            return MYOS_STATUS_INVALID_CAP;
        }
        myos_status_t status = scratch_.map(
            reference.value(), 0, memory_size,
            MYOS_VM_READ | MYOS_VM_WRITE);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        auto* const destination = reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(scratch_.address()));
        status = B::memory_write(
            destination + offset,
            static_cast<const uint8_t*>(source), source_size);
        const myos_status_t unmapped = scratch_.unmap();
        return status != MYOS_STATUS_OK ? status : unmapped;
    }

    // Build a readonly child mapping from a bounded caller-owned snapshot.
    // The writable MemoryObject selector is closed after the mapping commits;
    // kernel MappingAuthority/ObjectRef retains the object lifetime.
    [[nodiscard]] auto materialize_readonly(
        myos_word_t address,
        const void* source,
        size_t source_size,
        typename Image::Mapping& output) noexcept -> myos_status_t {
        output = {};
        const auto size = rounded(source_size);
        if (source == nullptr || !size.has_value()
            || !valid_range(address, size.value())) {
            return MYOS_STATUS_BAD_ARGS;
        }
        LocalSlot memory{};
        myos_status_t status = create_memory(
            size.value(), MYOS_VM_READ | MYOS_VM_WRITE, memory);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        status = populate_bytes(
            memory, static_cast<const uint8_t*>(source), source_size,
            size.value());
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        LocalSlot region{};
        status = create_region(
            address, size.value(), MYOS_VM_READ, region);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        status = map(
            region, memory, address, size.value(), MYOS_VM_READ);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        status = close_memory(memory);
        if (status != MYOS_STATUS_OK) {
            return status;
        }
        output = typename Image::Mapping{
            .memory = memory,
            .region = region,
            .address = address,
            .size = size.value(),
            .access = MYOS_VM_READ};
        return MYOS_STATUS_OK;
    }

private:
    [[nodiscard]] static auto valid_range(
        uintptr_t address,
        myos_word_t size) noexcept -> bool {
        return address != 0 && size != 0
            && (address % MYOS_DEPLOY_PAGE_SIZE) == 0
            && (size % MYOS_DEPLOY_PAGE_SIZE) == 0
            && libk::checked_add(
                static_cast<myos_word_t>(address), size).has_value();
    }

    [[nodiscard]] static auto rounded(size_t size) noexcept
        -> libk::optional<myos_word_t> {
        if (size == 0 || size > static_cast<size_t>(~myos_word_t{})) {
            return libk::nullopt;
        }
        const auto aligned = libk::checked_align_up(
            static_cast<myos_word_t>(size), MYOS_DEPLOY_PAGE_SIZE);
        return aligned;
    }

    [[nodiscard]] auto create_memory(
        myos_word_t size,
        myos_word_t access,
        LocalSlot& output) noexcept -> myos_status_t {
        const auto pool = task_.pool();
        if (!pool || task_.phase() != Phase::Open) {
            return MYOS_STATUS_BAD_ARGS;
        }
        const SysResult created = B::memory_create(
            pool.value(), size, access);
        if (created.value == 0) {
            return created.status == MYOS_STATUS_OK
                ? MYOS_STATUS_INVALID_CAP : created.status;
        }
        owner_type owner{cap::CapRef{created.value, 0}};
        if (created.status != MYOS_STATUS_OK) {
            const myos_status_t closed = owner.close();
            if (closed != MYOS_STATUS_OK) {
                B::ownership_fault(closed);
            }
            return created.status;
        }
        const auto slot = task_.adopt_local(
            libk::move(owner), MYOS_OBJECT_KIND_MEMORY);
        if (!slot.has_value()) {
            return MYOS_STATUS_NO_MEMORY;
        }
        output = slot.value();
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto create_region(
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        LocalSlot& output) noexcept -> myos_status_t {
        const auto vspace = task_.lookup(
            task_.vspace_slot(), MYOS_OBJECT_KIND_VSPACE);
        if (!vspace.has_value()) {
            return MYOS_STATUS_INVALID_CAP;
        }
        const SysResult created = B::vm_create_region(
            vspace.value(), address, size, access, MYOS_VM_NORMAL,
            MYOS_RIGHT_MAP);
        if (created.value == 0) {
            return created.status == MYOS_STATUS_OK
                ? MYOS_STATUS_INVALID_CAP : created.status;
        }
        owner_type owner{cap::CapRef{created.value, 0}};
        if (created.status != MYOS_STATUS_OK) {
            const myos_status_t closed = owner.close();
            if (closed != MYOS_STATUS_OK) {
                B::ownership_fault(closed);
            }
            return created.status;
        }
        const auto slot = task_.adopt_local(
            libk::move(owner), MYOS_OBJECT_KIND_VSPACE);
        if (!slot.has_value()) {
            return MYOS_STATUS_NO_MEMORY;
        }
        output = slot.value();
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto map(
        LocalSlot region,
        LocalSlot memory,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access) noexcept -> myos_status_t {
        const auto region_ref = task_.lookup(region, MYOS_OBJECT_KIND_VSPACE);
        const auto memory_ref = task_.lookup(memory, MYOS_OBJECT_KIND_MEMORY);
        if (!region_ref.has_value() || !memory_ref.has_value()) {
            return MYOS_STATUS_INVALID_CAP;
        }
        const myos_status_t status = B::vm_map(
            region_ref.value(), memory_ref.value(), address, size, 0, access);
        return committed(status) ? MYOS_STATUS_OK : status;
    }

    [[nodiscard]] auto close_memory(LocalSlot slot) noexcept
        -> myos_status_t {
        return task_.close_slot(slot);
    }

    [[nodiscard]] auto populate(
        LocalSlot memory,
        const boot::Segment& segment,
        myos_word_t size) noexcept -> myos_status_t {
        if (segment.file_size != 0 && segment.file == nullptr) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return populate_bytes(
            memory, segment.file, segment.file_size, size);
    }

    [[nodiscard]] auto populate_bytes(
        LocalSlot memory,
        const uint8_t* source,
        size_t source_size,
        myos_word_t size) noexcept -> myos_status_t {
        const auto reference = task_.lookup(
            memory, MYOS_OBJECT_KIND_MEMORY);
        if (!reference.has_value() || source_size > size
            || (source_size != 0 && source == nullptr)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        const myos_status_t mapped = scratch_.map(
            reference.value(), 0, size,
            MYOS_VM_READ | MYOS_VM_WRITE);
        if (mapped != MYOS_STATUS_OK) {
            return mapped;
        }
        auto* const destination = reinterpret_cast<uint8_t*>(
            static_cast<uintptr_t>(scratch_.address()));
        myos_status_t status = B::memory_write(
            destination, source, source_size);
        if (status != MYOS_STATUS_OK) {
            const myos_status_t unmapped = scratch_.unmap();
            if (unmapped != MYOS_STATUS_OK) {
                B::ownership_fault(unmapped);
            }
            return status;
        }
        status = B::memory_write(
            destination + source_size, nullptr, size - source_size);
        if (status != MYOS_STATUS_OK) {
            const myos_status_t unmapped = scratch_.unmap();
            if (unmapped != MYOS_STATUS_OK) {
                B::ownership_fault(unmapped);
            }
            return status;
        }
        return scratch_.unmap();
    }

    [[nodiscard]] auto materialize_module(
        const boot::Module& module,
        Image& output) noexcept -> myos_status_t {
        output.clear();
        if (module.segment_count() == 0
            || module.segment_count() > SegmentCapacity) {
            return MYOS_STATUS_BAD_ARGS;
        }
        output.entry = module.entry();
        for (size_t index = 0; index < module.segment_count(); ++index) {
            boot::Segment segment{};
            if (!module.segment(index, segment)) {
                output.clear();
                return MYOS_STATUS_BAD_ARGS;
            }
            const auto size = rounded(segment.memory_size);
            if (!size.has_value()
                || segment.address == 0
                || (segment.address % MYOS_DEPLOY_PAGE_SIZE) != 0
                || !valid_range(segment.address, size.value())
                || segment.access == 0
                || (segment.access & ~static_cast<myos_word_t>(
                    MYOS_VM_READ | MYOS_VM_WRITE | MYOS_VM_EXECUTE)) != 0
                || ((segment.access & MYOS_VM_WRITE) != 0
                    && (segment.access & MYOS_VM_READ) == 0)
                || ((segment.access & MYOS_VM_WRITE) != 0
                    && (segment.access & MYOS_VM_EXECUTE) != 0)) {
                output.clear();
                return MYOS_STATUS_BAD_ARGS;
            }
            const myos_word_t load_access = segment.access
                | MYOS_VM_READ | MYOS_VM_WRITE;
            LocalSlot memory{};
            myos_status_t status = create_memory(
                size.value(), load_access, memory);
            if (status != MYOS_STATUS_OK
                || (status = populate(memory, segment, size.value()))
                    != MYOS_STATUS_OK) {
                output.clear();
                return status;
            }
            if ((segment.access & MYOS_VM_EXECUTE) != 0) {
                const auto reference = task_.lookup(
                    memory, MYOS_OBJECT_KIND_MEMORY);
                if (!reference.has_value()) {
                    output.clear();
                    return MYOS_STATUS_INVALID_CAP;
                }
                status = B::memory_seal(reference.value());
                if (status != MYOS_STATUS_OK) {
                    output.clear();
                    return status;
                }
            }
            LocalSlot region{};
            status = create_region(
                static_cast<myos_word_t>(segment.address), size.value(),
                segment.access, region);
            if (status != MYOS_STATUS_OK
                || (status = map(
                    region, memory, static_cast<myos_word_t>(segment.address),
                    size.value(), segment.access)) != MYOS_STATUS_OK
            ) {
                output.clear();
                return status;
            }
            if (!output.segments.try_push_back(typename Image::Mapping{
                    .memory = memory,
                    .region = region,
                    .address = segment.address,
                    .size = size.value(),
                    .access = segment.access})) {
                output.clear();
                return MYOS_STATUS_NO_MEMORY;
            }
        }
        return MYOS_STATUS_OK;
    }

    Task& task_;
    BundleLease& bundle_;
    Scratch& scratch_;
};

} // namespace myos::deploy
