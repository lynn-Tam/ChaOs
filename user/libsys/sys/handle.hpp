#pragma once

#include <stddef.h>
#include <concepts>
#include <optional>
#include <utility>
#include <uapi/capability.h>
#include <uapi/status.h>
#include <sys/syscall.hpp>
#include <expected>

namespace myos::cap {

// A selector plus the CSpace in which that selector is installed.  A zero
// CSpace is the caller's current CSpace; nonzero values are borrowed manager
// authorities used only by the remote CAP_CLOSE ABI.
struct CapRef final {
    myos_cap_t selector{};
    myos_cap_t cspace{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return selector != 0;
    }

    [[nodiscard]] friend constexpr auto operator==(
        const CapRef&, const CapRef&) noexcept -> bool = default;
};

template<typename Backend>
concept CapBackend = requires(CapRef reference, myos_status_t status) {
    { Backend::close(reference) } -> std::same_as<myos_status_t>;
    { Backend::ownership_fault(status) } noexcept;
};

// Backend is deliberately static: production userspace gets a direct syscall
// call and host tests inject a fixed fake without a vtable, heap or queue.
template<CapBackend Backend>
class BasicOwnedCap final {
public:
    constexpr BasicOwnedCap() noexcept = default;

    constexpr explicit BasicOwnedCap(CapRef reference) noexcept
        : reference_(reference) {}

    BasicOwnedCap(const BasicOwnedCap&) = delete;
    auto operator=(const BasicOwnedCap&) -> BasicOwnedCap& = delete;

    constexpr BasicOwnedCap(BasicOwnedCap&& other) noexcept
        : reference_(other.release()) {}

    auto operator=(BasicOwnedCap&& other) noexcept -> BasicOwnedCap& {
        if (this == &other) {
            return *this;
        }
        if (reference_) {
            const myos_status_t status = close();
            if (status != MYOS_STATUS_OK) {
                Backend::ownership_fault(status);
            }
        }
        reference_ = other.release();
        return *this;
    }

    ~BasicOwnedCap() noexcept {
        if (!reference_) {
            return;
        }
        // Destruction is permitted one bounded fallback only.  A failed
        // close cannot be queued or silently discarded without losing the
        // selector, so the backend must fail-stop.
        const myos_status_t status = Backend::close(reference_);
        if (status == MYOS_STATUS_OK) {
            reference_ = {};
            return;
        }
        Backend::ownership_fault(status);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(reference_);
    }

    [[nodiscard]] constexpr auto reference() const noexcept -> CapRef {
        return reference_;
    }

    [[nodiscard]] constexpr auto selector() const noexcept -> myos_cap_t {
        return reference_.selector;
    }

    [[nodiscard]] constexpr auto cspace() const noexcept -> myos_cap_t {
        return reference_.cspace;
    }

    // Explicit close retains the reference on every non-OK result.
    [[nodiscard]] auto close() noexcept -> myos_status_t {
        if (!reference_) {
            return MYOS_STATUS_OK;
        }
        const myos_status_t status = Backend::close(reference_);
        if (status == MYOS_STATUS_OK) {
            reference_ = {};
        }
        return status;
    }

    // Transfer ownership without invoking the backend.
    [[nodiscard]] constexpr auto release() noexcept -> CapRef {
        return std::exchange(reference_, CapRef{});
    }

private:
    CapRef reference_{};
};

} // namespace myos::cap

namespace myos::cap {

// Capability syscalls and ownership sit above the raw register ABI. All
// CapRef inputs below are current-CSpace authorities except CAP_CLOSE, whose
// explicit destination CSpace is represented by CapRef::cspace.
struct SyscallBackend final {
    [[nodiscard]] static auto close(CapRef reference) noexcept
        -> myos_status_t {
        if (!reference) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::cap_close(
            reference.selector, reference.cspace).status;
    }

