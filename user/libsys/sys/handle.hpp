#pragma once

#include <stddef.h>
#include <concepts>
#include <optional>
#include <utility>
#include <uapi/cap.h>
#include <uapi/abi.h>
#include <sys/syscall.hpp>
#include <expected>
#include <libk/unique_handle.hpp>

namespace sys::cap {

// A selector plus the CSpace in which that selector is installed.  A zero
// CSpace is the caller's current CSpace; nonzero values are borrowed manager
// authorities used only by the remote CAP_CLOSE ABI.
struct CapRef final {
    cap_t selector{};
    cap_t cspace{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return selector != 0;
    }

    [[nodiscard]] friend constexpr auto operator==(
        const CapRef&, const CapRef&) noexcept -> bool = default;
};

template<typename Backend>
concept CapBackend = requires(CapRef reference, status_t status) {
    { Backend::close(reference) } -> std::same_as<status_t>;
    { Backend::ownership_fault(status) } noexcept;
};

// Explicit close can fail and retains ownership; implicit destruction cannot
// report an error, so a failed close stops the process through the backend.
template<CapBackend Backend>
class BasicOwnedCap final {
    struct Drop {
        void operator()(CapRef ref) const noexcept {
            const auto status = Backend::close(ref);
            if (status != STATUS_OK) Backend::ownership_fault(status);
        }
    };
    struct Empty {
        static constexpr auto empty() noexcept -> CapRef { return {}; }
        static constexpr auto is_empty(CapRef ref) noexcept -> bool { return !ref; }
    };
    libk::unique_handle<CapRef, Drop, Empty> ref_{};

public:
    constexpr BasicOwnedCap() noexcept = default;
    constexpr explicit BasicOwnedCap(CapRef ref) noexcept : ref_(ref) {}
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return bool(ref_); }
    [[nodiscard]] constexpr auto reference() const noexcept -> CapRef { return ref_.get(); }
    [[nodiscard]] constexpr auto selector() const noexcept -> cap_t { return ref_.get().selector; }
    [[nodiscard]] constexpr auto cspace() const noexcept -> cap_t { return ref_.get().cspace; }
    [[nodiscard]] constexpr auto release() noexcept -> CapRef { return ref_.release(); }

    [[nodiscard]] auto close() noexcept -> status_t {
        if (!ref_) return STATUS_OK;
        const auto status = Backend::close(ref_.get());
        if (status == STATUS_OK) (void)ref_.release();
        return status;
    }
};

// Capability syscalls and ownership sit above the raw register ABI. All
// CapRef inputs below are current-CSpace authorities except CAP_CLOSE, whose
// explicit destination CSpace is represented by CapRef::cspace.
struct SyscallBackend final {
    [[nodiscard]] static auto close(CapRef reference) noexcept
        -> status_t {
        if (!reference) {
            return STATUS_BAD_ARGS;
        }
        return ::sys::cap_close(
            reference.selector, reference.cspace).status;
    }

    [[noreturn]] static void ownership_fault(
        status_t status) noexcept {
        static_cast<void>(status);
        __builtin_trap();
    }

