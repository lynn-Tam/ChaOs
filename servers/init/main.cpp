#include <servers/proof/protocol.hpp>
#include <user/lib/boot_bundle.hpp>
#include <user/lib/syscall.hpp>
#include <uapi/bootstrap.h>

namespace {

using namespace myos::proof;

constexpr myos_word_t BundleAddress = 0x1000'0000;
constexpr myos_word_t ScratchAddress = 0x1800'0000;
constexpr myos_word_t StackAddress = 0x2100'0000;
constexpr myos_word_t StackStride = 0x0001'0000;
constexpr myos_word_t StackSize = 4 * PageSize;
constexpr myos_word_t ChildMemory = 16 * 1024 * 1024;
constexpr myos_word_t ChildCaps = 512;
constexpr myos_word_t MaxThreads = 2;
constexpr myos_word_t MaxStacks = MaxThreads + 3 * VprocCount;
constexpr myos_word_t VprocDescriptorOffset = 512;
constexpr myos_word_t VprocDescriptorStride = 256;
constexpr myos_word_t SuccessFault = 0xe100;
constexpr myos_word_t FailureFault = 0xe000;

static_assert(MYOS_BOOT_SEGMENT_READ == MYOS_VM_READ);
static_assert(MYOS_BOOT_SEGMENT_WRITE == MYOS_VM_WRITE);
static_assert(MYOS_BOOT_SEGMENT_EXECUTE == MYOS_VM_EXECUTE);

[[nodiscard]] constexpr auto page_round(myos_word_t size) noexcept
    -> myos_word_t {
    return size <= static_cast<myos_word_t>(-1) - (PageSize - 1)
        ? (size + PageSize - 1) & ~(PageSize - 1)
        : 0;
}

[[nodiscard]] auto valid_bootstrap(
    const myos_bootstrap_info* bootstrap,
    myos_word_t size) noexcept -> bool {
    return bootstrap != nullptr
        && size >= sizeof(myos_bootstrap_info)
        && bootstrap->magic == MYOS_BOOTSTRAP_MAGIC
        && bootstrap->major == MYOS_BOOTSTRAP_MAJOR
        && bootstrap->minor >= MYOS_BOOTSTRAP_MINOR
        && bootstrap->size == sizeof(myos_bootstrap_info)
        && bootstrap->cap_count <= MYOS_BOOTSTRAP_MAX_CAPS
        && bootstrap->cpu_count != 0
        && bootstrap->boot_bundle_size != 0;
}

[[nodiscard]] auto capability(
    const myos_bootstrap_info& bootstrap,
    uint32_t kind) noexcept -> myos_cap_t {
    for (uint32_t index = 0; index < bootstrap.cap_count; ++index) {
        if (bootstrap.caps[index].kind == kind) {
            return bootstrap.caps[index].handle;
        }
    }
    return 0;
}

[[nodiscard]] constexpr auto committed(myos::SysResult result) noexcept
    -> bool {
    return result.status == MYOS_STATUS_OK
        || result.status == MYOS_STATUS_PENDING;
}

[[nodiscard]] constexpr auto retryable(myos_status_t status) noexcept
    -> bool {
    return status == MYOS_STATUS_BUSY || status == MYOS_STATUS_RETRY;
}

[[noreturn]] void fault(myos_word_t address) noexcept {
    (void)*reinterpret_cast<volatile const myos_word_t*>(address);
    myos::exit();
}

class Handles final {
public:
    [[nodiscard]] auto add(myos_cap_t handle) noexcept -> bool {
        if (handle == 0 || size_ == Capacity) {
            return false;
        }
        values_[size_++] = handle;
        return true;
    }