    [[noreturn]] static void ownership_fault(
        myos_status_t status) noexcept {
        static_cast<void>(status);
        __builtin_trap();
    }

    [[nodiscard]] static auto resource_create_child(
        CapRef pool,
        myos_word_t memory,
        myos_word_t caps,
        myos_word_t kinds) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::myos::resource_create_child(
            pool.selector, memory, caps, kinds);
    }

    [[nodiscard]] static auto resource_close(CapRef pool) noexcept
        -> myos_status_t {
        if (!current(pool)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::resource_close(pool.selector).status;
    }

    [[nodiscard]] static auto resource_close_async(CapRef pool, CapRef events, myos_word_t badge) noexcept
        -> myos_status_t {
        if (!current(pool) || !current(events)) return MYOS_STATUS_BAD_ARGS;
        return ::myos::resource_close_async(pool.selector, events.selector, badge).status;
    }

    [[nodiscard]] static auto typed_delegate(
        CapRef source,
        CapRef destination,
        CapRef descriptor,
        myos_word_t offset = 0) noexcept -> SysResult {
        if (!current(source) || !current(descriptor)
            || destination.cspace != 0) {
            return bad_args();
        }
        return ::myos::cap_typed_delegate(
            source.selector,
            destination.selector,
            descriptor.selector,
            offset);
    }

    [[nodiscard]] static auto duplicate(
        CapRef source,
        CapRef destination,
        myos_word_t rights) noexcept -> SysResult {
        if (!current(source) || !current(destination)) {
            return bad_args();
        }
        return ::myos::cap_duplicate(
            source.selector, destination.selector, rights);
    }

    [[nodiscard]] static auto channel_mint(
        CapRef source,
        CapRef destination,
        myos_word_t badge,
        myos_word_t rights) noexcept -> SysResult {
        if (!current(source) || !current(destination) || badge == 0) {
            return bad_args();
        }
        return ::myos::channel_mint(
            source.selector, destination.selector, badge, rights);
    }

    [[nodiscard]] static auto vspace_create(CapRef pool) noexcept
        -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::myos::vspace_create(pool.selector);
    }