    [[nodiscard]] static auto resource_create_child(
        CapRef pool,
        word_t memory,
        word_t caps,
        word_t kinds) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::sys::resource_create_child(
            pool.selector, memory, caps, kinds);
    }

    [[nodiscard]] static auto resource_close(CapRef pool) noexcept
        -> status_t {
        if (!current(pool)) {
            return STATUS_BAD_ARGS;
        }
        return ::sys::resource_close(pool.selector).status;
    }

    [[nodiscard]] static auto resource_close_async(CapRef pool, CapRef events, word_t badge) noexcept
        -> status_t {
        if (!current(pool) || !current(events)) return STATUS_BAD_ARGS;
        return ::sys::resource_close_async(pool.selector, events.selector, badge).status;
    }

    [[nodiscard]] static auto typed_delegate(
        CapRef source,
        CapRef destination,
        CapRef descriptor,
        word_t offset = 0) noexcept -> SysResult {
        if (!current(source) || !current(descriptor)
            || destination.cspace != 0) {
            return bad_args();
        }
        return ::sys::cap_typed_delegate(
            source.selector,
            destination.selector,
            descriptor.selector,
            offset);
    }

    [[nodiscard]] static auto duplicate(
        CapRef source,
        CapRef destination,
        word_t rights) noexcept -> SysResult {
        if (!current(source) || !current(destination)) {
            return bad_args();
        }
        return ::sys::cap_duplicate(
            source.selector, destination.selector, rights);
    }

    [[nodiscard]] static auto channel_mint(
        CapRef source,
        CapRef destination,
        word_t badge,
        word_t rights) noexcept -> SysResult {
        if (!current(source) || !current(destination) || badge == 0) {
            return bad_args();
        }
        return ::sys::channel_mint(
            source.selector, destination.selector, badge, rights);
    }

    [[nodiscard]] static auto vspace_create(CapRef pool) noexcept
        -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::sys::vspace_create(pool.selector);
    }

    [[nodiscard]] static auto cspace_create(
        CapRef pool,
        word_t slots,
        word_t pages) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::sys::cspace_create(pool.selector, slots, pages);
    }

    [[nodiscard]] static auto memory_create(
        CapRef pool,
        word_t size,
        word_t access) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::sys::memory_create(pool.selector, size, access);
    }

    [[nodiscard]] static auto memory_create_pager(
        CapRef pool,
        word_t size,
        word_t access,
        CapRef pager) noexcept -> SysResult {
        if (!current(pool) || !current(pager)) {
            return bad_args();
        }
        return ::sys::memory_create_pager(
            pool.selector, size, access, pager.selector);
    }

    [[nodiscard]] static auto memory_seal(CapRef memory) noexcept
        -> status_t {
        if (!current(memory)) {
            return STATUS_BAD_ARGS;
        }
        return ::sys::memory_seal(memory.selector).status;
    }

    [[nodiscard]] static auto memory_populate(CapRef memory,
        word_t page) noexcept -> status_t {
        if (!current(memory)) return STATUS_BAD_ARGS;
        return ::sys::memory_populate(memory.selector, page).status;
    }

    [[nodiscard]] static auto memory_write(
        void* destination,
        const uint8_t* source,
        size_t size) noexcept -> status_t {
        // A null source is the bounded zero-fill form used for BSS/tail
        // population.  The destination remains mandatory even for an empty
        // request so callers cannot accidentally turn a bad address into a
        // successful no-op.
        if (destination == nullptr) {
            return STATUS_BAD_ARGS;
        }
        auto* const bytes = static_cast<uint8_t*>(destination);
        for (size_t index = 0; index < size; ++index) {
            bytes[index] = source == nullptr ? 0 : source[index];
        }
        return STATUS_OK;
    }

    [[nodiscard]] static auto vm_slice(
        CapRef vspace,
        word_t address,
        word_t size,
        word_t access,
        word_t rights) noexcept -> SysResult {
        if (!current(vspace)) {
            return bad_args();
        }
        return ::sys::vm_slice(
            vspace.selector, address, size, access, rights);
    }

    [[nodiscard]] static auto vm_map(
        CapRef region,
        CapRef memory,
        word_t address,
        word_t size,
        word_t object_page,
        word_t access) noexcept -> status_t {
        if (!current(region) || !current(memory)) {
            return STATUS_BAD_ARGS;
        }
        return ::sys::vm_map(
            region.selector, memory.selector, address, size,
            object_page, access).status;
    }

    [[nodiscard]] static auto vm_unmap(
        CapRef region,
        word_t address,
        word_t size) noexcept -> status_t {
        if (!current(region)) {
            return STATUS_BAD_ARGS;
        }
        return ::sys::vm_unmap(region.selector, address, size).status;
    }

    [[nodiscard]] static auto vm_clear(CapRef region) noexcept
        -> status_t {
        if (!current(region)) {
            return STATUS_BAD_ARGS;
        }
        return ::sys::vm_clear(region.selector).status;
    }

    [[nodiscard]] static auto sc_bind(
        CapRef context,
        CapRef thread) noexcept -> status_t {
        if (!current(context) || !current(thread)) {
            return STATUS_BAD_ARGS;
        }
        return ::sys::sc_bind(context.selector, thread.selector).status;
    }

    [[nodiscard]] static auto sc_create(
        CapRef pool,
        CapRef domain,
        word_t budget,
        word_t period,
        word_t urgency,
        word_t home_cpu) noexcept -> SysResult {
        if (!current(pool) || !current(domain)) {
            return bad_args();
        }
        return ::sys::sc_create(
            pool.selector, domain.selector, budget, period,
            urgency, home_cpu);
    }

    [[nodiscard]] static auto thread_create(
        CapRef pool,
        CapRef vspace,
        CapRef cspace,
        CapRef descriptor,
        word_t offset = 0) noexcept -> SysResult {
        if (!current(pool) || !current(vspace) || !current(cspace)
            || !current(descriptor)) {
            return bad_args();
        }
        return ::sys::thread_create(
            pool.selector, vspace.selector, cspace.selector,
            descriptor.selector, offset);
    }

    [[nodiscard]] static auto notification_create(
        CapRef pool,
        word_t badge) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::sys::notification_create(pool.selector, badge);
    }

    [[nodiscard]] static auto notification_take(
        CapRef notification) noexcept -> SysResult {
        if (!current(notification)) {
            return bad_args();
        }
        return ::sys::notification_take(notification.selector);
    }

    [[nodiscard]] static auto channel_create(
        CapRef pool,
        word_t queue,
        word_t words,
        word_t caps,
        word_t relations) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::sys::channel_create(
            pool.selector, queue, words, caps, relations);
    }

    [[nodiscard]] static auto pager_create(CapRef pool) noexcept -> SysResult {
        return current(pool) ? ::sys::pager_create(pool.selector) : bad_args();
    }

    [[nodiscard]] static auto endpoint_create(
        CapRef pool,
        CapRef vspace,
        CapRef cspace,
        CapRef descriptor,
        word_t offset = 0) noexcept -> SysResult {
        if (!current(pool) || !current(vspace) || !current(cspace)
            || !current(descriptor)) {
            return bad_args();
        }
        return ::sys::endpoint_create(
            pool.selector, vspace.selector, cspace.selector,
            descriptor.selector, offset);
    }

    [[nodiscard]] static auto exit_bind(
        CapRef target,
        CapRef notification,
        word_t badge) noexcept -> status_t {
        if (!current(target) || !current(notification)) {
            return STATUS_BAD_ARGS;
        }
        return ::sys::exit_bind(
            target.selector, notification.selector, badge).status;
    }

    [[nodiscard]] static auto execution_start(CapRef target) noexcept
        -> SysResult {
        if (!current(target)) {
            return bad_args();
        }
        return ::sys::execution_start(target.selector);
    }

    [[nodiscard]] static auto exit_query(CapRef target) noexcept
        -> SysResult {
        if (!current(target)) {
            return bad_args();
        }
        return ::sys::exit_query(target.selector);
    }