    void close_all() noexcept {
        while (size_ != 0) {
            (void)myos::cap_close(values_[--size_]);
        }
    }

private:
    static constexpr myos_word_t Capacity = 64;
    myos_cap_t values_[Capacity]{};
    myos_word_t size_{};
};

class Loader final {
public:
    Loader(
        const myos_bootstrap_info& bootstrap,
        myos_cap_t parent_pool) noexcept
        : root_vspace_(capability(
              bootstrap, MYOS_BOOTSTRAP_CAP_VSPACE)),
          domain_(capability(
              bootstrap, MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN)),
          bundle_(capability(
              bootstrap, MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE)),
          parent_pool_(parent_pool),
          bundle_size_(bootstrap.boot_bundle_size),
          thread_count_(bootstrap.cpu_count < MaxThreads
                  ? bootstrap.cpu_count
                  : MaxThreads) {}

    [[nodiscard]] auto run() noexcept -> bool {
        stage_ = 1;
        if (root_vspace_ == 0 || domain_ == 0 || bundle_ == 0
            || thread_count_ == 0 || !map_bundle()) {
            return false;
        }

        stage_ = 2;
        const auto package = myos::boot::Bundle::parse(
            reinterpret_cast<const void*>(BundleAddress), bundle_size_);
        myos::boot::Module proof{};
        if (!package || !package.find("proof", proof)
            || proof.segment_count() == 0
            || proof.segment_count() > MaxSegments) {
            return false;
        }
        entry_ = proof.entry();

        myos_word_t scratch_size{};
        for (myos_word_t index = 0; index < proof.segment_count(); ++index) {
            myos::boot::Segment segment{};
            if (!proof.segment(index, segment)) {
                return false;
            }
            const myos_word_t rounded = page_round(segment.memory_size);
            if (rounded == 0) {
                return false;
            }
            if (rounded > scratch_size) {
                scratch_size = rounded;
            }
        }
        stage_ = 3;
        if (!make_region(
                root_vspace_, ScratchAddress, scratch_size,
                MYOS_VM_READ | MYOS_VM_WRITE,
                MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP,
                scratch_region_)) {
            return false;
        }

        stage_ = 4;
        const auto child = myos::resource_create_child(
            parent_pool_, ChildMemory, ChildCaps, MYOS_RESOURCE_E3_KINDS);
        if (child.status != MYOS_STATUS_OK || !children_.add(child.value)) {
            return false;
        }
        pool_ = child.value;

        stage_ = 5;
        const auto vspace = myos::vspace_create(pool_);
        const auto cspace = myos::cspace_create(pool_, 32, 8);
        if (vspace.status != MYOS_STATUS_OK
            || cspace.status != MYOS_STATUS_OK
            || !children_.add(vspace.value)
            || !children_.add(cspace.value)) {
            return false;
        }
        child_vspace_ = vspace.value;
        child_cspace_ = cspace.value;

        stage_ = 6;
        for (myos_word_t index = 0; index < proof.segment_count(); ++index) {
            myos::boot::Segment segment{};
            if (!proof.segment(index, segment) || !load_segment(segment)) {
                return false;
            }
        }
        stage_ = 7;
        if (!make_shared_page()) {
            return false;
        }
        stage_ = 8;
        if (!make_notification()) {
            return false;
        }
        stage_ = 9;
        if (!make_stacks()) {
            return false;
        }
        stage_ = 10;
        if (!make_vproc_runtime()) {
            return false;
        }
        stage_ = 11;
        if (!make_executions(proof.entry())) {
            return false;
        }
        stage_ = 12;
        const auto notified = myos::notification_wait(notification_);
        if (notified.status != MYOS_STATUS_OK
            || (notified.value & NotificationBadge) == 0) {
            return false;
        }
        stage_ = 13;
        if (!await_children()) {
            return false;
        }
        stage_ = 14;
        if (!exercise_vproc()) {
            return false;
        }
        stage_ = 15;
        if (!close_child()) {
            return false;
        }
        stage_ = 0;
        return true;
    }

    [[nodiscard]] auto stage() const noexcept -> myos_word_t {
        return stage_;
    }

