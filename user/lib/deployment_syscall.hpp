#pragma once

#include <user/lib/deployment.hpp>
#include <user/lib/syscall.hpp>

namespace myos::cap {

// The deployment adapter is intentionally above raw syscall.hpp.  All
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

    [[nodiscard]] static auto vm_create_region(
        CapRef vspace,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        myos_word_t types,
        myos_word_t rights) noexcept -> SysResult {
        if (!current(vspace)) {
            return bad_args();
        }
        return ::myos::vm_create_region(
            vspace.selector, address, size, access, types, rights);
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

    [[nodiscard]] static auto vm_destroy_region(CapRef region) noexcept
        -> myos_status_t {
        if (!current(region)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::vm_destroy_region(region.selector).status;
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

    [[nodiscard]] static auto vproc_create(
        CapRef pool,
        CapRef vspace,
        CapRef cspace,
        CapRef descriptor,
        myos_word_t offset = 0) noexcept -> SysResult {
        if (!current(pool) || !current(vspace) || !current(cspace)
            || !current(descriptor)) {
            return bad_args();
        }
        return ::myos::vproc_create(
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

    [[nodiscard]] static auto pager_create(
        CapRef pool,
        myos_word_t backing_key,
        myos_word_t max_pages) noexcept -> SysResult {
        if (!current(pool)) {
            return bad_args();
        }
        return ::myos::pager_create(
            pool.selector, backing_key, max_pages);
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

    [[nodiscard]] static auto terminal_observe_bind(
        CapRef target,
        CapRef notification,
        myos_word_t badge) noexcept -> myos_status_t {
        if (!current(target) || !current(notification)) {
            return MYOS_STATUS_BAD_ARGS;
        }
        return ::myos::terminal_observe_bind(
            target.selector, notification.selector, badge).status;
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

template<size_t LocalCapacity, size_t RemoteCapacity>
using DeploymentCaps = BasicDeploymentCaps<
    LocalCapacity, RemoteCapacity, SyscallBackend>;

template<size_t LocalCapacity, size_t RemoteCapacity>
using TaskSpace = deploy::TaskSpace<
    LocalCapacity, RemoteCapacity, SyscallBackend>;

using MappedBundle = deploy::MappedBundle<SyscallBackend>;
using ScratchWindow = deploy::ScratchWindow<SyscallBackend>;

} // namespace myos::cap