    [[nodiscard]] static auto cspace_create(
        CapRef pool,
        myos_word_t slots,
        myos_word_t pages) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::myos::cspace_create(pool.selector, slots, pages);
    }

    [[nodiscard]] static auto memory_create(
        CapRef pool,
        myos_word_t size,
        myos_word_t access) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::myos::memory_create(pool.selector, size, access);
    }

    [[nodiscard]] static auto memory_create_pager(
        CapRef pool,
        myos_word_t size,
        myos_word_t access,
        CapRef pager) noexcept -> SysResult {
        if (!current(pool) || !current(pager)) {
            return bad_args();
        }
        return ::myos::memory_create_pager(
            pool.selector, size, access, pager.selector);
    }

    [[nodiscard]] static auto memory_seal(CapRef memory) noexcept
        -> myos_status_t {
        if (!current(memory)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::memory_seal(memory.selector).status;
    }

    [[nodiscard]] static auto memory_populate(CapRef memory,
        myos_word_t page) noexcept -> myos_status_t {
        if (!current(memory)) return MYOS_STATUS_BAD_ARGS;
        return ::myos::memory_populate(memory.selector, page).status;
    }

    [[nodiscard]] static auto memory_write(
        void* destination,
        const uint8_t* source,
        size_t size) noexcept -> myos_status_t {
        // A null source is the bounded zero-fill form used for BSS/tail
        // population.  The destination remains mandatory even for an empty
        // request so callers cannot accidentally turn a bad address into a
        // successful no-op.
        if (destination == nullptr) {
            return MYOS_STATUS_BAD_ARGS;
        }
        auto* const bytes = static_cast<uint8_t*>(destination);
        for (size_t index = 0; index < size; ++index) {
            bytes[index] = source == nullptr ? 0 : source[index];
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto vm_slice(
        CapRef vspace,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        myos_word_t rights) noexcept -> SysResult {
        if (!current(vspace)) {
            return bad_args();
        }
        return ::myos::vm_slice(
            vspace.selector, address, size, access, rights);
    }

    [[nodiscard]] static auto vm_map(
        CapRef region,
        CapRef memory,
        myos_word_t address,
        myos_word_t size,
        myos_word_t object_page,
        myos_word_t access) noexcept -> myos_status_t {
        if (!current(region) || !current(memory)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::vm_map(
            region.selector, memory.selector, address, size,
            object_page, access).status;
    }

    [[nodiscard]] static auto vm_unmap(
        CapRef region,
        myos_word_t address,
        myos_word_t size) noexcept -> myos_status_t {
        if (!current(region)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::vm_unmap(region.selector, address, size).status;
    }

    [[nodiscard]] static auto vm_clear(CapRef region) noexcept
        -> myos_status_t {
        if (!current(region)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::vm_clear(region.selector).status;
    }

    [[nodiscard]] static auto sc_bind(
        CapRef context,
        CapRef thread) noexcept -> myos_status_t {
        if (!current(context) || !current(thread)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::sc_bind(context.selector, thread.selector).status;
    }

    [[nodiscard]] static auto sc_create(
        CapRef pool,
        CapRef domain,
        myos_word_t budget,
        myos_word_t period,
        myos_word_t urgency,
        myos_word_t home_cpu) noexcept -> SysResult {
        if (!current(pool) || !current(domain)) {
            return bad_args();
        }
        return ::myos::sc_create(
            pool.selector, domain.selector, budget, period,
            urgency, home_cpu);
    }

    [[nodiscard]] static auto thread_create(
        CapRef pool,
        CapRef vspace,
        CapRef cspace,
        CapRef descriptor,
        myos_word_t offset = 0) noexcept -> SysResult {
        if (!current(pool) || !current(vspace) || !current(cspace)
            || !current(descriptor)) {
            return bad_args();
        }
        return ::myos::thread_create(
            pool.selector, vspace.selector, cspace.selector,
            descriptor.selector, offset);
    }

    [[nodiscard]] static auto notification_create(
        CapRef pool,
        myos_word_t badge) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::myos::notification_create(pool.selector, badge);
    }

    [[nodiscard]] static auto notification_take(
        CapRef notification) noexcept -> SysResult {
        if (!current(notification)) {
            return bad_args();
        }
        return ::myos::notification_take(notification.selector);
    }

    [[nodiscard]] static auto channel_create(
        CapRef pool,
        myos_word_t queue,
        myos_word_t words,
        myos_word_t caps,
        myos_word_t relations) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::myos::channel_create(
            pool.selector, queue, words, caps, relations);
    }

    [[nodiscard]] static auto pager_create(CapRef pool) noexcept -> SysResult {
        return current(pool) ? ::myos::pager_create(pool.selector) : bad_args();
    }

    [[nodiscard]] static auto endpoint_create(
        CapRef pool,
        CapRef vspace,
        CapRef cspace,
        CapRef descriptor,
        myos_word_t offset = 0) noexcept -> SysResult {
        if (!current(pool) || !current(vspace) || !current(cspace)
            || !current(descriptor)) {
            return bad_args();
        }
        return ::myos::endpoint_create(
            pool.selector, vspace.selector, cspace.selector,
            descriptor.selector, offset);
    }

    [[nodiscard]] static auto exit_bind(
        CapRef target,
        CapRef notification,
        myos_word_t badge) noexcept -> myos_status_t {
        if (!current(target) || !current(notification)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::exit_bind(
            target.selector, notification.selector, badge).status;
    }

    [[nodiscard]] static auto execution_start(CapRef target) noexcept
        -> SysResult {
        if (!current(target)) {
            return bad_args();
        }
        return ::myos::execution_start(target.selector);
    }

    [[nodiscard]] static auto exit_query(CapRef target) noexcept
        -> SysResult {
        if (!current(target)) {
            return bad_args();
        }
        return ::myos::exit_query(target.selector);
    }

private:
    [[nodiscard]] static constexpr auto current(CapRef reference) noexcept
        -> bool {
        return static_cast<bool>(reference) && reference.cspace == 0;
    }

    [[nodiscard]] static constexpr auto bad_args() noexcept -> SysResult {
        return SysResult{.status = MYOS_STATUS_BAD_ARGS};
    }
};

using OwnedCap = BasicOwnedCap<SyscallBackend>;


} // namespace myos::cap

namespace myos {

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
    [[nodiscard]] auto close() noexcept -> myos_status_t {
        if (region) {
            const auto status = vm_clear(region.selector()).status;
            if (status != MYOS_STATUS_OK) return status;
            region = {};
        }
        if (owns_memory_ && memory) {
            const auto status = object_destroy(memory.selector()).status;
            if (status != MYOS_STATUS_OK) return status;
        }
        memory = {};
        owns_memory_ = false;
        address = 0;
        size = 0;
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] static auto map(myos_cap_t vspace, cap::OwnedCap&& memory,
        uintptr_t address, size_t size, myos_word_t access) noexcept -> std::expected<MappedMemory, myos_status_t> {
        return map_impl(vspace, std::move(memory), address, size, access, false);
    }

    // The source stays immutable; the first write to each page belongs to this mapping.
    [[nodiscard]] static auto map_private(myos_cap_t vspace, cap::OwnedCap&& source,
        uintptr_t address, size_t size) noexcept -> std::expected<MappedMemory, myos_status_t> {
        return map_impl(vspace, std::move(source), address, size,
            MYOS_VM_READ | MYOS_VM_WRITE, false, MYOS_VM_MAP_PRIVATE);
    }

    [[nodiscard]] static auto create(myos_cap_t pool, myos_cap_t vspace,
        uintptr_t address, size_t size) noexcept -> std::expected<MappedMemory, myos_status_t> {
        const auto memory = memory_create(pool, size, MYOS_VM_READ | MYOS_VM_WRITE);
        if (memory.status != MYOS_STATUS_OK) return std::unexpected(memory.status);
        return map_impl(vspace, cap::OwnedCap{{memory.value, 0}}, address, size,
            MYOS_VM_READ | MYOS_VM_WRITE, true);
    }

private:
    bool owns_memory_{};

    void require_close() noexcept {
        const auto status = close();
        if (status != MYOS_STATUS_OK) cap::SyscallBackend::ownership_fault(status);
    }
    void take(MappedMemory& other) noexcept {
        memory = std::move(other.memory);
        region = std::move(other.region);
        address = std::exchange(other.address, 0);
        size = std::exchange(other.size, 0);
        owns_memory_ = std::exchange(other.owns_memory_, false);
    }
    static auto map_impl(myos_cap_t vspace, cap::OwnedCap&& memory,
        uintptr_t address, size_t size, myos_word_t access,
        bool owns_memory, myos_word_t flags = 0) noexcept -> std::expected<MappedMemory, myos_status_t> {
        MappedMemory result;
        result.memory = std::move(memory);
        result.owns_memory_ = owns_memory;
        const auto region = vm_slice(vspace, address, size, access,
            MYOS_RIGHT_MAP | MYOS_RIGHT_PROTECT | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
        if (region.status != MYOS_STATUS_OK) return std::unexpected(region.status);
        result.region = cap::OwnedCap{{region.value, 0}};
        result.address = address;
        result.size = size;
        const auto mapped = vm_map(region.value, result.memory.selector(), address, size, 0, access | flags);
        if (mapped.status != MYOS_STATUS_OK)
            return std::unexpected(mapped.status);
        return result;
    }

};

} // namespace myos