    void cleanup() noexcept {
        if (pool_ != 0 && !closed_) {
            (void)myos::resource_close(pool_);
        }
        children_.close_all();
        if (scratch_region_ != 0) {
            (void)myos::cap_close(scratch_region_);
            scratch_region_ = 0;
        }
        if (bundle_region_ != 0) {
            (void)myos::cap_close(bundle_region_);
            bundle_region_ = 0;
        }
    }

private:
    static constexpr myos_word_t MaxSegments = 16;

    [[nodiscard]] auto make_region(
        myos_cap_t vspace,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        myos_word_t rights,
        myos_cap_t& result) noexcept -> bool {
        for (;;) {
            const auto region = myos::vm_create_region(
                vspace, address, size, access, MYOS_VM_NORMAL, rights);
            if (region.status == MYOS_STATUS_OK) {
                result = region.value;
                return true;
            }
            if (!retryable(region.status)) {
                return false;
            }
            myos::yield();
        }
    }

    [[nodiscard]] auto map(
        myos_cap_t region,
        myos_cap_t memory,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access) noexcept -> bool {
        for (;;) {
            const auto mapped = myos::vm_map(
                region, memory, address, size, 0, access);
            if (committed(mapped)) {
                return true;
            }
            if (!retryable(mapped.status)) {
                return false;
            }
            myos::yield();
        }
    }

    [[nodiscard]] auto map_bundle() noexcept -> bool {
        const myos_word_t mapped_size = page_round(bundle_size_);
        return mapped_size != 0
            && make_region(
                root_vspace_, BundleAddress, mapped_size, MYOS_VM_READ,
                MYOS_RIGHT_MAP, bundle_region_)
            && map(
                bundle_region_, bundle_, BundleAddress, mapped_size,
                MYOS_VM_READ);
    }

    [[nodiscard]] auto create_memory(
        myos_word_t size,
        myos_word_t access,
        myos_cap_t& result) noexcept -> bool {
        const auto memory = myos::memory_create(pool_, size, access);
        if (memory.status != MYOS_STATUS_OK
            || !children_.add(memory.value)) {
            return false;
        }
        result = memory.value;
        return true;
    }

    [[nodiscard]] auto write_memory(
        myos_cap_t memory,
        myos_word_t size,
        const uint8_t* source,
        myos_word_t source_size) noexcept -> bool {
        if (source_size > size
            || !map(
                scratch_region_, memory, ScratchAddress, size,
                MYOS_VM_READ | MYOS_VM_WRITE)) {
            return false;
        }
        auto* const destination = reinterpret_cast<volatile uint8_t*>(
            ScratchAddress);
        for (myos_word_t index = 0; index < source_size; ++index) {
            destination[index] = source[index];
        }
        for (myos_word_t index = source_size; index < size; ++index) {
            destination[index] = 0;
        }
        return committed(myos::vm_unmap(
            scratch_region_, ScratchAddress, size));
    }

    [[nodiscard]] auto seal(myos_cap_t memory) noexcept -> bool {
        for (;;) {
            const auto sealed = myos::memory_seal(memory);
            if (sealed.status == MYOS_STATUS_OK) {
                return true;
            }
            if (!retryable(sealed.status)) {
                return false;
            }
            myos::yield();
        }
    }

    [[nodiscard]] auto load_segment(
        const myos::boot::Segment& segment) noexcept -> bool {
        const myos_word_t size = page_round(segment.memory_size);
        const bool executable =
            (segment.access & MYOS_VM_EXECUTE) != 0;
        const myos_word_t load_access = segment.access
            | MYOS_VM_READ | MYOS_VM_WRITE;
        myos_cap_t memory{};
        if (size == 0
            || !create_memory(size, load_access, memory)
            || !write_memory(memory, size, segment.file, segment.file_size)
            || (executable && !seal(memory))) {
            return false;
        }

        if (executable && entry_ >= segment.address
            && entry_ - segment.address < size) {
            code_memory_ = memory;
            code_address_ = segment.address;
            code_size_ = size;
        }

        myos_cap_t region{};
        if (!make_region(
                child_vspace_, segment.address, size, segment.access,
                MYOS_RIGHT_MAP, region)
            || !children_.add(region)
            || !map(
                region, memory, segment.address, size, segment.access)) {
            return false;
        }
        return true;
    }

