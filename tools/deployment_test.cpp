#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <libk/assert.hpp>
#include <libk/utility.hpp>
#include <user/lib/deployment.hpp>

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    __builtin_trap();
}
} // namespace libk

namespace {

struct FakeBackend final {
    enum class Op : uint8_t {
        Close,
        Child,
        ResourceClose,
        VSpace,
        CSpace,
        Region,
        Map,
        Unmap,
        Destroy,
    };

    struct Call final {
        Op op;
        myos::cap::CapRef first;
        myos::cap::CapRef second;
        myos_word_t address;
        myos_word_t size;
        myos_word_t access;
        myos_word_t types;
        myos_word_t rights;

        constexpr Call() noexcept : Call(Op::Close) {}

        constexpr Call(
            Op operation,
            myos::cap::CapRef first_reference = {},
            myos::cap::CapRef second_reference = {},
            myos_word_t call_address = 0,
            myos_word_t call_size = 0,
            myos_word_t call_access = 0,
            myos_word_t call_types = 0,
            myos_word_t call_rights = 0) noexcept
            : op(operation),
              first(first_reference),
              second(second_reference),
              address(call_address),
              size(call_size),
              access(call_access),
              types(call_types),
              rights(call_rights) {}
    };

    static inline Call calls[128]{};
    static inline size_t call_count{};
    static inline myos_status_t next_close{MYOS_STATUS_OK};
    static inline myos_status_t next_pool_close{MYOS_STATUS_OK};
    static inline myos_status_t next_child{MYOS_STATUS_OK};
    static inline myos_status_t next_resource_close{MYOS_STATUS_OK};
    static inline myos_status_t next_vspace{MYOS_STATUS_OK};
    static inline myos_status_t next_cspace{MYOS_STATUS_OK};
    static inline myos_status_t next_region{MYOS_STATUS_OK};
    static inline myos_status_t next_map{MYOS_STATUS_OK};
    static inline myos_status_t next_unmap{MYOS_STATUS_OK};
    static inline myos_status_t next_destroy{MYOS_STATUS_OK};
    static inline myos_cap_t next_cap{100};
    static inline size_t fault_count{};
    static inline jmp_buf* fault_target{};

    static void reset() noexcept {
        call_count = 0;
        next_close = MYOS_STATUS_OK;
        next_pool_close = MYOS_STATUS_OK;
        next_child = MYOS_STATUS_OK;
        next_resource_close = MYOS_STATUS_OK;
        next_vspace = MYOS_STATUS_OK;
        next_cspace = MYOS_STATUS_OK;
        next_region = MYOS_STATUS_OK;
        next_map = MYOS_STATUS_OK;
        next_unmap = MYOS_STATUS_OK;
        next_destroy = MYOS_STATUS_OK;
        next_cap = 100;
        fault_count = 0;
        fault_target = nullptr;
    }

    static void record(Call call) noexcept {
        if (call_count < sizeof(calls) / sizeof(calls[0])) {
            calls[call_count++] = call;
        }
    }

    [[nodiscard]] static auto consume(myos_status_t& status) noexcept
        -> myos_status_t {
        const myos_status_t result = status;
        status = MYOS_STATUS_OK;
        return result;
    }

    [[nodiscard]] static auto close(
        myos::cap::CapRef reference) noexcept -> myos_status_t {
        record(Call{Op::Close, reference});
        return reference.selector == 10
            ? consume(next_pool_close)
            : consume(next_close);
    }

    [[noreturn]] static void ownership_fault(myos_status_t) noexcept {
        ++fault_count;
        if (fault_target != nullptr) {
            longjmp(*fault_target, 1);
        }
        for (;;) {}
    }

    [[nodiscard]] static auto resource_create_child(
        myos::cap::CapRef pool,
        myos_word_t,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        record(Call{Op::Child, pool});
        const myos_status_t status = consume(next_child);
        return {status, status == MYOS_STATUS_OK ? myos_word_t{10} : 0, 0};
    }

    [[nodiscard]] static auto resource_close(
        myos::cap::CapRef pool) noexcept -> myos_status_t {
        record(Call{Op::ResourceClose, pool});
        return consume(next_resource_close);
    }

    [[nodiscard]] static auto vspace_create(
        myos::cap::CapRef pool) noexcept -> myos::SysResult {
        record(Call{Op::VSpace, pool});
        const myos_status_t status = consume(next_vspace);
        return {status, status == MYOS_STATUS_OK ? myos_word_t{11} : 0, 0};
    }

    [[nodiscard]] static auto cspace_create(
        myos::cap::CapRef pool,
        myos_word_t,
        myos_word_t) noexcept -> myos::SysResult {
        record(Call{Op::CSpace, pool});
        const myos_status_t status = consume(next_cspace);
        return {status, status == MYOS_STATUS_OK ? myos_word_t{12} : 0, 0};
    }