private:
    [[nodiscard]] static constexpr auto current(CapRef reference) noexcept
        -> bool {
        return static_cast<bool>(reference) && reference.cspace == 0;
    }

    [[nodiscard]] static constexpr auto bad_args() noexcept -> SysResult {
        return SysResult{.status = STATUS_BAD_ARGS};
    }
};

using OwnedCap = BasicOwnedCap<SyscallBackend>;


} // namespace sys::cap

namespace sys {

// Owns a task-local region and its capability references. create() additionally
// owns the new anonymous object; map() only owns the imported capability.
// The caller must stop local pointer users before closing or replacing it.
struct MappedMemory final {
    cap::OwnedCap memory{};
    cap::OwnedCap region{};
    uintptr_t address{};
    size_t size{};

    MappedMemory() noexcept = default;
    MappedMemory(const MappedMemory&) = delete;
    auto operator=(const MappedMemory&) -> MappedMemory& = delete;
    MappedMemory(MappedMemory&& other) noexcept { take(other); }
    auto operator=(MappedMemory&& other) noexcept -> MappedMemory& {
        if (this != &other) {
            require_close();
            take(other);
        }
        return *this;
    }
    ~MappedMemory() noexcept { require_close(); }

    // Return follows PTE invalidation and region retirement. A successful close
    // permits immediate reuse of the virtual range.
    [[nodiscard]] auto close() noexcept -> status_t {
        if (region) {
            const auto status = vm_clear(region.selector()).status;
            if (status != STATUS_OK) return status;
            region = {};
        }
        if (owns_memory_ && memory) {
            const auto status = object_destroy(memory.selector()).status;
            if (status != STATUS_OK) return status;
        }
        memory = {};
        owns_memory_ = false;
        address = 0;
        size = 0;
        return STATUS_OK;
    }

    [[nodiscard]] static auto map(cap_t vspace, cap::OwnedCap&& memory,
        uintptr_t address, size_t size, word_t access) noexcept -> std::expected<MappedMemory, status_t> {
        return map_impl(vspace, std::move(memory), address, size, access, false);
    }

    // The source stays immutable; the first write to each page belongs to this mapping.
    [[nodiscard]] static auto map_private(cap_t vspace, cap::OwnedCap&& source,
        uintptr_t address, size_t size) noexcept -> std::expected<MappedMemory, status_t> {
        return map_impl(vspace, std::move(source), address, size,
            VM_READ | VM_WRITE, false, VM_MAP_PRIVATE);
    }

    [[nodiscard]] static auto create(cap_t pool, cap_t vspace,
        uintptr_t address, size_t size) noexcept -> std::expected<MappedMemory, status_t> {
        const auto memory = memory_create(pool, size, VM_READ | VM_WRITE);
        if (memory.status != STATUS_OK) return std::unexpected(memory.status);
        return map_impl(vspace, cap::OwnedCap{{memory.value, 0}}, address, size,
            VM_READ | VM_WRITE, true);
    }

private:
    bool owns_memory_{};

    void require_close() noexcept {
        const auto status = close();
        if (status != STATUS_OK) cap::SyscallBackend::ownership_fault(status);
    }
    void take(MappedMemory& other) noexcept {
        memory = std::move(other.memory);
        region = std::move(other.region);
        address = std::exchange(other.address, 0);
        size = std::exchange(other.size, 0);
        owns_memory_ = std::exchange(other.owns_memory_, false);
    }
    static auto map_impl(cap_t vspace, cap::OwnedCap&& memory,
        uintptr_t address, size_t size, word_t access,
        bool owns_memory, word_t flags = 0) noexcept -> std::expected<MappedMemory, status_t> {
        MappedMemory result;
        result.memory = std::move(memory);
        result.owns_memory_ = owns_memory;
        const auto region = vm_slice(vspace, address, size, access,
            RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP | RIGHT_DESTROY);
        if (region.status != STATUS_OK) return std::unexpected(region.status);
        result.region = cap::OwnedCap{{region.value, 0}};
        result.address = address;
        result.size = size;
        const auto mapped = vm_map(region.value, result.memory.selector(), address, size, 0, access | flags);
        if (mapped.status != STATUS_OK)
            return std::unexpected(mapped.status);
        return result;
    }

};

} // namespace sys