    [[nodiscard]] auto make_shared_page() noexcept -> bool {
        if (!create_memory(
                PageSize, MYOS_VM_READ | MYOS_VM_WRITE, shared_memory_)) {
            return false;
        }
        myos_cap_t root_region{};
        myos_cap_t child_region{};
        if (!make_region(
                root_vspace_, SharedAddress, PageSize,
                MYOS_VM_READ | MYOS_VM_WRITE, MYOS_RIGHT_MAP,
                root_region)
            || !make_region(
                child_vspace_, SharedAddress, PageSize,
                MYOS_VM_READ | MYOS_VM_WRITE, MYOS_RIGHT_MAP,
                child_region)
            || !children_.add(root_region)
            || !children_.add(child_region)
            || !map(
                root_region, shared_memory_, SharedAddress, PageSize,
                MYOS_VM_READ | MYOS_VM_WRITE)
            || !map(
                child_region, shared_memory_, SharedAddress, PageSize,
                MYOS_VM_READ | MYOS_VM_WRITE)) {
            return false;
        }
        flags_ = reinterpret_cast<volatile myos_word_t*>(SharedAddress);
        for (myos_word_t index = 0; index < SharedWords; ++index) {
            flags_[index] = 0;
        }
        return true;
    }

    [[nodiscard]] auto make_stacks() noexcept -> bool {
        const myos_word_t count = thread_count_ + 3 * VprocCount;
        for (myos_word_t index = 0; index < count; ++index) {
            myos_cap_t memory{};
            myos_cap_t region{};
            const myos_word_t address = StackAddress + index * StackStride;
            if (!create_memory(
                    StackSize, MYOS_VM_READ | MYOS_VM_WRITE, memory)
                || !make_region(
                    child_vspace_, address, StackSize,
                    MYOS_VM_READ | MYOS_VM_WRITE, MYOS_RIGHT_MAP, region)
                || !children_.add(region)
                || !map(
                    region, memory, address, StackSize,
                    MYOS_VM_READ | MYOS_VM_WRITE)) {
                return false;
            }
            stack_memory_[index] = memory;
            stack_bases_[index] = address;
            stack_tops_[index] = address + StackSize;
        }
        return true;
    }

    [[nodiscard]] auto make_notification() noexcept -> bool {
        const auto created = myos::notification_create(
            pool_, NotificationBadge);
        if (created.status != MYOS_STATUS_OK
            || !children_.add(created.value)) {
            return false;
        }
        notification_ = created.value;
        if (myos::notification_signal(notification_).status != MYOS_STATUS_OK
            || myos::notification_signal(notification_).status
                != MYOS_STATUS_OK) {
            return false;
        }
        const auto coalesced = myos::notification_take(notification_);
        if (coalesced.status != MYOS_STATUS_OK
            || coalesced.value != NotificationBadge
            || myos::notification_take(notification_).status
                != MYOS_STATUS_RETRY) {
            return false;
        }
        const auto delegated = myos::cap_delegate(
            notification_, child_cspace_, MYOS_RIGHT_SIGNAL);
        if (delegated.status != MYOS_STATUS_OK) {
            return false;
        }
        flags_[NotificationSlot] = delegated.value;

        const auto vproc = myos::notification_create(pool_, VprocBadge);
        if (vproc.status != MYOS_STATUS_OK
            || !children_.add(vproc.value)) {
            return false;
        }
        vproc_notification_ = vproc.value;
        const auto waiter = myos::cap_delegate(
            vproc_notification_, child_cspace_, MYOS_RIGHT_WAIT);
        if (waiter.status != MYOS_STATUS_OK) {
            return false;
        }
        flags_[VprocNotificationSlot] = waiter.value;
        flags_[VprocKeySlot] = 0;
        flags_[VprocStateSlot] = 0;
        return true;
    }