    [[nodiscard]] static auto vm_create_region(
        myos::cap::CapRef vspace,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        myos_word_t types,
        myos_word_t rights) noexcept -> myos::SysResult {
        record(Call{Op::Region, vspace, {}, address, size,
            access, types, rights});
        const myos_status_t status = consume(next_region);
        return {status, status == MYOS_STATUS_OK ? next_cap++ : 0, 0};
    }

    [[nodiscard]] static auto vm_map(
        myos::cap::CapRef region,
        myos::cap::CapRef memory,
        myos_word_t address,
        myos_word_t size,
        myos_word_t,
        myos_word_t) noexcept -> myos_status_t {
        record(Call{Op::Map, region, memory, address, size});
        return consume(next_map);
    }

    [[nodiscard]] static auto vm_unmap(
        myos::cap::CapRef region,
        myos_word_t address,
        myos_word_t size) noexcept -> myos_status_t {
        record(Call{Op::Unmap, region, {}, address, size});
        return consume(next_unmap);
    }

    [[nodiscard]] static auto vm_destroy_region(
        myos::cap::CapRef region) noexcept -> myos_status_t {
        record(Call{Op::Destroy, region});
        return consume(next_destroy);
    }
};

using Space = myos::deploy::TaskSpace<3, 3, FakeBackend>;
using Owner = myos::cap::BasicOwnedCap<FakeBackend>;
using Bundle = myos::deploy::MappedBundle<FakeBackend>;
using Scratch = myos::deploy::ScratchWindow<FakeBackend>;

static_assert(myos::deploy::Backend<FakeBackend>);
static_assert(!libk::is_copy_constructible_v<Space>);

[[nodiscard]] auto has_call(
    FakeBackend::Op op,
    size_t index,
    myos_cap_t selector = 0,
    myos_cap_t cspace = 0) noexcept -> bool {
    if (index >= FakeBackend::call_count
        || FakeBackend::calls[index].op != op) {
        return false;
    }
    if (selector == 0) {
        return true;
    }
    return FakeBackend::calls[index].first
        == myos::cap::CapRef{selector, cspace};
}

[[nodiscard]] auto count_calls(FakeBackend::Op op) noexcept -> size_t {
    size_t count = 0;
    for (size_t index = 0; index < FakeBackend::call_count; ++index) {
        if (FakeBackend::calls[index].op == op) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] auto region_call_matches(
    size_t index,
    myos_word_t access,
    myos_word_t types,
    myos_word_t rights) noexcept -> bool {
    return index < FakeBackend::call_count
        && FakeBackend::calls[index].op == FakeBackend::Op::Region
        && FakeBackend::calls[index].access == access
        && FakeBackend::calls[index].types == types
        && FakeBackend::calls[index].rights == rights;
}

[[nodiscard]] auto open_space(Space& space) noexcept -> bool {
    return space.open(
        myos::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        == MYOS_STATUS_OK;
}

[[nodiscard]] auto test_taskspace_lifecycle() noexcept -> bool {
    FakeBackend::reset();
    Space space{};
    if (!open_space(space)
        || space.phase() != myos::deploy::Phase::Open
        || !space.vspace_slot().valid()
        || space.vspace_slot().index != 0
        || !space.manager_slot().is_manager()
        || !space.lookup(space.vspace_slot(), MYOS_OBJECT_KIND_VSPACE)
        || !space.lookup(space.manager_slot(), MYOS_OBJECT_KIND_CSPACE)
        || space.close_slot(space.manager_slot()) != MYOS_STATUS_BAD_RIGHTS) {
        return false;
    }
    return space.close() == MYOS_STATUS_OK
        && space.phase() == myos::deploy::Phase::Closed
        && has_call(FakeBackend::Op::ResourceClose, 5, 10, 0)
        && has_call(FakeBackend::Op::Close, 6, 10, 0);
}

[[nodiscard]] auto test_creation_failures_strong_close() noexcept -> bool {
    FakeBackend::reset();
    {
        Space space{};
        FakeBackend::next_child = MYOS_STATUS_BUSY;
        if (space.open({1, 0}, 4096, 64, 0x100, 16, 2)
                != MYOS_STATUS_BUSY
            || space.phase() != myos::deploy::Phase::Closed) {
            return false;
        }
    }
    FakeBackend::reset();
    {
        Space space{};
        FakeBackend::next_vspace = MYOS_STATUS_BUSY;
        if (space.open({1, 0}, 4096, 64, 0x100, 16, 2)
                != MYOS_STATUS_BUSY
            || space.phase() != myos::deploy::Phase::Closed) {
            return false;
        }
    }
    FakeBackend::reset();
    {
        Space space{};
        FakeBackend::next_cspace = MYOS_STATUS_BUSY;
        if (space.open({1, 0}, 4096, 64, 0x100, 16, 2)
                != MYOS_STATUS_BUSY
            || space.phase() != myos::deploy::Phase::Closed) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto test_resource_close_retry() noexcept -> bool {
    FakeBackend::reset();
    Space space{};
    if (!open_space(space)) {
        return false;
    }
    FakeBackend::next_close = MYOS_STATUS_BUSY;
    if (space.close() != MYOS_STATUS_BUSY
        || space.phase() != myos::deploy::Phase::Draining
        || count_calls(FakeBackend::Op::ResourceClose) != 0) {
        return false;
    }
    if (space.close() != MYOS_STATUS_OK
        || space.phase() != myos::deploy::Phase::Closed) {
        return false;
    }

    FakeBackend::reset();
    Space partial{};
    FakeBackend::next_cspace = MYOS_STATUS_BUSY;
    FakeBackend::next_resource_close = MYOS_STATUS_BUSY;
    if (partial.open({1, 0}, 4096, 64, 0x100, 16, 2)
            != MYOS_STATUS_BUSY
        || partial.phase() != myos::deploy::Phase::ResourceClosing
        || partial.close() != MYOS_STATUS_OK
        || partial.phase() != myos::deploy::Phase::Closed) {
        return false;
    }

    FakeBackend::reset();
    Space drain_failure{};
    FakeBackend::next_cspace = MYOS_STATUS_BUSY;
    FakeBackend::next_close = MYOS_STATUS_BUSY;
    if (drain_failure.open({1, 0}, 4096, 64, 0x100, 16, 2)
            != MYOS_STATUS_BUSY
        || drain_failure.phase() != myos::deploy::Phase::Draining
        || !drain_failure.pool()
        || drain_failure.pool().value() != myos::cap::CapRef{10, 0}
        || FakeBackend::call_count != 4
        || count_calls(FakeBackend::Op::ResourceClose) != 0
        || !has_call(FakeBackend::Op::Close, 3, 11, 0)
        || drain_failure.close() != MYOS_STATUS_OK
        || drain_failure.phase() != myos::deploy::Phase::Closed
        || FakeBackend::call_count != 7
        || !has_call(FakeBackend::Op::Close, 4, 11, 0)
        || !has_call(FakeBackend::Op::ResourceClose, 5, 10, 0)
        || !has_call(FakeBackend::Op::Close, 6, 10, 0)) {
        return false;
    }

    FakeBackend::reset();
    Space pool_close_failure{};
    FakeBackend::next_cspace = MYOS_STATUS_BUSY;
    FakeBackend::next_pool_close = MYOS_STATUS_BUSY;
    if (pool_close_failure.open({1, 0}, 4096, 64, 0x100, 16, 2)
            != MYOS_STATUS_BUSY
        || pool_close_failure.phase() != myos::deploy::Phase::ResourceClosed
        || !pool_close_failure.pool()
        || pool_close_failure.pool().value() != myos::cap::CapRef{10, 0}
        || FakeBackend::call_count != 6
        || !has_call(FakeBackend::Op::ResourceClose, 4, 10, 0)
        || !has_call(FakeBackend::Op::Close, 5, 10, 0)
        || pool_close_failure.close() != MYOS_STATUS_OK
        || pool_close_failure.phase() != myos::deploy::Phase::Closed
        || FakeBackend::call_count != 7
        || !has_call(FakeBackend::Op::Close, 6, 10, 0)) {
        return false;
    }

    FakeBackend::reset();
    Space resource{};
    if (!open_space(resource)) {
        return false;
    }
    FakeBackend::next_resource_close = MYOS_STATUS_BUSY;
    if (resource.close() != MYOS_STATUS_BUSY
        || resource.phase() != myos::deploy::Phase::ResourceClosing) {
        return false;
    }
    if (resource.close() != MYOS_STATUS_OK
        || resource.phase() != myos::deploy::Phase::Closed) {
        return false;
    }

    FakeBackend::reset();
    Space second{};
    if (!open_space(second)) {
        return false;
    }
    FakeBackend::next_pool_close = MYOS_STATUS_BUSY;
    if (second.close() != MYOS_STATUS_BUSY
        || second.phase() != myos::deploy::Phase::ResourceClosed
        || second.close() != MYOS_STATUS_OK
        || second.phase() != myos::deploy::Phase::Closed) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_slot_identity_tombstone_and_move() noexcept -> bool {
    FakeBackend::reset();
    Space space{};
    if (!open_space(space)) {
        return false;
    }
    Owner memory{{20, 0}};
    Owner notification{{21, 0}};
    const auto memory_slot = space.adopt_local(
        libk::move(memory), MYOS_OBJECT_KIND_MEMORY);
    const auto notification_slot = space.adopt_local(
        libk::move(notification), MYOS_OBJECT_KIND_NOTIFICATION);
    if (!memory_slot || !notification_slot
        || memory_slot->index != 1 || notification_slot->index != 2
        || space.local_cumulative() != 3
        || space.lookup(*memory_slot, MYOS_OBJECT_KIND_NOTIFICATION)
        || space.lookup(
            myos::deploy::LocalSlot{
                .pool = 999, .index = memory_slot->index,
                .kind = MYOS_OBJECT_KIND_MEMORY},
            MYOS_OBJECT_KIND_MEMORY)
        || space.close_slot(*memory_slot) != MYOS_STATUS_OK
        || space.lookup(*memory_slot, MYOS_OBJECT_KIND_MEMORY)) {
        return false;
    }
    Owner overflow{{22, 0}};
    if (space.adopt_local(libk::move(overflow), MYOS_OBJECT_KIND_MEMORY)
        || !overflow
        || space.local_cumulative() != 3) {
        return false;
    }
    if (overflow.close() != MYOS_STATUS_OK) {
        return false;
    }
    Space moved{libk::move(space)};
    if (space.phase() != myos::deploy::Phase::Closed
        || moved.lookup(*notification_slot, MYOS_OBJECT_KIND_NOTIFICATION)
            .value()
            != myos::cap::CapRef{21, 0}
        || moved.close_slot(*notification_slot) != MYOS_STATUS_OK) {
        return false;
    }
    Space assigned{};
    assigned = libk::move(moved);
    return moved.phase() == myos::deploy::Phase::Closed
        && assigned.lookup(
            assigned.vspace_slot(), MYOS_OBJECT_KIND_VSPACE)
        && assigned.close() == MYOS_STATUS_OK;
}

[[nodiscard]] auto test_destructor_rejects_open_space() noexcept -> bool {
    FakeBackend::reset();
    jmp_buf target{};
    FakeBackend::fault_target = &target;
    if (setjmp(target) == 0) {
        Space space{};
        if (!open_space(space)) {
            return false;
        }
    }
    FakeBackend::fault_target = nullptr;
    return FakeBackend::fault_count == 1
        && count_calls(FakeBackend::Op::ResourceClose) == 0;
}

alignas(4096) uint8_t bundle_bytes[4096]{};

void put_bundle(
    size_t offset,
    uint64_t value,
    size_t width) noexcept {
    for (size_t byte = 0; byte < width; ++byte) {
        bundle_bytes[offset + byte] = static_cast<uint8_t>(
            value >> (byte * 8));
    }
}

[[nodiscard]] auto make_valid_bundle() noexcept -> size_t {
    constexpr size_t modules_offset = MYOS_BOOT_HEADER_SIZE;
    constexpr size_t segments_offset =
        modules_offset + MYOS_BOOT_MODULE_SIZE;
    constexpr size_t name_offset =
        segments_offset + 2 * MYOS_BOOT_SEGMENT_SIZE;
    constexpr size_t image_offset = name_offset + 8;
    constexpr size_t total_size = image_offset + 8;
    for (size_t index = 0; index < total_size; ++index) {
        bundle_bytes[index] = 0;
    }
    put_bundle(0, MYOS_BOOT_MAGIC, 8);
    put_bundle(8, MYOS_BOOT_MAJOR, 2);
    put_bundle(10, MYOS_BOOT_MINOR, 2);
    put_bundle(12, MYOS_BOOT_HEADER_SIZE, 4);
    put_bundle(16, total_size, 8);
    put_bundle(24, MYOS_BOOT_ARCH_RISCV64, 4);
    put_bundle(28, MYOS_BOOT_ABI_RISCV_LP64, 4);
    put_bundle(40, modules_offset, 8);
    put_bundle(48, 1, 4);
    put_bundle(56, segments_offset, 8);
    put_bundle(64, 1, 4);
    put_bundle(modules_offset, name_offset, 8);
    put_bundle(modules_offset + 8, 1, 4);
    put_bundle(modules_offset + 12, MYOS_BOOT_MODULE_BOOTABLE, 4);
    put_bundle(modules_offset + 16, image_offset, 8);
    put_bundle(modules_offset + 24, 4, 8);
    put_bundle(modules_offset + 32, 0x200000, 8);
    put_bundle(modules_offset + 44, 1, 4);
    bundle_bytes[name_offset] = 'x';
    for (size_t index = 0; index < 4; ++index) {
        bundle_bytes[image_offset + index] = static_cast<uint8_t>(index + 1);
    }
    put_bundle(segments_offset, 0x200000, 8);
    put_bundle(segments_offset + 8, image_offset, 8);
    put_bundle(segments_offset + 16, 4, 8);
    put_bundle(segments_offset + 24, 0x1000, 8);
    put_bundle(segments_offset + 32, 0x1000, 8);
    put_bundle(
        segments_offset + 40,
        MYOS_BOOT_SEGMENT_READ | MYOS_BOOT_SEGMENT_EXECUTE,
        4);
    return total_size;
}

[[nodiscard]] auto test_bundle_cleanup_retry() noexcept -> bool {
    FakeBackend::reset();
    Bundle bundle{};
    const myos::deploy::Window window{
        reinterpret_cast<myos_word_t>(bundle_bytes), sizeof(bundle_bytes)};
    const myos_status_t opened = bundle.open(
        {1, 0}, {2, 0}, window, sizeof(bundle_bytes));
    if (opened != MYOS_STATUS_BAD_ARGS
        || bundle.phase() != myos::deploy::LeasePhase::Mapped
        || bundle.view() != nullptr) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_BUSY;
    if (bundle.close() != MYOS_STATUS_BUSY
        || bundle.phase() != myos::deploy::LeasePhase::Unmapping) {
        return false;
    }
    FakeBackend::next_destroy = MYOS_STATUS_BUSY;
    if (bundle.close() != MYOS_STATUS_BUSY
        || bundle.phase() != myos::deploy::LeasePhase::Destroying) {
        return false;
    }
    FakeBackend::next_close = MYOS_STATUS_BUSY;
    if (bundle.close() != MYOS_STATUS_BUSY
        || bundle.phase() != myos::deploy::LeasePhase::Closing) {
        return false;
    }
    return bundle.close() == MYOS_STATUS_OK
        && bundle.phase() == myos::deploy::LeasePhase::Closed;
}

[[nodiscard]] auto test_lease_create_and_map_failures() noexcept -> bool {
    const myos::deploy::Window window{0x300000, 0x2000};

    FakeBackend::reset();
    Bundle create_failure{};
    FakeBackend::next_region = MYOS_STATUS_BUSY;
    if (create_failure.open({1, 0}, {2, 0}, window, 0x1000)
            != MYOS_STATUS_BUSY
        || create_failure.phase() != myos::deploy::LeasePhase::Empty
        || FakeBackend::call_count != 1
        || !region_call_matches(
            0, MYOS_VM_READ, MYOS_VM_NORMAL,
            MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY)
        || create_failure.close() != MYOS_STATUS_OK) {
        return false;
    }

    FakeBackend::reset();
    Bundle map_failure{};
    FakeBackend::next_map = MYOS_STATUS_BUSY;
    if (map_failure.open({1, 0}, {2, 0}, window, 0x1000)
            != MYOS_STATUS_BUSY
        || map_failure.phase() != myos::deploy::LeasePhase::Ready
        || map_failure.close() != MYOS_STATUS_OK
        || FakeBackend::call_count != 4
        || !has_call(FakeBackend::Op::Destroy, 2, 100, 0)
        || !has_call(FakeBackend::Op::Close, 3, 100, 0)) {
        return false;
    }

    FakeBackend::reset();
    Scratch scratch_create_failure{};
    FakeBackend::next_region = MYOS_STATUS_BUSY;
    if (scratch_create_failure.open({1, 0}, window) != MYOS_STATUS_BUSY
        || scratch_create_failure.phase() != myos::deploy::LeasePhase::Empty
        || !region_call_matches(
            0, MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_NORMAL,
            MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY)) {
        return false;
    }

    FakeBackend::reset();
    Scratch scratch_map_failure{};
    if (scratch_map_failure.open({1, 0}, window) != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::next_map = MYOS_STATUS_BUSY;
    if (scratch_map_failure.map({2, 0}, 0, 0x1000, MYOS_VM_READ)
            != MYOS_STATUS_BUSY
        || scratch_map_failure.phase() != myos::deploy::LeasePhase::Ready
        || scratch_map_failure.close() != MYOS_STATUS_OK
        || FakeBackend::call_count != 4) {
        return false;
    }
    return has_call(FakeBackend::Op::Destroy, 2, 100, 0)
        && has_call(FakeBackend::Op::Close, 3, 100, 0);
}

[[nodiscard]] auto test_pending_cleanup_is_committed() noexcept -> bool {
    FakeBackend::reset();
    Bundle bundle{};
    const myos::deploy::Window window{
        reinterpret_cast<myos_word_t>(bundle_bytes), 0x1000};
    const size_t bundle_size = make_valid_bundle();
    if (bundle.open({1, 0}, {2, 0}, window, bundle_size)
            != MYOS_STATUS_OK
        || bundle.view() == nullptr) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_PENDING;
    FakeBackend::next_destroy = MYOS_STATUS_PENDING;
    if (bundle.close() != MYOS_STATUS_OK
        || bundle.phase() != myos::deploy::LeasePhase::Closed
        || FakeBackend::call_count != 5
        || !has_call(FakeBackend::Op::Unmap, 2, 100, 0)
        || !has_call(FakeBackend::Op::Destroy, 3, 100, 0)
        || !has_call(FakeBackend::Op::Close, 4, 100, 0)) {
        return false;
    }
    return bundle.view() == nullptr;
}

[[nodiscard]] auto test_pending_phase_retries() noexcept -> bool {
    const myos::deploy::Window bundle_window{
        reinterpret_cast<myos_word_t>(bundle_bytes), 0x1000};

    FakeBackend::reset();
    Bundle bundle{};
    if (bundle.open({1, 0}, {2, 0}, bundle_window, make_valid_bundle())
            != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_PENDING;
    FakeBackend::next_destroy = MYOS_STATUS_BUSY;
    if (bundle.close() != MYOS_STATUS_BUSY
        || bundle.phase() != myos::deploy::LeasePhase::Destroying
        || bundle.view() != nullptr
        || FakeBackend::call_count != 4
        || count_calls(FakeBackend::Op::Unmap) != 1
        || count_calls(FakeBackend::Op::Destroy) != 1
        || FakeBackend::calls[2].size != 0x1000
        || FakeBackend::calls[3].size != 0) {
        return false;
    }
    if (bundle.close() != MYOS_STATUS_OK
        || bundle.phase() != myos::deploy::LeasePhase::Closed
        || FakeBackend::call_count != 6
        || count_calls(FakeBackend::Op::Unmap) != 1
        || count_calls(FakeBackend::Op::Destroy) != 2
        || count_calls(FakeBackend::Op::Close) != 1
        || !has_call(FakeBackend::Op::Destroy, 4, 100, 0)
        || !has_call(FakeBackend::Op::Close, 5, 100, 0)) {
        return false;
    }

    FakeBackend::reset();
    Bundle close_retry{};
    if (close_retry.open(
            {1, 0}, {2, 0}, bundle_window, make_valid_bundle())
            != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_PENDING;
    FakeBackend::next_destroy = MYOS_STATUS_PENDING;
    FakeBackend::next_close = MYOS_STATUS_BUSY;
    if (close_retry.close() != MYOS_STATUS_BUSY
        || close_retry.phase() != myos::deploy::LeasePhase::Closing
        || FakeBackend::call_count != 5) {
        return false;
    }
    if (close_retry.close() != MYOS_STATUS_OK
        || close_retry.phase() != myos::deploy::LeasePhase::Closed
        || FakeBackend::call_count != 6
        || count_calls(FakeBackend::Op::Unmap) != 1
        || count_calls(FakeBackend::Op::Destroy) != 1
        || count_calls(FakeBackend::Op::Close) != 2) {
        return false;
    }

    FakeBackend::reset();
    Scratch scratch{};
    if (scratch.open({1, 0}, {0x500000, 0x1000}) != MYOS_STATUS_OK
        || scratch.map({2, 0}, 0, 0x1000, MYOS_VM_READ) != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_PENDING;
    FakeBackend::next_destroy = MYOS_STATUS_BUSY;
    if (scratch.close() != MYOS_STATUS_BUSY
        || scratch.phase() != myos::deploy::LeasePhase::Destroying
        || FakeBackend::call_count != 4
        || count_calls(FakeBackend::Op::Unmap) != 1
        || count_calls(FakeBackend::Op::Destroy) != 1
        || FakeBackend::calls[2].size != 0x1000) {
        return false;
    }
    if (scratch.close() != MYOS_STATUS_OK
        || scratch.phase() != myos::deploy::LeasePhase::Closed
        || FakeBackend::call_count != 6
        || count_calls(FakeBackend::Op::Unmap) != 1
        || count_calls(FakeBackend::Op::Destroy) != 2
        || count_calls(FakeBackend::Op::Close) != 1) {
        return false;
    }

    FakeBackend::reset();
    Scratch scratch_close_retry{};
    if (scratch_close_retry.open({1, 0}, {0x500000, 0x1000})
            != MYOS_STATUS_OK
        || scratch_close_retry.map({2, 0}, 0, 0x1000, MYOS_VM_READ)
            != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_PENDING;
    FakeBackend::next_destroy = MYOS_STATUS_PENDING;
    FakeBackend::next_close = MYOS_STATUS_BUSY;
    if (scratch_close_retry.close() != MYOS_STATUS_BUSY
        || scratch_close_retry.phase() != myos::deploy::LeasePhase::Closing
        || FakeBackend::call_count != 5
        || scratch_close_retry.close() != MYOS_STATUS_OK
        || scratch_close_retry.phase() != myos::deploy::LeasePhase::Closed
        || FakeBackend::call_count != 6
        || count_calls(FakeBackend::Op::Unmap) != 1
        || count_calls(FakeBackend::Op::Destroy) != 1
        || count_calls(FakeBackend::Op::Close) != 2) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_move_preserves_cleanup_phase_and_view() noexcept
    -> bool {
    FakeBackend::reset();
    Bundle bundle{};
    const size_t bundle_size = make_valid_bundle();
    const myos::deploy::Window window{
        reinterpret_cast<myos_word_t>(bundle_bytes), 0x1000};
    if (bundle.open({1, 0}, {2, 0}, window, bundle_size)
            != MYOS_STATUS_OK
        || bundle.view() == nullptr) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_BUSY;
    if (bundle.close() != MYOS_STATUS_BUSY
        || bundle.phase() != myos::deploy::LeasePhase::Unmapping
        || bundle.view() != nullptr) {
        return false;
    }
    Bundle moved{libk::move(bundle)};
    if (bundle.phase() != myos::deploy::LeasePhase::Empty
        || moved.phase() != myos::deploy::LeasePhase::Unmapping
        || moved.close() != MYOS_STATUS_OK
        || moved.phase() != myos::deploy::LeasePhase::Closed) {
        return false;
    }

    FakeBackend::reset();
    Scratch scratch{};
    if (scratch.open({1, 0}, {0x500000, 0x1000}) != MYOS_STATUS_OK
        || scratch.map({2, 0}, 0, 0x1000, MYOS_VM_READ)
            != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_BUSY;
    if (scratch.close() != MYOS_STATUS_BUSY
        || scratch.phase() != myos::deploy::LeasePhase::Unmapping) {
        return false;
    }
    Scratch scratch_moved{libk::move(scratch)};
    return scratch_moved.phase() == myos::deploy::LeasePhase::Unmapping
        && scratch_moved.close() == MYOS_STATUS_OK
        && scratch_moved.phase() == myos::deploy::LeasePhase::Closed;
}

[[nodiscard]] auto test_move_assignment_preserves_cleanup() noexcept -> bool {
    const myos::deploy::Window bundle_window{
        reinterpret_cast<myos_word_t>(bundle_bytes), 0x1000};

    FakeBackend::reset();
    Bundle source{};
    if (source.open({1, 0}, {2, 0}, bundle_window, make_valid_bundle())
            != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_BUSY;
    if (source.close() != MYOS_STATUS_BUSY
        || source.phase() != myos::deploy::LeasePhase::Unmapping) {
        return false;
    }
    Bundle target{};
    target = libk::move(source);
    if (source.phase() != myos::deploy::LeasePhase::Empty
        || target.phase() != myos::deploy::LeasePhase::Unmapping
        || target.view() != nullptr
        || FakeBackend::call_count != 3) {
        return false;
    }
    if (target.close() != MYOS_STATUS_OK
        || target.phase() != myos::deploy::LeasePhase::Closed
        || FakeBackend::call_count != 6
        || count_calls(FakeBackend::Op::Unmap) != 2
        || count_calls(FakeBackend::Op::Destroy) != 1
        || count_calls(FakeBackend::Op::Close) != 1
        || FakeBackend::calls[2].size != 0x1000
        || FakeBackend::calls[3].size != 0x1000
        || !has_call(FakeBackend::Op::Destroy, 4, 100, 0)
        || !has_call(FakeBackend::Op::Close, 5, 100, 0)) {
        return false;
    }

    FakeBackend::reset();
    Scratch scratch_source{};
    if (scratch_source.open({1, 0}, {0x600000, 0x1000}) != MYOS_STATUS_OK
        || scratch_source.map({2, 0}, 0, 0x1000, MYOS_VM_READ)
            != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_BUSY;
    if (scratch_source.close() != MYOS_STATUS_BUSY
        || scratch_source.phase() != myos::deploy::LeasePhase::Unmapping) {
        return false;
    }
    Scratch scratch_target{};
    scratch_target = libk::move(scratch_source);
    if (scratch_source.phase() != myos::deploy::LeasePhase::Empty
        || scratch_target.phase() != myos::deploy::LeasePhase::Unmapping
        || FakeBackend::call_count != 3) {
        return false;
    }
    return scratch_target.close() == MYOS_STATUS_OK
        && scratch_target.phase() == myos::deploy::LeasePhase::Closed
        && FakeBackend::call_count == 6
        && count_calls(FakeBackend::Op::Unmap) == 2
        && count_calls(FakeBackend::Op::Destroy) == 1
        && count_calls(FakeBackend::Op::Close) == 1
        && FakeBackend::calls[2].size == 0x1000
        && FakeBackend::calls[3].size == 0x1000
        && has_call(FakeBackend::Op::Destroy, 4, 100, 0)
        && has_call(FakeBackend::Op::Close, 5, 100, 0);
}

[[nodiscard]] auto test_scratch_unmap_blocks_then_reuses() noexcept -> bool {
    FakeBackend::reset();
    Scratch scratch{};
    if (scratch.open({1, 0}, {0x400000, 0x2000}) != MYOS_STATUS_OK
        || scratch.map({2, 0}, 0, 0x1000, MYOS_VM_READ | MYOS_VM_WRITE)
            != MYOS_STATUS_OK) {
        return false;
    }
    FakeBackend::next_unmap = MYOS_STATUS_BUSY;
    if (scratch.unmap() != MYOS_STATUS_BUSY
        || scratch.reusable()
        || scratch.address() != 0
        || scratch.map({3, 0}, 1, 0x1000, MYOS_VM_READ)
            != MYOS_STATUS_BAD_ARGS) {
        return false;
    }
    if (scratch.unmap() != MYOS_STATUS_OK
        || !scratch.reusable()
        || scratch.map({3, 0}, 1, 0x1000, MYOS_VM_READ)
            != MYOS_STATUS_OK) {
        return false;
    }
    return scratch.close() == MYOS_STATUS_OK
        && scratch.phase() == myos::deploy::LeasePhase::Closed;
}

[[nodiscard]] auto test_lease_destructor_fault() noexcept -> bool {
    FakeBackend::reset();
    jmp_buf target{};
    FakeBackend::fault_target = &target;
    if (setjmp(target) == 0) {
        Scratch scratch{};
        if (scratch.open({1, 0}, {0x600000, 0x1000}) != MYOS_STATUS_OK) {
            return false;
        }
        FakeBackend::next_destroy = MYOS_STATUS_BUSY;
    }
    FakeBackend::fault_target = nullptr;
    return FakeBackend::fault_count == 1;
}

[[nodiscard]] auto test_window_checks() noexcept -> bool {
    using myos::deploy::Window;
    if (Window{0x1001, 0x1000}.valid()
        || Window{0x1000, 0x1001}.valid()
        || Window{~myos_word_t{} - 0xfff, 0x2000}.valid()
        || !myos::deploy::windows_disjoint(
            Window{0x1000, 0x1000}, Window{0x2000, 0x1000})
        || myos::deploy::windows_disjoint(
            Window{0x1000, 0x2000}, Window{0x2000, 0x1000})) {
        return false;
    }
    FakeBackend::reset();
    Bundle bundle{};
    if (bundle.open(
            {1, 0}, {2, 0}, {0x800000, 0x1000}, 0x1000,
            {0x800000, 0x1000})
        != MYOS_STATUS_BAD_ARGS) {
        return false;
    }
    if (bundle.open(
            {1, 0}, {2, 0}, {0x800000, 0x1000}, 0x1000,
            {0x810000, 0x1001})
        != MYOS_STATUS_BAD_ARGS) {
        return false;
    }
    Scratch scratch{};
    if (scratch.open(
            {1, 0}, {0x800000, 0x1000}, {0x800000, 0x1000})
            != MYOS_STATUS_BAD_ARGS
        || scratch.open(
            {1, 0}, {0x800000, 0x1000}, {0x810000, 0x1001})
            != MYOS_STATUS_BAD_ARGS) {
        return false;
    }
    return true;
}

struct Test final {
    const char* name;
    bool (*run)() noexcept;
};

constexpr Test tests[] = {
    {"TaskSpace lifecycle", test_taskspace_lifecycle},
    {"creation failures strong close", test_creation_failures_strong_close},
    {"resource close retry", test_resource_close_retry},
    {"typed slot tombstone/move", test_slot_identity_tombstone_and_move},
    {"TaskSpace destructor fault", test_destructor_rejects_open_space},
    {"MappedBundle cleanup retry", test_bundle_cleanup_retry},
    {"lease create/map failures", test_lease_create_and_map_failures},
    {"pending cleanup committed", test_pending_cleanup_is_committed},
    {"pending phase retries", test_pending_phase_retries},
    {"lease move/view invalidation", test_move_preserves_cleanup_phase_and_view},
    {"lease move assignment", test_move_assignment_preserves_cleanup},
    {"ScratchWindow unmap retry/reuse", test_scratch_unmap_blocks_then_reuses},
    {"lease destructor fault", test_lease_destructor_fault},
    {"window checks", test_window_checks},
};

} // namespace

int main() {
    size_t failures = 0;
    for (const Test& test : tests) {
        if (test.run()) {
            continue;
        }
        ++failures;
        (void)fprintf(stderr, "[FAIL] %s\n", test.name);
    }
    (void)fprintf(
        stdout,
        "deployment tests: %zu passed, %zu failed\n",
        sizeof(tests) / sizeof(tests[0]) - failures,
        failures);
    return failures == 0 ? 0 : 1;
}
