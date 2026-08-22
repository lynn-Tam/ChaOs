#include <servers/proof/protocol.hpp>
#include <user/lib/boot_bundle.hpp>
#include <user/lib/syscall.hpp>
#include <uapi/bootstrap.h>

namespace {

using namespace myos::proof;

constexpr myos_word_t BundleAddress = 0x1000'0000;
constexpr myos_word_t ScratchAddress = 0x1800'0000;
constexpr myos_word_t StackSize = 4 * PageSize;
// E7 adds terminal state and pager/IRQ grant metadata to each execution and
// object slot. Keep the proof workload's budget explicit instead of letting a
// late scheduling-context construction fail after the address-space work.
constexpr myos_word_t ChildMemory = 32 * 1024 * 1024;
/*luna change: keep pressure child sponsorship below the 25MiB root ceiling,
  reason: 11MiB admits the proof child and does not manufacture PMM pressure;
  the fault-boundary OwnedPageGroup drain is the sole pressure mechanism*/
constexpr myos_word_t PressureMemory = 11 * 1024 * 1024;
constexpr myos_word_t ChildCaps = 512;
constexpr myos_word_t MaxThreads = 2;
constexpr myos_word_t ChannelThreadCount = 2;
/*luna change: reserve stacks and execution slots for the Pager workers, reason: the proof adds real blocking service Threads and the resilience role pre-creates a replacement without reducing existing Vproc/channel capacity*/
constexpr myos_word_t PagerWorkerCount = 1;
constexpr myos_word_t MaxPagerWorkers = 2;
constexpr myos_word_t EndpointActivations = 1;
constexpr myos_word_t MaxStacks =
    MaxThreads + 3 * VprocCount + EndpointActivations
    + ChannelThreadCount + MaxPagerWorkers;
constexpr myos_word_t ThreadDescriptorCount =
    MaxThreads + ChannelThreadCount + MaxPagerWorkers;
constexpr myos_word_t VprocDescriptorStride = 256;
constexpr myos_word_t VprocDescriptorOffset =
    ((ThreadDescriptorCount * sizeof(myos_thread_start)
         + VprocDescriptorStride - 1)
        / VprocDescriptorStride)
    * VprocDescriptorStride;
constexpr myos_word_t EndpointDescriptorOffset = 2048;
constexpr myos_word_t SuccessFault = 0xe100;
constexpr myos_word_t FailureFault = 0xe000;

static_assert(MYOS_BOOT_SEGMENT_READ == MYOS_VM_READ);
static_assert(MYOS_BOOT_SEGMENT_WRITE == MYOS_VM_WRITE);
static_assert(MYOS_BOOT_SEGMENT_EXECUTE == MYOS_VM_EXECUTE);
/*luna change: align Vproc descriptors after the complete Thread descriptor block, reason: five thread starts occupy 520 bytes and must not overlap the former 512-byte Vproc offset*/
static_assert(
    ThreadDescriptorCount * sizeof(myos_thread_start) <= VprocDescriptorOffset);
static_assert(
    VprocDescriptorOffset + VprocCount * VprocDescriptorStride
    <= EndpointDescriptorOffset);

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
    static constexpr myos_word_t Capacity = 128;
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
        if (!package) {
            return false;
        }
        /*luna change: freeze the manifest-selected run mode, reason: only the
          bundle root role may authorize mode-only construction*/
        pressure_ = package.root_is("pressure");
        resilience_ = package.root_is("resilience");
        // Pressure and resilience are focused proofs. The sole ordinary
        // Thread owns the tested fault sequence; the second CPU Thread
        // belongs to E1.
        if (pressure_ || resilience_) {
            thread_count_ = 1;
        }
        if (!package.find("proof", proof)) {
            return false;
        }
        if (proof.segment_count() == 0
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
            parent_pool_, pressure_ ? PressureMemory : ChildMemory, ChildCaps,
            MYOS_RESOURCE_E6_KINDS | MYOS_RESOURCE_PAGER);
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
        /*luna change: publish the frozen mode before any child descriptor exists, reason: proof actors must gate workload phases from configuration rather than runtime observations*/
        shared_.store(RunModeSlot, run_mode());
        stage_ = 8;
        /*luna change: place Pager construction before child execution setup, reason: the target mapping and notification must exist before descriptors and workers are started*/
        if (!make_pager()) {
            return false;
        }
        stage_ = 9;
        if (!make_notification()) {
            return false;
        }
        stage_ = 10;
        if (!pressure_ && !resilience_ && !make_channel()) {
            return false;
        }
        stage_ = 11;
        if (!make_stacks()) {
            return false;
        }
        stage_ = 12;
        if (!make_vproc_runtime()) {
            return false;
        }
        stage_ = 13;
        if (!make_executions(proof.entry())) {
            return false;
        }
        shared_.progress(
            ProgressActor::Coordinator,
            ProgressStage::Boot,
            ProgressWait::Notification);
        stage_ = 13;
        /*luna change: bound the coordinator notification observation, reason: a missing child signal must remain visible to the existing progress watchdog*/
        /*luna change: also wait for the doomed worker terminal badge in the
          resilience role, reason: TerminalObservation delivery is evidence
          and precedes any barrier judgement*/
        const myos_word_t required_badges = resilience_
            ? (NotificationBadge | WorkerDeathBadge)
            : NotificationBadge;
        myos_word_t observed_badges{};
        for (;;) {
            const auto notified = myos::notification_take(notification_);
            if (notified.status == MYOS_STATUS_OK) {
                observed_badges |= notified.value;
                if ((observed_badges & required_badges) == required_badges) {
                    break;
                }
            } else if (notified.status != MYOS_STATUS_RETRY
                || !observe()) {
                return false;
            }
            myos::yield();
        }
        if (resilience_
            && myos::terminal_query(worker_a_).status != MYOS_STATUS_OK) {
            return false;
        }
        shared_.progress(
            ProgressActor::Coordinator,
            ProgressStage::ChannelBound,
            ProgressWait::Children);
        stage_ = 14;
        if (!await_children()) {
            return false;
        }
        /*luna change: project the Pager wait through the existing coordinator trace, reason: init must explain the worker lane without using diagnostics as a barrier*/
        shared_.progress(
            ProgressActor::Coordinator,
            ProgressStage::Pager,
            ProgressWait::Pager);
        stage_ = 15;
        if (!exercise_pager()) {
            return false;
        }
        /*luna change: terminate pressure mode at the completed Pager proof,
          reason: ordinary Vproc, tunnel and channel exercise must not create
          post-drain demand and both modes can share the existing close tail*/
        if (!pressure_ && !resilience_) {
            shared_.progress(
                ProgressActor::Coordinator,
                ProgressStage::FirstReceive,
                ProgressWait::VprocReady);
            stage_ = 16;
            if (!exercise_vproc()) {
                return false;
            }
            shared_.progress(
                ProgressActor::Coordinator,
                ProgressStage::VprocDone,
                ProgressWait::ChannelReady);
            stage_ = 17;
            if (!await_channel()) {
                return false;
            }
        }
        stage_ = 18;
        shared_.progress(
            ProgressActor::Coordinator,
            ProgressStage::Complete);
        if (!close_child()) {
            return false;
        }
        stage_ = 0;
        return true;
    }

    [[nodiscard]] auto stage() const noexcept -> myos_word_t {
        return stage_;
    }

    [[nodiscard]] auto failure_code() const noexcept -> myos_word_t {
        return diagnostic_code_ != 0 ? diagnostic_code_ : stage_;
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
    static constexpr myos_word_t max_word =
        static_cast<myos_word_t>(-1);

    [[nodiscard]] auto vproc_count() const noexcept -> myos_word_t {
        return (pressure_ || resilience_) ? 1 : VprocCount;
    }

    [[nodiscard]] auto channel_count() const noexcept -> myos_word_t {
        return (pressure_ || resilience_) ? 0 : ChannelThreadCount;
    }

    [[nodiscard]] auto endpoint_count() const noexcept -> myos_word_t {
        return (pressure_ || resilience_) ? 0 : EndpointActivations;
    }

    /*luna change: pre-create two workers only for the resilience role,
      reason: worker replacement is a deployment fact fixed by the bundle
      root, not a runtime supervision decision*/
    [[nodiscard]] auto worker_count() const noexcept -> myos_word_t {
        return resilience_ ? MaxPagerWorkers : PagerWorkerCount;
    }

    [[nodiscard]] auto run_mode() const noexcept -> myos_word_t {
        return pressure_
            ? ModePressure
            : (resilience_ ? ModeResilience : ModeOrdinary);
    }

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
        auto* const destination = reinterpret_cast<uint8_t*>(
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
        const auto clock0 = myos::clock_now();
        const auto clock1 = myos::clock_now();
        const auto frequency = myos::clock_frequency();
        if (clock0.status != MYOS_STATUS_OK
            || clock1.status != MYOS_STATUS_OK
            || frequency.status != MYOS_STATUS_OK
            || clock1.value < clock0.value || frequency.value == 0) {
            return false;
        }
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
        shared_.bind(SharedAddress);
        for (myos_word_t index = 0; index < SharedWords; ++index) {
            shared_.store(index, 0);
        }
        shared_.progress(
            ProgressActor::Coordinator,
            ProgressStage::Boot,
            ProgressWait::Notification);
        observe_start();
        return true;
    }

    [[nodiscard]] auto make_stacks() noexcept -> bool {
        /*luna change: include every Pager worker in stack allocation, reason: each registered IPC page belongs to its own blocking Thread stack*/
        const myos_word_t count =
            thread_count_ + 3 * vproc_count() + endpoint_count()
            + channel_count() + worker_count();
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

    /*luna change: construct one Pager-backed target and a resident staging page, reason: the proof must drive the existing request/claim/supply path rather than a test-only mapping*/
    [[nodiscard]] auto make_pager() noexcept -> bool {
        const auto pager = myos::pager_create(
            pool_, PagerBackingKey, 1);
        if (pager.status != MYOS_STATUS_OK
            || !children_.add(pager.value)) {
            return false;
        }
        pager_ = pager.value;
        if (!create_memory(
                PageSize, MYOS_VM_READ | MYOS_VM_WRITE, staging_memory_)) {
            return false;
        }
        const myos_word_t value = PagerValue;
        if (!write_memory(
                staging_memory_, PageSize,
                reinterpret_cast<const uint8_t*>(&value), sizeof(value))) {
            return false;
        }
        /*luna change: reserve the staging rematerialization route only in pressure mode,
          reason: ordinary E1 transfers the seed once and must not allocate an
          unused VSpace mutation capability*/
        if (pressure_
            && (!make_region(
                    child_vspace_, StagingAddress, PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    /*luna change: retain parent delegation on the staging region, reason: pressure mode must attenuate this source into the child CSpace without widening the child view*/
                    MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DELEGATE,
                    staging_region_)
                || !children_.add(staging_region_))) {
            return false;
        }
        /*luna change: size the pager target from the frozen run mode, reason:
          resilience needs one supplied page and one failed page while the
          other roles keep their original extent*/
        const myos_word_t target_size =
            resilience_ ? 2 * PageSize : PageSize;
        const auto target = myos::memory_create_pager(
            pool_, target_size, MYOS_VM_READ | MYOS_VM_WRITE, pager_);
        if (target.status != MYOS_STATUS_OK
            || !children_.add(target.value)) {
            return false;
        }
        target_memory_ = target.value;

        const myos_word_t pager_size = pressure_ ? 2 * PageSize : target_size;
        if (!make_region(
                child_vspace_, PagerAddress, pager_size,
                MYOS_VM_READ | MYOS_VM_WRITE,
                MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_PROTECT
                    | MYOS_RIGHT_DELEGATE,
                pager_region_)
            || !children_.add(pager_region_)
            || !map(
                pager_region_, target_memory_, PagerAddress, target_size,
                MYOS_VM_READ | MYOS_VM_WRITE)) {
            return false;
        }
        /*luna change: retain PagerAddress's L0 table with the existing shared
          page only in pressure mode, reason: exact Pager invalidation must
          return only the resident candidate frame to a Pressure relation*/
        if (pressure_ && !map(
                pager_region_, shared_memory_, PagerAddress + PageSize,
                PageSize, MYOS_VM_READ | MYOS_VM_WRITE)) {
            return false;
        }

        /*luna change: keep one four-page lazy pressure object, reason: two
          tested faults isolate clean and dirty reclaim, page two prewarms
          metadata, and page three acknowledges fixture release*/
        if (pressure_) {
            if (!create_memory(
                    StressSize, MYOS_VM_READ | MYOS_VM_WRITE, stress_memory_)
                || !make_region(
                    child_vspace_, StressAddress,
                    StressSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DELEGATE,
                    stress_region_)
                || !children_.add(stress_region_)) {
                return false;
            }
        }

        const auto notification = myos::notification_create(
            pool_, PagerBadge);
        if (notification.status != MYOS_STATUS_OK
            || !children_.add(notification.value)) {
            return false;
        }
        pager_notification_ = notification.value;
        return myos::pager_bind(
            pager_, pager_notification_, PagerBadge).status == MYOS_STATUS_OK;
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
            notification_, child_cspace_,
            MYOS_RIGHT_SIGNAL | MYOS_RIGHT_DUPLICATE
                | MYOS_RIGHT_DELEGATE);
        if (delegated.status != MYOS_STATUS_OK) {
            return false;
        }
        shared_.store(NotificationSlot, delegated.value);

        // The focused pressure and resilience graphs have no ordinary
        // notification exercise.
        if (pressure_ || resilience_) {
            return true;
        }

        const auto vproc = myos::notification_create(pool_, VprocBadge);
        if (vproc.status != MYOS_STATUS_OK
            || !children_.add(vproc.value)) {
            return false;
        }
        vproc_notification_ = vproc.value;
        const auto waiter = myos::cap_delegate(
            vproc_notification_, child_cspace_, MYOS_RIGHT_RECEIVE);
        if (waiter.status != MYOS_STATUS_OK) {
            return false;
        }
        shared_.store(VprocNotificationSlot, waiter.value);
        shared_.store(VprocKeySlot, 0);
        shared_.store(VprocStateSlot, 0);
        return true;
    }

    [[nodiscard]] auto make_channel() noexcept -> bool {
        const auto created = myos::channel_create(
            pool_, 1, MYOS_CHANNEL_MAX_WORDS, 1, MYOS_CHANNEL_MAX_RELATIONS);
        if (created.status != MYOS_STATUS_OK
            || created.value == 0 || created.value2 == 0
            || !children_.add(created.value)
            || !children_.add(created.value2)) {
            return false;
        }
        const auto sender = myos::channel_mint(
            created.value,
            child_cspace_,
            10,
            MYOS_RIGHT_SEND | MYOS_RIGHT_CLOSE);
        const auto sender_alt = myos::channel_mint(
            created.value,
            child_cspace_,
            20,
            MYOS_RIGHT_SEND | MYOS_RIGHT_CLOSE);
        const auto receiver = myos::channel_mint(
            created.value2,
            child_cspace_,
            30,
            MYOS_RIGHT_RECEIVE | MYOS_RIGHT_CLOSE);
        if (sender.status != MYOS_STATUS_OK
            || sender_alt.status != MYOS_STATUS_OK
            || receiver.status != MYOS_STATUS_OK) {
            return false;
        }
        shared_.store(ChannelSenderSlot, sender.value);
        shared_.store(ChannelSenderAltSlot, sender_alt.value);
        shared_.store(ChannelReceiverSlot, receiver.value);

        const auto notify_r = myos::notification_create(
            pool_, ChannelNotifyRBadge);
        const auto notify_s = myos::notification_create(
            pool_, ChannelNotifySBadge);
        if (notify_r.status != MYOS_STATUS_OK
            || notify_s.status != MYOS_STATUS_OK
            || !children_.add(notify_r.value)
            || !children_.add(notify_s.value)) {
            return false;
        }
        const auto notify_r_child = myos::cap_delegate(
            notify_r.value,
            child_cspace_,
            MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE
                | MYOS_RIGHT_DUPLICATE);
        const auto notify_s_child = myos::cap_delegate(
            notify_s.value,
            child_cspace_,
            MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE
                | MYOS_RIGHT_DUPLICATE);
        if (notify_r_child.status != MYOS_STATUS_OK
            || notify_s_child.status != MYOS_STATUS_OK) {
            return false;
        }
        shared_.store(ChannelNotifyRSlot, notify_r_child.value);
        shared_.store(ChannelNotifySSlot, notify_s_child.value);
        return true;
    }

    [[nodiscard]] auto make_vproc_runtime() noexcept -> bool {
        for (myos_word_t index = 0; index < vproc_count(); ++index) {
            myos_cap_t control_region{};
            myos_cap_t event_region{};
            myos_cap_t ipc_region{};
            const myos_word_t control_address =
                ControlAddress + index * VprocRuntimeStride;
            const myos_word_t event_address =
                EventAddress + index * VprocRuntimeStride;
            const myos_word_t ipc_address =
                ChannelVprocIpcAddress + index * PageSize;
            if (!create_memory(
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    control_memory_[index])
                || !create_memory(
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    event_memory_[index])
                || !create_memory(
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    ipc_memory_[index])
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
                || !make_region(
                    child_vspace_,
                    ipc_address,
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    MYOS_RIGHT_MAP,
                    ipc_region)
                || !children_.add(ipc_region)
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
                    MYOS_VM_READ)
                || !map(
                    ipc_region,
                    ipc_memory_[index],
                    ipc_address,
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto make_executions(myos_word_t entry) noexcept -> bool {
        stage_ = 110;
        const myos_word_t active_vprocs = vproc_count();
        const myos_word_t active_channels = channel_count();
        const myos_word_t active_endpoints = endpoint_count();
        /*luna change: extend execution targets for the Pager worker, reason: descriptor and start order must cover every created Thread and Vproc exactly once*/
        myos_cap_t targets[
            MaxThreads + ChannelThreadCount + MaxPagerWorkers
            + VprocCount]{};
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
        const auto pager = myos::cap_delegate(
            pager_, child_cspace_, MYOS_RIGHT_SERVE | MYOS_RIGHT_SUPPLY);
        const auto target = myos::cap_delegate(
            target_memory_, child_cspace_, MYOS_RIGHT_MANAGE);
        /*luna change: gate the worker's source-map authority on pressure mode,
          reason: only the extended sequence rematerializes staging through
          ordinary VSpace operations*/
        const auto staging = myos::cap_delegate(
            staging_memory_, child_cspace_,
            MYOS_RIGHT_MANAGE | (pressure_ ? MYOS_RIGHT_MAP : 0));
        /*luna change: retain optional pressure delegation results in the existing execution setup, reason: capability validation must stay local without adding shared state or a second handoff path*/
        myos::SysResult staging_region{};
        myos::SysResult stress_memory{};
        myos::SysResult stress_region{};
        myos::SysResult pager_region{};
        if (pressure_) {
            staging_region = myos::cap_delegate(
                staging_region_, child_cspace_,
                MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP);
            /*luna change: attenuate the pressure authorities for Thread0, reason: the parent owns logical-object creation while the child only remaps and accesses its fixed working set*/
            stress_memory = myos::cap_delegate(
                stress_memory_, child_cspace_, MYOS_RIGHT_MAP);
            stress_region = myos::cap_delegate(
                stress_region_, child_cspace_,
                MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP);
        }
        const auto pager_notification = myos::cap_delegate(
            pager_notification_, child_cspace_, MYOS_RIGHT_RECEIVE);
        /*luna change: delegate only Protect on the exact Pager region,
          reason: VSpace mutation authority names one AddressRegion and does
          not recursively traverse nested regions*/
        if (pressure_) {
            pager_region = myos::cap_delegate(
                pager_region_, child_cspace_,
                MYOS_RIGHT_PROTECT);
        }
        if (code_memory_ == 0 || code_size_ == 0
            || entry < code_address_ || entry - code_address_ >= code_size_
            || child_pool.status != MYOS_STATUS_OK
            || child_cspace.status != MYOS_STATUS_OK
            || arm_memory.status != MYOS_STATUS_OK
            || code.status != MYOS_STATUS_OK
            || pager.status != MYOS_STATUS_OK
            || target.status != MYOS_STATUS_OK
            || staging.status != MYOS_STATUS_OK
            || pager_notification.status != MYOS_STATUS_OK
            || (pressure_ && (staging_region.status != MYOS_STATUS_OK
                || stress_memory.status != MYOS_STATUS_OK
                || stress_region.status != MYOS_STATUS_OK
                || pager_region.status != MYOS_STATUS_OK))) {
            return false;
        }
        shared_.store(PoolSlot, child_pool.value);
        shared_.store(CSpaceSlot, child_cspace.value);
        /*luna change: delegate only the pager service capabilities needed by the worker, reason: capability authority stays explicit while the shared page carries stable child handles*/
        shared_.store(PagerCapSlot, pager.value);
        shared_.store(PagerTargetCapSlot, target.value);
        shared_.store(PagerSourceCapSlot, staging.value);
        shared_.store(PagerNotifyCapSlot, pager_notification.value);
        if (pressure_) {
            shared_.store(PagerStagingRegionSlot, staging_region.value);
            shared_.store(PagerRegionSlot, pager_region.value);
        }

        myos_cap_t upcall_stacks[VprocCount]{};
        for (myos_word_t index = 0; index < active_vprocs; ++index) {
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

        auto* const starts = reinterpret_cast<myos_thread_start*>(
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
            /*luna change: pass pressure authorities through Thread0's existing actor-local arguments, reason: the handoff avoids new shared capability slots or ABI fields and is not worker state*/
            if (pressure_ && index == 0) {
                starts[index].arguments[2] = stress_region.value;
                starts[index].arguments[3] = stress_memory.value;
            }
            starts[index].ipc.memory = stack_memory_[index];
            starts[index].ipc.page = 0;
            starts[index].ipc.address = stack_bases_[index];
            starts[index].ipc.pages = 1;
        }
        const myos_word_t channel_stack_base =
            thread_count_ + 3 * active_vprocs + active_endpoints;
        for (myos_word_t index = 0; index < active_channels; ++index) {
            const myos_word_t descriptor_index = thread_count_ + index;
            const myos_word_t stack_index = channel_stack_base + index;
            starts[descriptor_index].version = MYOS_THREAD_START_VERSION;
            starts[descriptor_index].flags = 0;
            starts[descriptor_index].entry = entry;
            starts[descriptor_index].stack = stack_tops_[stack_index];
            starts[descriptor_index].arguments[0] = SharedAddress;
            starts[descriptor_index].arguments[1] = index == 0
                ? ChannelSenderMagic
                : ChannelReceiverMagic;
            starts[descriptor_index].arguments[2] = stack_bases_[stack_index];
            for (myos_word_t argument = 3; argument < 6; ++argument) {
                starts[descriptor_index].arguments[argument] = 0;
            }
            starts[descriptor_index].ipc.memory = stack_memory_[stack_index];
            starts[descriptor_index].ipc.page = 0;
            starts[descriptor_index].ipc.address = stack_bases_[stack_index];
            starts[descriptor_index].ipc.pages = 1;
        }
        /*luna change: add one ordinary proof worker with its own registered IPC page, reason: Pager service calls must be exercised by a real blocking Thread and never by a Vproc runtime stack*/
        const myos_word_t worker_descriptor = thread_count_
            + active_channels;
        const myos_word_t worker_stack = thread_count_
            + 3 * active_vprocs + active_endpoints + active_channels;
        starts[worker_descriptor].version = MYOS_THREAD_START_VERSION;
        starts[worker_descriptor].flags = 0;
        starts[worker_descriptor].entry = entry;
        starts[worker_descriptor].stack = stack_tops_[worker_stack];
        starts[worker_descriptor].arguments[0] = SharedAddress;
        starts[worker_descriptor].arguments[1] = PagerWorkerMagic;
        starts[worker_descriptor].arguments[2] = 0;
        starts[worker_descriptor].arguments[3] = 0;
        starts[worker_descriptor].arguments[4] = 0;
        starts[worker_descriptor].arguments[5] = stack_bases_[worker_stack];
        starts[worker_descriptor].ipc.memory = stack_memory_[worker_stack];
        starts[worker_descriptor].ipc.page = 0;
        starts[worker_descriptor].ipc.address = stack_bases_[worker_stack];
        starts[worker_descriptor].ipc.pages = 1;
        /*luna change: pre-create the replacement worker beside the doomed
          one, reason: replacement is a deployment fact of the resilience
          role and the peer IPC address hands over the stale identity
          without new shared state*/
        for (myos_word_t index = 1; index < worker_count(); ++index) {
            const myos_word_t descriptor = worker_descriptor + index;
            const myos_word_t stack = worker_stack + index;
            starts[descriptor].version = MYOS_THREAD_START_VERSION;
            starts[descriptor].flags = 0;
            starts[descriptor].entry = entry;
            starts[descriptor].stack = stack_tops_[stack];
            starts[descriptor].arguments[0] = SharedAddress;
            starts[descriptor].arguments[1] = PagerWorkerBMagic;
            starts[descriptor].arguments[2] = 0;
            starts[descriptor].arguments[3] = 0;
            starts[descriptor].arguments[4] = stack_bases_[worker_stack];
            starts[descriptor].arguments[5] = stack_bases_[stack];
            starts[descriptor].ipc.memory = stack_memory_[stack];
            starts[descriptor].ipc.page = 0;
            starts[descriptor].ipc.address = stack_bases_[stack];
            starts[descriptor].ipc.pages = 1;
        }
        for (myos_word_t index = 0; index < active_vprocs; ++index) {
            auto* const vproc_start =
                reinterpret_cast<myos_vproc_start*>(
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
            vproc_start->arguments[5] =
                ChannelVprocIpcAddress + index * PageSize;
            vproc_start->control_memory = control_memory_[index];
            vproc_start->control_page = 0;
            vproc_start->control_address =
                ControlAddress + index * VprocRuntimeStride;
            vproc_start->event_memory = event_memory_[index];
            vproc_start->event_page = 0;
            vproc_start->event_address =
                EventAddress + index * VprocRuntimeStride;
            vproc_start->ipc.memory = ipc_memory_[index];
            vproc_start->ipc.page = 0;
            vproc_start->ipc.address =
                ChannelVprocIpcAddress + index * PageSize;
            vproc_start->ipc.pages = 1;

            auto* const arm = reinterpret_cast<myos_vproc_arm*>(
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

        if (!pressure_ && !resilience_) {
            const myos_word_t endpoint_stack =
                thread_count_ + 3 * active_vprocs;
            myos_cap_t endpoint_ipc_region{};
            if (!create_memory(
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    endpoint_ipc_memory_)
                || !make_region(
                    child_vspace_, EndpointIpcAddress, PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE, MYOS_RIGHT_MAP,
                    endpoint_ipc_region)
                || !children_.add(endpoint_ipc_region)
                || !map(
                    endpoint_ipc_region, endpoint_ipc_memory_,
                    EndpointIpcAddress, PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE)) {
                return false;
            }
            auto* const endpoint_desc =
                reinterpret_cast<myos_endpoint_desc*>(
                    ScratchAddress + EndpointDescriptorOffset);
            endpoint_desc->version = MYOS_ENDPOINT_VERSION;
            endpoint_desc->flags = MYOS_ENDPOINT_FLAGS_NONE;
            endpoint_desc->entry = entry;
            endpoint_desc->code_memory = code_memory_;
            endpoint_desc->code_page = 0;
            endpoint_desc->code_address = code_address_;
            endpoint_desc->code_pages = 1;
            endpoint_desc->stack_memory = stack_memory_[endpoint_stack];
            endpoint_desc->stack_page = 0;
            endpoint_desc->stack_address = stack_bases_[endpoint_stack];
            endpoint_desc->stack_pages = StackSize / PageSize;
            endpoint_desc->stack_stride = StackSize;
            endpoint_desc->ipc.memory = endpoint_ipc_memory_;
            endpoint_desc->ipc.page = 0;
            endpoint_desc->ipc.address = EndpointIpcAddress;
            endpoint_desc->ipc.pages = 1;
            endpoint_desc->ipc_stride = PageSize;
            endpoint_desc->activation_count = EndpointActivations;
            endpoint_desc->queue_capacity = 2;
            endpoint_desc->max_depth = 4;
            endpoint_desc->budget_floor_ns = 1'000;
            endpoint_desc->urgency_ceiling = 30;

            const auto endpoint = myos::endpoint_create(
                pool_, child_vspace_, child_cspace_, descriptors,
                EndpointDescriptorOffset);
            if (endpoint.status != MYOS_STATUS_OK
                || !children_.add(endpoint.value)) {
                return false;
            }
            const auto caller = myos::endpoint_mint(
                endpoint.value,
                child_cspace_,
                EndpointBadge,
                1,
                MYOS_RIGHT_CALL);
            if (caller.status != MYOS_STATUS_OK) {
                return false;
            }
            shared_.store(EndpointSlot, caller.value);
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

        for (myos_word_t index = 0; index < active_channels; ++index) {
            const myos_word_t step = 130 + index * 5;
            const myos_word_t descriptor_index = thread_count_ + index;
            stage_ = step;
            const auto thread = myos::thread_create(
                pool_, child_vspace_, child_cspace_, descriptors,
                descriptor_index * sizeof(myos_thread_start));
            if (thread.status != MYOS_STATUS_OK) {
                return false;
            }
            stage_ = step + 1;
            const myos_word_t home = index == 1 && thread_count_ > 1
                ? 1
                : 0;
            const auto context = myos::sc_create(
                pool_, domain_, 1'000'000, 10'000'000, 30, home);
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
            targets[descriptor_index] = thread.value;
        }

        /*luna change: create every worker through its canonical descriptor index, reason: descriptor layout and execution creation share one index and the doomed worker binds terminal observation first*/
        for (myos_word_t index = 0; index < worker_count(); ++index) {
            stage_ = 138 + index;
            const auto worker = myos::thread_create(
                pool_, child_vspace_, child_cspace_, descriptors,
                (worker_descriptor + index) * sizeof(myos_thread_start));
            if (worker.status != MYOS_STATUS_OK) {
                return false;
            }
            const auto worker_context = myos::sc_create(
                pool_, domain_, 1'000'000, 10'000'000, 30, 0);
            if (worker_context.status != MYOS_STATUS_OK
                || !children_.add(worker.value)
                || !children_.add(worker_context.value)
                || myos::sc_bind(worker_context.value, worker.value).status
                    != MYOS_STATUS_OK) {
                return false;
            }
            if (index == 0) {
                worker_a_ = worker.value;
            }
            targets[worker_descriptor + index] = worker.value;
        }
        /*luna change: bind terminal observation on the doomed worker, reason: the coordinator's badge is the evidence that execution terminal delivery works without supervision machinery*/
        if (resilience_
            && myos::terminal_observe_bind(
                   worker_a_, notification_, WorkerDeathBadge).status
                != MYOS_STATUS_OK) {
            return false;
        }

        for (myos_word_t index = 0; index < active_vprocs; ++index) {
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
            targets[
                thread_count_ + active_channels + worker_count() + index]
                = vproc.value;
        }

        for (myos_word_t index = 0;
             index < thread_count_ + active_channels + worker_count()
                 + active_vprocs;
             ++index) {
            stage_ = 160 + index;
            if (myos::execution_start(targets[index]).status
                != MYOS_STATUS_OK) {
                return false;
            }
        }
        return true;
    }

    void observe_start() noexcept {
        const auto frequency = myos::clock_frequency();
        const auto now = myos::clock_now();
        if (frequency.status != MYOS_STATUS_OK
            || now.status != MYOS_STATUS_OK || frequency.value == 0) {
            return;
        }
        observe_frequency_ = frequency.value;
        observe_last_tick_ = now.value;
        observe_last_epoch_ = shared_.load(ProgressEpochSlot);
        observe_hard_ticks_ = observe_frequency_ > max_word / 8
            ? max_word : observe_frequency_ * 8;
        observe_enabled_ = true;
    }

    [[nodiscard]] auto observe() noexcept -> bool {
        if (!observe_enabled_) {
            return true;
        }
        const auto now = myos::clock_now();
        if (now.status != MYOS_STATUS_OK) {
            return true;
        }
        const myos_word_t epoch = shared_.load(ProgressEpochSlot);
        if (epoch != observe_last_epoch_) {
            observe_last_epoch_ = epoch;
            observe_last_tick_ = now.value;
            return true;
        }
        if (now.value < observe_last_tick_
            || now.value - observe_last_tick_ < observe_hard_ticks_) {
            return true;
        }

        myos_word_t actor_code{};
        myos_word_t best_sequence = max_word;
        for (myos_word_t index = 0;
             index < ProgressActorCount; ++index) {
            ProgressSnapshot snapshot{};
            const auto actor = static_cast<ProgressActor>(index);
            if (!shared_.progress_read(actor, snapshot)) {
                continue;
            }
            if (snapshot.wait != 0 && snapshot.sequence < best_sequence) {
                best_sequence = snapshot.sequence;
                actor_code = (index << 16)
                    | ((snapshot.stage & 0xff) << 8)
                    | (snapshot.wait & 0xff);
            }
        }
        diagnostic_code_ = 0x4000'0000 | actor_code;
        shared_.progress(
            ProgressActor::Coordinator,
            ProgressStage::Failed,
            ProgressWait::None,
            diagnostic_code_);
        return false;
    }

    [[nodiscard]] auto await_children() noexcept -> bool {
        for (;;) {
            bool all_ready{true};
            for (myos_word_t index = 0; index < thread_count_; ++index) {
                if (shared_.load(index) != ChildReady + index) {
                    all_ready = false;
                }
            }
            if (all_ready) {
                return pressure_ || resilience_
                    || shared_.load(EndpointResultSlot) == EndpointTransfer;
            }
            if (!observe()) {
                return false;
            }
            myos::yield();
        }
    }

    /*luna change: wait on the pager proof's published barriers, reason: init observes the real Thread/Vproc completion edges without steering Pager state*/
    [[nodiscard]] auto exercise_pager() noexcept -> bool {
        /*luna change: wait the resilience phases through the same actor cells,
          reason: worker redelivery and pager_fail settlement are one proof
          sequence without a second barrier truth*/
        if (resilience_) {
            // WorkerFailed causally follows the replacement supply, Thread0's
            // value check and second fault. Intermediate actor-cell values are
            // projections and may advance before the coordinator samples them.
            while (shared_.load(PagerWorkerSlot) != PagerWorkerFailed
                || shared_.load(PagerVprocSlot) != PagerVprocDropped) {
                if (shared_.load(PagerDetailSlot) != 0 || !observe()) {
                    return false;
                }
                myos::yield();
            }
            return true;
        }
        /*luna change: select completion barriers from the frozen mode,
          reason: ordinary E1 waits only for its original PageIn/coalesce lane
          while pressure mode additionally waits for reclaim and writeback*/
        const myos_word_t thread_done = pressure_
            ? PagerThreadPressureReleased
            : PagerThreadDone;
        const myos_word_t worker_done = pressure_
            ? PagerWorkerWritebackDone
            : PagerWorkerSupplied;
        const myos_word_t vproc_done = pressure_
            ? PagerVprocDirtyRetryDone
            : PagerVprocDone;
        /*luna change: snapshot the Pager lane before pressure waiting, reason: a local actor-cell clock must ignore unrelated aggregate progress while ordinary E1 keeps its generic observer*/
        myos_word_t pager_thread{};
        myos_word_t pager_worker{};
        myos_word_t pager_vproc{};
        myos_word_t pager_last_tick{};
        bool pager_clock_ready{};
        if (pressure_) {
            pager_thread = shared_.load(PagerThreadSlot);
            pager_worker = shared_.load(PagerWorkerSlot);
            pager_vproc = shared_.load(PagerVprocSlot);
            if (observe_enabled_) {
                const auto now = myos::clock_now();
                if (now.status == MYOS_STATUS_OK) {
                    pager_last_tick = now.value;
                    pager_clock_ready = true;
                }
            }
        }
        while (shared_.load(PagerThreadSlot) != thread_done
            || shared_.load(PagerWorkerSlot) != worker_done
            || shared_.load(PagerVprocSlot) != vproc_done) {
            /*luna change: surface the first proof-only TargetVproc gate failure immediately, reason: a compact detail code is diagnostic projection and must not wait for the watchdog*/
            const myos_word_t detail = shared_.load(PagerDetailSlot);
            if (detail != 0) {
                diagnostic_code_ = 0x4000'0000 | (detail & 0xff);
                return false;
            }
            bool stalled{};
            if (pressure_) {
                const myos_word_t thread_state =
                    shared_.load(PagerThreadSlot);
                const myos_word_t worker_state =
                    shared_.load(PagerWorkerSlot);
                const myos_word_t vproc_state =
                    shared_.load(PagerVprocSlot);
                const bool progressed = thread_state != pager_thread
                    || worker_state != pager_worker
                    || vproc_state != pager_vproc;
                if (progressed) {
                    pager_thread = thread_state;
                    pager_worker = worker_state;
                    pager_vproc = vproc_state;
                }
                const auto now = myos::clock_now();
                if (now.status != MYOS_STATUS_OK || !observe_enabled_) {
                    pager_clock_ready = false;
                    stalled = !observe();
                } else if (!pager_clock_ready || progressed) {
                    pager_last_tick = now.value;
                    pager_clock_ready = true;
                } else if (now.value >= pager_last_tick
                    && now.value - pager_last_tick >= observe_hard_ticks_) {
                    stalled = true;
                }
            } else {
                stalled = !observe();
            }
            if (stalled) {
                /*luna change: pack the three Pager actor phases into the local
                  timeout projection, reason: the single-writer cells locate
                  pressure progress without changing waits or generic diagnostics*/
                const myos_word_t thread_state =
                    shared_.load(PagerThreadSlot);
                const myos_word_t worker_state =
                    shared_.load(PagerWorkerSlot);
                const myos_word_t vproc_state =
                    shared_.load(PagerVprocSlot);
                const myos_word_t thread_phase =
                    thread_state == PagerThreadFaulting ? 1
                    : thread_state == PagerThreadDone ? 2
                    : thread_state == PagerThreadCleanRetryDone ? 3
                    : thread_state == PagerThreadDirtyReady ? 4
                    : thread_state == PagerThreadPressureReleased ? 5
                    : 0;
                const myos_word_t worker_phase =
                    worker_state == PagerWorkerQueued ? 1
                    : worker_state == PagerWorkerClaimed ? 2
                    : worker_state == PagerWorkerSupplied ? 3
                    : worker_state == PagerWorkerMapAccepted ? 4
                    : worker_state == PagerWorkerWritten ? 5
                    : worker_state == PagerWorkerPrepared ? 6
                    : worker_state == PagerWorkerWritebackClaimed ? 7
                    : worker_state == PagerWorkerWritebackDone ? 8
                    : 0;
                const myos_word_t vproc_phase =
                    vproc_state == PagerVprocFaulting ? 1
                    : vproc_state == PagerVprocPending ? 2
                    : vproc_state == PagerVprocDone ? 3
                    : vproc_state == PagerVprocDirtyRetryDone ? 4
                    : 0;
                diagnostic_code_ = 0x5000'0000
                    | (thread_phase << 16)
                    | (worker_phase << 8)
                    | vproc_phase;
                shared_.progress(
                    ProgressActor::Coordinator,
                    ProgressStage::Failed,
                    ProgressWait::None,
                    diagnostic_code_);
                return false;
            }
            myos::yield();
        }
        return true;
    }

    [[nodiscard]] auto exercise_vproc() noexcept -> bool {
        while (shared_.load(VprocStateSlot) != VprocReady) {
            if (!observe()) {
                return false;
            }
            myos::yield();
        }
        if (myos::notification_signal(vproc_notification_).status
                != MYOS_STATUS_OK) {
            return false;
        }
        const myos_word_t expected = VprocComplete | VprocBadge;
        while (shared_.load(VprocStateSlot) != expected) {
            if (!observe()) {
                return false;
            }
            myos::yield();
        }
        while (shared_.load(ParkResultSlot) != ParkRejected
            || shared_.load(ParkWakeSlot) != ParkCommitted
            || shared_.load(TunnelDeliveryCountSlot) < 2
            || shared_.load(TunnelSourceStateSlot)
                != TunnelSecondInvoked) {
            if (!observe()) {
                return false;
            }
            myos::yield();
        }
        // The target may acknowledge the second kernel publication before the
        // source returns from tunnel_invoke().  SourceState is published after
        // SourceSequence, so its acquire observation closes that SMP ordering
        // before the two user-visible sequence values are compared.
        return shared_.load(TunnelSourceSequenceSlot) != 0
            && shared_.load(TunnelSourceSequenceSlot)
                == shared_.load(TunnelTargetSequenceSlot)
            && shared_.load(TunnelHeartbeatSlot) != 0;
    }

    [[nodiscard]] auto await_channel() noexcept -> bool {
        for (;;) {
            const myos_word_t failure = shared_.load(ChannelFailureSlot);
            if (failure == ChannelFailure) {
                return false;
            }
            if (shared_.load(ChannelCompleteSlot) == ChannelComplete) {
                return shared_.load(ChannelVprocSentSlot) == ChannelVprocSent
                    && shared_.load(ChannelVprocDoneSlot) == ChannelVprocDone
                    && shared_.load(ChannelSecondSentSlot) == ChannelSecondSent
                    && shared_.load(ChannelDrainReceivedSlot) == ChannelReady;
            }
            if (!observe()) {
                return false;
            }
            myos::yield();
        }
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
    /*luna change: retain the parsed root-role modes for one-way construction gates, reason: init must not infer run-mode semantics from child progress*/
    bool pressure_{};
    bool resilience_{};
    /*luna change: keep the doomed worker's capability for terminal evidence, reason: TerminalObservation needs the exact target across the child lifetime*/
    myos_cap_t worker_a_{};
    myos_cap_t pool_{};
    myos_cap_t child_vspace_{};
    myos_cap_t child_cspace_{};
    myos_cap_t bundle_region_{};
    myos_cap_t scratch_region_{};
    myos_cap_t shared_memory_{};
    /*luna change: retain Pager transport and exact region capabilities,
      reason: child delegation must preserve the AddressRegion authority that
      directly owns PagerAddress while Handles retains reverse-close lifetime*/
    myos_cap_t pager_{};
    myos_cap_t target_memory_{};
    myos_cap_t pager_region_{};
    myos_cap_t staging_memory_{};
    /*luna change: retain the worker's initially-unmapped source region,
      reason: children_ must keep its capability alive across later formal
      staging rematerialization*/
    myos_cap_t staging_region_{};
    /*luna change: retain the pressure Region and anonymous object for the existing Thread start handoff, reason: one child-owned authority pair must survive every bounded remap without a second truth source*/
    myos_cap_t stress_memory_{};
    myos_cap_t stress_region_{};
    myos_cap_t pager_notification_{};
    myos_cap_t notification_{};
    myos_cap_t vproc_notification_{};
    myos_cap_t control_memory_[VprocCount]{};
    myos_cap_t event_memory_[VprocCount]{};
    myos_cap_t ipc_memory_[VprocCount]{};
    myos_cap_t code_memory_{};
    myos_cap_t endpoint_ipc_memory_{};
    myos_word_t code_address_{};
    myos_word_t code_size_{};
    myos_word_t entry_{};
    Shared shared_{};
    myos_cap_t stack_memory_[MaxStacks]{};
    myos_word_t stack_bases_[MaxStacks]{};
    myos_word_t stack_tops_[MaxStacks]{};
    Handles children_{};
    bool closed_{};
    myos_word_t stage_{};
    bool observe_enabled_{};
    myos_word_t observe_frequency_{};
    myos_word_t observe_last_tick_{};
    myos_word_t observe_last_epoch_{};
    myos_word_t observe_hard_ticks_{};
    myos_word_t diagnostic_code_{};
};

// A service deployment has one owner for the child address space and one
// explicit capability handoff.  It deliberately does not reuse the proof
// loader's shared-memory protocol: UART only needs an ELF image, a stack, and
// the platform resources delegated by root init.
class UartLoader final {
public:
    UartLoader(
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
          uart_memory_(capability(
              bootstrap, MYOS_BOOTSTRAP_CAP_UART_MEMORY)),
          uart_irq_(capability(
              bootstrap, MYOS_BOOTSTRAP_CAP_UART_IRQ)),
          uart_notification_(capability(
              bootstrap, MYOS_BOOTSTRAP_CAP_UART_NOTIFICATION)) {}

    [[nodiscard]] auto run() noexcept -> bool {
        stage_ = 1;
        if (root_vspace_ == 0 || domain_ == 0 || bundle_ == 0
            || parent_pool_ == 0 || uart_memory_ == 0 || uart_irq_ == 0
            || uart_notification_ == 0) {
            return false;
        }
        if (!map_bundle()) {
            return false;
        }

        const auto package = myos::boot::Bundle::parse(
            reinterpret_cast<const void*>(UartBundleAddress), bundle_size_);
        myos::boot::Module module{};
        if (!package || !package.find("uart", module)
            || module.segment_count() == 0
            || module.segment_count() > MaxSegments) {
            return false;
        }
        entry_ = module.entry();
        for (myos_word_t index = 0; index < module.segment_count(); ++index) {
            myos::boot::Segment segment{};
            if (!module.segment(index, segment)) {
                return false;
            }
            const myos_word_t rounded = page_round(segment.memory_size);
            if (segment.memory_size != 0 && rounded == 0) {
                return false;
            }
            if (rounded > scratch_size_) {
                scratch_size_ = rounded;
            }
        }
        if (scratch_size_ < PageSize
            || !make_region(
                root_vspace_, UartScratchAddress, scratch_size_,
                MYOS_VM_READ | MYOS_VM_WRITE,
                MYOS_VM_NORMAL,
                MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP,
                scratch_region_)) {
            return false;
        }

        stage_ = 2;
        const auto child = myos::resource_create_child(
            parent_pool_, ChildMemory, ChildCaps, MYOS_RESOURCE_E7_KINDS);
        if (child.status != MYOS_STATUS_OK || !children_.add(child.value)) {
            return false;
        }
        pool_ = child.value;

        const auto vspace = myos::vspace_create(pool_);
        const auto cspace = myos::cspace_create(pool_, 64, 8);
        if (vspace.status != MYOS_STATUS_OK
            || cspace.status != MYOS_STATUS_OK
            || !children_.add(vspace.value)
            || !children_.add(cspace.value)) {
            return false;
        }
        child_vspace_ = vspace.value;
        child_cspace_ = cspace.value;

        stage_ = 3;
        for (myos_word_t index = 0; index < module.segment_count(); ++index) {
            myos::boot::Segment segment{};
            if (!module.segment(index, segment) || !load_segment(segment)) {
                return false;
            }
        }
        if (!make_stack() || !make_info() || !make_start()) {
            return false;
        }
        stage_ = 4;
        if (!start()) {
            return false;
        }

        // The child owns all live mappings and caps.  Only the temporary root
        // views used to copy the bundle and start record are closed here.
        (void)myos::cap_close(scratch_region_);
        (void)myos::cap_close(bundle_region_);
        scratch_region_ = 0;
        bundle_region_ = 0;
        stage_ = 0;
        return true;
    }

    [[nodiscard]] auto failure_code() const noexcept -> myos_word_t {
        return stage_;
    }

private:
    static constexpr myos_word_t MaxSegments = 16;
    static constexpr myos_word_t ChildMemory = 2 * 1024 * 1024;
    static constexpr myos_word_t ChildCaps = 128;
    static constexpr myos_word_t UartBundleAddress = 0x1400'0000;
    static constexpr myos_word_t UartScratchAddress = 0x1900'0000;
    static constexpr myos_word_t UartInfoAddress = 0x3000'0000;
    static constexpr myos_word_t UartMapAddress = 0x3001'0000;
    static constexpr myos_word_t UartStackAddress = 0x3002'0000;

    [[nodiscard]] auto make_region(
        myos_cap_t vspace,
        myos_word_t address,
        myos_word_t size,
        myos_word_t access,
        myos_word_t types,
        myos_word_t rights,
        myos_cap_t& result) noexcept -> bool {
        for (;;) {
            const auto region = myos::vm_create_region(
                vspace, address, size, access, types, rights);
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
                root_vspace_, UartBundleAddress, mapped_size, MYOS_VM_READ,
                MYOS_VM_NORMAL, MYOS_RIGHT_MAP, bundle_region_)
            && map(
                bundle_region_, bundle_, UartBundleAddress, mapped_size,
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
                scratch_region_, memory, UartScratchAddress, size,
                MYOS_VM_READ | MYOS_VM_WRITE)) {
            return false;
        }
        auto* const destination = reinterpret_cast<uint8_t*>(
            UartScratchAddress);
        for (myos_word_t index = 0; index < source_size; ++index) {
            destination[index] = source[index];
        }
        for (myos_word_t index = source_size; index < size; ++index) {
            destination[index] = 0;
        }
        return committed(myos::vm_unmap(
            scratch_region_, UartScratchAddress, size));
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
        if (segment.memory_size == 0) {
            return true;
        }
        const bool executable = (segment.access & MYOS_VM_EXECUTE) != 0;
        const myos_word_t load_access = segment.access
            | MYOS_VM_READ | MYOS_VM_WRITE;
        myos_cap_t memory{};
        if (size == 0
            || !create_memory(size, load_access, memory)
            || !write_memory(memory, size, segment.file, segment.file_size)
            || (executable && !seal(memory))) {
            return false;
        }
        myos_cap_t region{};
        return make_region(
                   child_vspace_, segment.address, size, segment.access,
                   MYOS_VM_NORMAL, MYOS_RIGHT_MAP, region)
            && children_.add(region)
            && map(region, memory, segment.address, size, segment.access);
    }

    [[nodiscard]] auto make_stack() noexcept -> bool {
        if (!create_memory(
                StackSize, MYOS_VM_READ | MYOS_VM_WRITE, stack_memory_)) {
            return false;
        }
        return make_region(
                   child_vspace_, UartStackAddress, StackSize,
                   MYOS_VM_READ | MYOS_VM_WRITE, MYOS_VM_NORMAL,
                   MYOS_RIGHT_MAP, stack_region_)
            && children_.add(stack_region_)
            && map(
                stack_region_, stack_memory_, UartStackAddress, StackSize,
                MYOS_VM_READ | MYOS_VM_WRITE);
    }

    [[nodiscard]] auto make_info() noexcept -> bool {
        if (!create_memory(
                PageSize, MYOS_VM_READ | MYOS_VM_WRITE, info_memory_)) {
            return false;
        }
        myos_cap_t child_info_region{};
        if (!make_region(
                child_vspace_, UartInfoAddress, PageSize, MYOS_VM_READ,
                MYOS_VM_NORMAL, MYOS_RIGHT_MAP, child_info_region)
            || !children_.add(child_info_region)
            || !map(
                scratch_region_, info_memory_, UartScratchAddress, PageSize,
                MYOS_VM_READ | MYOS_VM_WRITE)
            || !map(
                child_info_region, info_memory_, UartInfoAddress, PageSize,
                MYOS_VM_READ)) {
            return false;
        }

        const auto vspace = myos::cap_delegate(
            child_vspace_, child_cspace_,
            MYOS_RIGHT_CREATE_REGION | MYOS_RIGHT_MAP
                | MYOS_RIGHT_UNMAP | MYOS_RIGHT_INSPECT);
        const auto memory = myos::cap_delegate(
            uart_memory_, child_cspace_,
            MYOS_RIGHT_MAP | MYOS_RIGHT_INSPECT);
        const auto irq = myos::cap_delegate(
            uart_irq_, child_cspace_,
            MYOS_RIGHT_ROUTE | MYOS_RIGHT_ACK | MYOS_RIGHT_INSPECT);
        const auto notification = myos::cap_delegate(
            uart_notification_, child_cspace_,
            MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE
                | MYOS_RIGHT_INSPECT);
        if (vspace.status != MYOS_STATUS_OK
            || memory.status != MYOS_STATUS_OK
            || irq.status != MYOS_STATUS_OK
            || notification.status != MYOS_STATUS_OK) {
            return false;
        }

        myos_bootstrap_info info{};
        info.magic = MYOS_BOOTSTRAP_MAGIC;
        info.major = MYOS_BOOTSTRAP_MAJOR;
        info.minor = MYOS_BOOTSTRAP_MINOR;
        info.size = sizeof(info);
        info.cap_count = 4;
        info.cpu_count = 1;
        info.stack_base = UartStackAddress;
        info.stack_size = StackSize;
        info.boot_bundle_size = bundle_size_;
        info.caps[0] = myos_bootstrap_cap{
            .kind = MYOS_BOOTSTRAP_CAP_VSPACE,
            .flags = 0,
            .handle = vspace.value,
        };
        info.caps[1] = myos_bootstrap_cap{
            .kind = MYOS_BOOTSTRAP_CAP_UART_MEMORY,
            .flags = 0,
            .handle = memory.value,
        };
        info.caps[2] = myos_bootstrap_cap{
            .kind = MYOS_BOOTSTRAP_CAP_UART_IRQ,
            .flags = 0,
            .handle = irq.value,
        };
        info.caps[3] = myos_bootstrap_cap{
            .kind = MYOS_BOOTSTRAP_CAP_UART_NOTIFICATION,
            .flags = 0,
            .handle = notification.value,
        };
        auto* const destination = reinterpret_cast<myos_bootstrap_info*>(
            UartScratchAddress);
        *destination = info;
        return committed(myos::vm_unmap(
            scratch_region_, UartScratchAddress, PageSize));
    }

    [[nodiscard]] auto make_start() noexcept -> bool {
        if (!create_memory(
                PageSize, MYOS_VM_READ | MYOS_VM_WRITE, start_memory_)
            || !map(
                scratch_region_, start_memory_, UartScratchAddress, PageSize,
                MYOS_VM_READ | MYOS_VM_WRITE)) {
            return false;
        }
        auto* const start = reinterpret_cast<myos_thread_start*>(
            UartScratchAddress);
        start->version = MYOS_THREAD_START_VERSION;
        start->flags = 0;
        start->entry = entry_;
        start->stack = UartStackAddress + StackSize;
        start->arguments[0] = UartInfoAddress;
        start->arguments[1] = sizeof(myos_bootstrap_info);
        for (myos_word_t index = 2; index < 6; ++index) {
            start->arguments[index] = 0;
        }
        start->ipc = myos_ipc_binding{};
        return committed(myos::vm_unmap(
            scratch_region_, UartScratchAddress, PageSize));
    }

    [[nodiscard]] auto start() noexcept -> bool {
        const auto thread = myos::thread_create(
            pool_, child_vspace_, child_cspace_, start_memory_);
        if (thread.status != MYOS_STATUS_OK
            || !children_.add(thread.value)) {
            return false;
        }
        const auto context = myos::sc_create(
            pool_, domain_, 1'000'000, 10'000'000, 20, 0);
        if (context.status != MYOS_STATUS_OK
            || !children_.add(context.value)
            || myos::sc_bind(context.value, thread.value).status
                != MYOS_STATUS_OK) {
            return false;
        }
        return myos::execution_start(thread.value).status == MYOS_STATUS_OK;
    }

    myos_cap_t root_vspace_{};
    myos_cap_t domain_{};
    myos_cap_t bundle_{};
    myos_cap_t parent_pool_{};
    myos_word_t bundle_size_{};
    myos_cap_t uart_memory_{};
    myos_cap_t uart_irq_{};
    myos_cap_t uart_notification_{};
    myos_cap_t pool_{};
    myos_cap_t child_vspace_{};
    myos_cap_t child_cspace_{};
    myos_cap_t bundle_region_{};
    myos_cap_t scratch_region_{};
    myos_cap_t stack_region_{};
    myos_cap_t stack_memory_{};
    myos_cap_t info_memory_{};
    myos_cap_t start_memory_{};
    myos_word_t scratch_size_{PageSize};
    myos_word_t entry_{};
    Handles children_{};
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
    if (!complete) {
        fault(FailureFault + loader.failure_code());
    }
    UartLoader uart{*bootstrap, parent_pool};
    if (!uart.run()) {
        fault(FailureFault + 0x100 + uart.failure_code());
    }
    fault(SuccessFault);
}