    [[nodiscard]] auto make_vproc_runtime() noexcept -> bool {
        for (myos_word_t index = 0; index < VprocCount; ++index) {
            myos_cap_t control_region{};
            myos_cap_t event_region{};
            const myos_word_t control_address =
                ControlAddress + index * VprocRuntimeStride;
            const myos_word_t event_address =
                EventAddress + index * VprocRuntimeStride;
            if (!create_memory(
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    control_memory_[index])
                || !create_memory(
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    event_memory_[index])
                || !make_region(
                    child_vspace_,
                    control_address,
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    MYOS_RIGHT_MAP,
                    control_region)
                || !children_.add(control_region)
                || !make_region(
                    child_vspace_,
                    event_address,
                    PageSize,
                    MYOS_VM_READ,
                    MYOS_RIGHT_MAP,
                    event_region)
                || !children_.add(event_region)
                || !map(
                    control_region,
                    control_memory_[index],
                    control_address,
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE)
                || !map(
                    event_region,
                    event_memory_[index],
                    event_address,
                    PageSize,
                    MYOS_VM_READ)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto make_executions(myos_word_t entry) noexcept -> bool {
        stage_ = 110;
        myos_cap_t targets[MaxThreads + VprocCount]{};
        myos_cap_t descriptors{};
        if (!create_memory(
                PageSize, MYOS_VM_READ | MYOS_VM_WRITE, descriptors)
            || !map(
                scratch_region_, descriptors, ScratchAddress, PageSize,
                MYOS_VM_READ | MYOS_VM_WRITE)) {
            return false;
        }

        const auto child_pool = myos::cap_delegate(
            pool_, child_cspace_, MYOS_RIGHT_CREATE);
        const auto child_cspace = myos::cap_delegate(
            child_cspace_, child_cspace_, MYOS_RIGHT_MANAGE);
        const auto arm_memory = myos::cap_delegate(
            shared_memory_, child_cspace_, MYOS_RIGHT_INSPECT);
        const auto code = myos::cap_delegate(
            code_memory_, child_cspace_, MYOS_RIGHT_MAP);
        if (code_memory_ == 0 || code_size_ == 0
            || entry < code_address_ || entry - code_address_ >= code_size_
            || child_pool.status != MYOS_STATUS_OK
            || child_cspace.status != MYOS_STATUS_OK
            || arm_memory.status != MYOS_STATUS_OK
            || code.status != MYOS_STATUS_OK) {
            return false;
        }
        flags_[PoolSlot] = child_pool.value;
        flags_[CSpaceSlot] = child_cspace.value;

        myos_cap_t upcall_stacks[VprocCount]{};
        for (myos_word_t index = 0; index < VprocCount; ++index) {
            const myos_word_t stack_index =
                thread_count_ + 3 * index + 2;
            const auto delegated = myos::cap_delegate(
                stack_memory_[stack_index],
                child_cspace_,
                MYOS_RIGHT_MAP);
            if (delegated.status != MYOS_STATUS_OK) {
                return false;
            }
            upcall_stacks[index] = delegated.value;
        }

        auto* const starts = reinterpret_cast<volatile myos_thread_start*>(
            ScratchAddress);
        for (myos_word_t index = 0; index < thread_count_; ++index) {
            starts[index].version = MYOS_THREAD_START_VERSION;
            starts[index].flags = 0;
            starts[index].entry = entry;
            starts[index].stack = stack_tops_[index];
            starts[index].arguments[0] = SharedAddress;
            starts[index].arguments[1] = index;
            for (myos_word_t argument = 2; argument < 6; ++argument) {
                starts[index].arguments[argument] = 0;
            }
        }
        for (myos_word_t index = 0; index < VprocCount; ++index) {
            auto* const vproc_start =
                reinterpret_cast<volatile myos_vproc_start*>(
                    ScratchAddress + VprocDescriptorOffset
                    + index * VprocDescriptorStride);
            vproc_start->version = MYOS_VPROC_START_VERSION;
            vproc_start->flags = 0;
            vproc_start->entry = entry;
            vproc_start->stack = stack_tops_[thread_count_ + 3 * index];
            vproc_start->arguments[0] = arm_memory.value;
            vproc_start->arguments[1] =
                ArmDescriptorOffset + index * ArmDescriptorStride;
            vproc_start->arguments[2] = SharedAddress;
            vproc_start->arguments[3] = index == TargetVproc
                ? VprocMagic
                : SourceVprocMagic;
            vproc_start->arguments[4] =
                stack_tops_[thread_count_ + 3 * index + 1];
            vproc_start->arguments[5] = 0;
            vproc_start->control_memory = control_memory_[index];
            vproc_start->control_page = 0;
            vproc_start->control_address =
                ControlAddress + index * VprocRuntimeStride;
            vproc_start->event_memory = event_memory_[index];
            vproc_start->event_page = 0;
            vproc_start->event_address =
                EventAddress + index * VprocRuntimeStride;

            auto* const arm = reinterpret_cast<volatile myos_vproc_arm*>(
                SharedAddress + ArmDescriptorOffset
                + index * ArmDescriptorStride);
            const myos_word_t code_page =
                (entry - code_address_) / PageSize;
            const myos_word_t upcall_stack =
                thread_count_ + 3 * index + 2;
            arm->version = MYOS_VPROC_ARM_VERSION;
            arm->flags = 0;
            arm->entry = entry;
            arm->code_memory = code.value;
            arm->code_page = code_page;
            arm->code_address = code_address_ + code_page * PageSize;
            arm->code_pages = 1;
            arm->stack_memory = upcall_stacks[index];
            arm->stack_page = StackSize / PageSize - 1;
            arm->stack_address =
                stack_bases_[upcall_stack] + StackSize - PageSize;
            arm->stack_pages = 1;
            arm->stack_top = stack_tops_[upcall_stack];
        }
        if (!committed(myos::vm_unmap(
                scratch_region_, ScratchAddress, PageSize))) {
            return false;
        }

        for (myos_word_t index = 0; index < thread_count_; ++index) {
            const myos_word_t step = 120 + index * 5;
            stage_ = step;
            const auto thread = myos::thread_create(
                pool_, child_vspace_, child_cspace_, descriptors,
                index * sizeof(myos_thread_start));
            if (thread.status != MYOS_STATUS_OK) {
                return false;
            }
            stage_ = step + 1;
            const auto context = myos::sc_create(
                pool_, domain_, 1'000'000, 10'000'000, 30, index);
            if (context.status != MYOS_STATUS_OK) {
                return false;
            }
            stage_ = step + 2;
            if (!children_.add(thread.value)
                || !children_.add(context.value)) {
                return false;
            }
            stage_ = step + 3;
            if (myos::sc_bind(context.value, thread.value).status
                != MYOS_STATUS_OK) {
                return false;
            }
            targets[index] = thread.value;
        }

        for (myos_word_t index = 0; index < VprocCount; ++index) {
            stage_ = 140 + index * 5;
            myos::SysResult vproc{};
            for (;;) {
                vproc = myos::vproc_create(
                    pool_,
                    child_vspace_,
                    child_cspace_,
                    descriptors,
                    VprocDescriptorOffset + index * VprocDescriptorStride);
                if (vproc.status == MYOS_STATUS_OK) {
                    break;
                }
                if (!retryable(vproc.status)) {
                    stage_ = 180
                        + static_cast<myos_word_t>(-vproc.status);
                    return false;
                }
                myos::yield();
            }
            stage_ = 141 + index * 5;
            const myos_word_t home = index == SourceVproc
                    && thread_count_ > 1
                ? 1
                : 0;
            const auto context = myos::sc_create(
                pool_, domain_, 1'000'000, 10'000'000, 30, home);
            if (context.status != MYOS_STATUS_OK
                || !children_.add(vproc.value)
                || !children_.add(context.value)
                || myos::sc_bind(context.value, vproc.value).status
                    != MYOS_STATUS_OK) {
                return false;
            }
            targets[thread_count_ + index] = vproc.value;
        }

        for (myos_word_t index = 0;
             index < thread_count_ + VprocCount;
             ++index) {
            stage_ = 160 + index;
            if (myos::execution_start(targets[index]).status
                != MYOS_STATUS_OK) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto await_children() noexcept -> bool {
        for (;;) {
            bool all_ready{true};
            for (myos_word_t index = 0; index < thread_count_; ++index) {
                if (flags_[index] != ChildReady + index) {
                    all_ready = false;
                }
            }
            if (all_ready) {
                return true;
            }
            myos::yield();
        }
    }

    [[nodiscard]] auto exercise_vproc() noexcept -> bool {
        while (flags_[VprocStateSlot] != VprocReady) {
            myos::yield();
        }
        if (flags_[VprocKeySlot] == 0
            || myos::notification_signal(vproc_notification_).status
                != MYOS_STATUS_OK) {
            return false;
        }
        const myos_word_t expected = VprocComplete | VprocBadge;
        while (flags_[VprocStateSlot] != expected) {
            myos::yield();
        }
        while (flags_[TunnelSourceStateSlot] != TunnelInvoked
            || flags_[TunnelTargetStateSlot] != TunnelDelivered) {
            myos::yield();
        }
        return flags_[TunnelSourceSequenceSlot] != 0
            && flags_[TunnelSourceSequenceSlot]
                == flags_[TunnelTargetSequenceSlot]
            && flags_[TunnelHeartbeatSlot] != 0;
    }

    [[nodiscard]] auto close_child() noexcept -> bool {
        const auto closed = myos::resource_close(pool_);
        if (closed.status != MYOS_STATUS_OK) {
            return false;
        }
        closed_ = true;
        children_.close_all();
        pool_ = 0;
        return true;
    }

    myos_cap_t root_vspace_{};
    myos_cap_t domain_{};
    myos_cap_t bundle_{};
    myos_cap_t parent_pool_{};
    myos_word_t bundle_size_{};
    myos_word_t thread_count_{};
    myos_cap_t pool_{};
    myos_cap_t child_vspace_{};
    myos_cap_t child_cspace_{};
    myos_cap_t bundle_region_{};
    myos_cap_t scratch_region_{};
    myos_cap_t shared_memory_{};
    myos_cap_t notification_{};
    myos_cap_t vproc_notification_{};
    myos_cap_t control_memory_[VprocCount]{};
    myos_cap_t event_memory_[VprocCount]{};
    myos_cap_t code_memory_{};
    myos_word_t code_address_{};
    myos_word_t code_size_{};
    myos_word_t entry_{};
    volatile myos_word_t* flags_{};
    myos_cap_t stack_memory_[MaxStacks]{};
    myos_word_t stack_bases_[MaxStacks]{};
    myos_word_t stack_tops_[MaxStacks]{};
    Handles children_{};
    bool closed_{};
    myos_word_t stage_{};
};

} // namespace

//Confirmatory experiment.
// Exit condition: replace the proof child and shared flag rendezvous with the
// first persistent process service once Endpoint IPC exists.
extern "C" void myos_main(
    myos_word_t bootstrap_address,
    myos_word_t bootstrap_size) noexcept {
    const auto* const bootstrap =
        reinterpret_cast<const myos_bootstrap_info*>(bootstrap_address);
    if (!valid_bootstrap(bootstrap, bootstrap_size)) {
        fault(FailureFault);
    }
    const myos_cap_t parent_pool = capability(
        *bootstrap, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    if (parent_pool == 0) {
        fault(FailureFault);
    }

    Loader loader{*bootstrap, parent_pool};
    const bool complete = loader.run();
    loader.cleanup();
    fault(complete ? SuccessFault : FailureFault + loader.stage());
}
