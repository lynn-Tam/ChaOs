#include <test/user/e7/protocol.hpp>
#include <user/lib/bootstrap.hpp>
#include <user/lib/boot_bundle.hpp>
#include <user/lib/deployment_syscall.hpp>
#include <user/lib/image_materializer.hpp>
#include <user/lib/syscall.hpp>
#include <user/lib/uart.hpp>
#include <uapi/bootstrap.h>

namespace {

using namespace myos::proof;

constexpr myos_word_t BundleAddress = 0x1000'0000;
constexpr myos_word_t ScratchAddress = 0x1800'0000;
constexpr myos_word_t TypedDescriptorSize = 4096;
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
constexpr myos_word_t UartAddress = 0x3001'0000;

void put_le16(uint8_t* bytes, size_t offset, uint16_t value) noexcept {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}
void put_le32(uint8_t* bytes, size_t offset, uint32_t value) noexcept {
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

void put_le64(uint8_t* bytes, size_t offset, uint64_t value) noexcept {
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

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

class UartRoot final {
public:
    [[nodiscard]] auto open(
        myos::cap::CapRef vspace,
        myos::cap::CapRef memory) noexcept -> bool {
        if (!vspace || vspace.cspace != 0 || !memory
            || memory.cspace != 0 || region_) {
            return false;
        }
        const auto created = myos::vm_create_region(
            vspace.selector,
            UartAddress,
            PageSize,
            MYOS_VM_READ | MYOS_VM_WRITE,
            MYOS_VM_DEVICE,
            MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP | MYOS_RIGHT_DESTROY);
        if (created.status != MYOS_STATUS_OK || created.value == 0) {
            return false;
        }
        region_ = myos::cap::OwnedCap{
            myos::cap::CapRef{created.value, 0}};
        const auto mapped = myos::vm_map(
            region_.selector(), memory.selector,
            UartAddress, PageSize, 0,
            MYOS_VM_READ | MYOS_VM_WRITE);
        if (!committed(mapped)) {
            return false;
        }
        mapped_ = true;
        myos::uart::Port{UartAddress}.reset();
        return true;
    }

    [[nodiscard]] auto close() noexcept -> bool {
        if (!region_) {
            return true;
        }
        if (mapped_) {
            for (;;) {
                const auto unmapped = myos::vm_unmap(
                    region_.selector(), UartAddress, PageSize);
                if (committed(unmapped)) {
                    mapped_ = false;
                    break;
                }
                if (!retryable(unmapped.status)) {
                    return false;
                }
                myos::yield();
            }
        }
        for (;;) {
            const auto destroyed = myos::vm_destroy_region(
                region_.selector());
            if (committed(destroyed)) {
                break;
            }
            if (!retryable(destroyed.status)) {
                return false;
            }
            myos::yield();
        }
        for (;;) {
            const myos_status_t closed = region_.close();
            if (closed == MYOS_STATUS_OK) {
                return true;
            }
            if (!retryable(closed)) {
                return false;
            }
            myos::yield();
        }
    }

private:
    myos::cap::OwnedCap region_{};
    bool mapped_{};
};

class Loader final {
public:
    using Backend = myos::cap::SyscallBackend;
    using Task = myos::cap::TaskSpace<128, 24>;
    using Bundle = myos::cap::MappedBundle;
    using Scratch = myos::cap::ScratchWindow;
    using Materializer = myos::deploy::ImageMaterializer<
        128, 24, Backend, 16, MaxStacks>;
    using Image = Materializer::Image;

    Loader(
        const myos::bootstrap::BootstrapView& bootstrap,
        myos_cap_t parent_pool) noexcept
        : root_vspace_(bootstrap.selector(MYOS_BOOTSTRAP_CAP_VSPACE)),
          domain_(bootstrap.selector(MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN)),
          bundle_(bootstrap.selector(MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE)),
          parent_pool_(parent_pool),
          bundle_size_(bootstrap.bundle_size()),
          thread_count_(bootstrap.cpu_count() < MaxThreads
                  ? bootstrap.cpu_count()
                  : MaxThreads) {}

    [[nodiscard]] auto run() noexcept -> bool {
        stage_ = 1;
        const myos::cap::CapRef root_vspace{root_vspace_, 0};
        const myos::cap::CapRef bundle{bundle_, 0};
        const myos::cap::CapRef parent_pool{parent_pool_, 0};
        if (!root_vspace || !domain_ || !bundle || !parent_pool
            || thread_count_ == 0 || bundle_size_ == 0) {
            return false;
        }
        const myos_word_t bundle_window_size = page_round(bundle_size_);
        if (bundle_window_size == 0
            || bundle_view_.open(
                root_vspace, bundle,
                myos::deploy::Window{
                    .address = BundleAddress,
                    .size = bundle_window_size},
                bundle_size_) != MYOS_STATUS_OK) {
            return false;
        }

        stage_ = 2;
        const auto* const package = bundle_view_.view();
        myos::boot::Module proof{};
        if (package == nullptr) {
            return false;
        }
        /*luna change: freeze the manifest-selected run mode, reason: only the
          bundle root role may authorize mode-only construction*/
        pressure_ = package->root_is("pressure");
        resilience_ = package->root_is("resilience");
        // Pressure and resilience are focused proofs. The sole ordinary
        // Thread owns the tested fault sequence; the second CPU Thread
        // belongs to E1.
        if (pressure_ || resilience_) {
            thread_count_ = 1;
        }
        if (!package->find("proof", proof)) {
            return false;
        }
        if (proof.segment_count() == 0
            || proof.segment_count() > MaxSegments) {
            return false;
        }
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
        if (scratch_.open(
                root_vspace,
                myos::deploy::Window{
                    .address = ScratchAddress,
                    .size = scratch_size},
                myos::deploy::Window{
                    .address = BundleAddress,
                    .size = bundle_window_size}) != MYOS_STATUS_OK) {
            return false;
        }

        stage_ = 4;
        if (task_.open(
                parent_pool,
                pressure_ ? PressureMemory : ChildMemory,
                ChildCaps,
                MYOS_RESOURCE_E6_KINDS | MYOS_RESOURCE_PAGER,
                32, 8) != MYOS_STATUS_OK) {
            return false;
        }
        const auto child_pool = task_.pool();
        const auto child_vspace = task_.lookup(
            task_.vspace_slot(), MYOS_OBJECT_KIND_VSPACE);
        const auto child_cspace = task_.lookup(
            task_.manager_slot(), MYOS_OBJECT_KIND_CSPACE);
        if (!child_pool.has_value() || !child_vspace.has_value()
            || !child_cspace.has_value()) {
            return false;
        }
        pool_ = child_pool->selector;
        child_vspace_ = child_vspace->selector;
        child_cspace_ = child_cspace->selector;

        stage_ = 6;
        Materializer materializer{task_, bundle_view_, scratch_};
        if (materializer.materialize_retained("proof", image_)
                != MYOS_STATUS_OK
            || materializer.materialize_stacks_retained(
                   thread_count_ + 3 * vproc_count() + endpoint_count()
                       + channel_count() + worker_count(),
                   StackAddress, StackStride, StackSize, image_)
                != MYOS_STATUS_OK
            || !cache_image_sources()) {
            return false;
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
        stage_ = 12;
        if (!make_vproc_runtime()) {
            return false;
        }
        stage_ = 13;
        if (!exercise_typed_delegate()) {
            return false;
        }
        if (!make_executions(materializer)) {
            return false;
        }
        if (materializer.retire_sources(image_) != MYOS_STATUS_OK) {
            return false;
        }
        clear_image_sources();
        if (!start_executions()) {
            return false;
        }
        if (!close_lease(scratch_) || !close_lease(bundle_view_)) {
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
        if (resilience_) {
            const auto worker = task_.lookup(
                worker_slot_, MYOS_OBJECT_KIND_THREAD);
            if (!worker.has_value()
                || myos::terminal_query(worker->selector).status
                    != MYOS_STATUS_OK) {
                return false;
            }
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

    [[nodiscard]] auto cleanup() noexcept -> bool {
        return close_lease(scratch_)
            && close_lease(bundle_view_)
            && close_task();
    }

private:
    static constexpr myos_word_t MaxSegments = 16;
    static constexpr myos_word_t max_word =
        static_cast<myos_word_t>(-1);

    struct Target final {
        myos::deploy::LocalSlot slot{};
        myos_object_kind_t kind{MYOS_OBJECT_KIND_INVALID};
    };

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

    [[nodiscard]] auto cache_image_sources() noexcept -> bool {
        entry_ = image_.entry;
        code_memory_ = 0;
        code_address_ = 0;
        code_size_ = 0;
        for (const auto& segment : image_.segments) {
            if (entry_ < segment.address
                || entry_ - segment.address >= segment.size) {
                continue;
            }
            const auto memory = task_.lookup(
                segment.memory, MYOS_OBJECT_KIND_MEMORY);
            if (!memory.has_value()) {
                return false;
            }
            code_memory_ = memory->selector;
            code_address_ = static_cast<myos_word_t>(segment.address);
            code_size_ = segment.size;
            break;
        }
        if (code_memory_ == 0 || code_size_ == 0
            || image_.stacks.size() > MaxStacks) {
            return false;
        }
        for (size_t index = 0; index < image_.stacks.size(); ++index) {
            const auto& stack = image_.stacks[index];
            const auto memory = task_.lookup(
                stack.mapping.memory, MYOS_OBJECT_KIND_MEMORY);
            if (!memory.has_value()) {
                return false;
            }
            stack_memory_[index] = memory->selector;
            stack_bases_[index] = static_cast<myos_word_t>(
                stack.mapping.address);
            stack_tops_[index] = stack.top;
        }
        return true;
    }

    // These selectors are construction-only projections.  Once all typed
    // constructors have snapshotted their descriptors, source retirement
    // clears the projections before any prepared child execution is exposed.
    void clear_image_sources() noexcept {
        code_memory_ = 0;
        code_address_ = 0;
        code_size_ = 0;
        entry_ = 0;
        for (size_t index = 0; index < MaxStacks; ++index) {
            stack_memory_[index] = 0;
            stack_bases_[index] = 0;
            stack_tops_[index] = 0;
        }
    }

    [[nodiscard]] auto adopt_local_selector(
        myos_cap_t selector,
        myos_object_kind_t kind) noexcept
        -> libk::optional<myos::deploy::LocalSlot> {
        if (selector == 0) {
            return libk::nullopt;
        }
        typename Task::owner_type owner{
            myos::cap::CapRef{selector, 0}};
        return task_.adopt_local(libk::move(owner), kind);
    }

    [[nodiscard]] auto retain_local(
        myos_cap_t selector,
        myos_object_kind_t kind) noexcept -> bool {
        return adopt_local_selector(selector, kind).has_value();
    }

    [[nodiscard]] auto delegate_remote(
        myos_cap_t source,
        myos_word_t rights,
        myos_cap_t& output) noexcept -> bool {
        if (source == 0 || child_cspace_ == 0
            || !task_.can_adopt_remote()) {
            return false;
        }
        const auto delegated = myos::cap_delegate(
            source, child_cspace_, rights);
        if (delegated.status != MYOS_STATUS_OK || delegated.value == 0) {
            return false;
        }
        typename Task::owner_type owner{myos::cap::CapRef{
            delegated.value, child_cspace_}};
        if (!task_.adopt_remote(libk::move(owner))) {
            return false;
        }
        output = delegated.value;
        return true;
    }

    [[nodiscard]] auto retain_remote(
        const myos::SysResult& result,
        myos_cap_t& output) noexcept -> bool {
        if (result.status != MYOS_STATUS_OK || result.value == 0
            || child_cspace_ == 0 || !task_.can_adopt_remote()) {
            return false;
        }
        typename Task::owner_type owner{myos::cap::CapRef{
            result.value, child_cspace_}};
        if (!task_.adopt_remote(libk::move(owner))) {
            return false;
        }
        output = result.value;
        return true;
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
                if (!retain_local(region.value, MYOS_OBJECT_KIND_VSPACE)) {
                    return false;
                }
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

    [[nodiscard]] auto create_memory(
        myos_word_t size,
        myos_word_t access,
        myos_cap_t& result,
        myos::deploy::LocalSlot* slot = nullptr) noexcept -> bool {
        const auto memory = myos::memory_create(pool_, size, access);
        if (memory.status != MYOS_STATUS_OK || memory.value == 0) {
            return false;
        }
        const auto adopted = adopt_local_selector(
            memory.value, MYOS_OBJECT_KIND_MEMORY);
        if (!adopted) {
            return false;
        }
        if (slot != nullptr) {
            *slot = *adopted;
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
            || (source_size != 0 && source == nullptr)
            || scratch_.map(
                   myos::cap::CapRef{memory, 0}, 0, size,
                   MYOS_VM_READ | MYOS_VM_WRITE) != MYOS_STATUS_OK) {
            return false;
        }
        auto* const destination = reinterpret_cast<uint8_t*>(
            ScratchAddress);
        if (Backend::memory_write(destination, source, source_size)
                != MYOS_STATUS_OK
            || Backend::memory_write(
                   destination + source_size, nullptr, size - source_size)
                != MYOS_STATUS_OK) {
            return false;
        }
        return scratch_.unmap() == MYOS_STATUS_OK;
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

    // Exercise the production typed-delegation ABI before any child
    // execution is published.  The descriptor is written through the same
    // MemoryObject/scratch path as deployment data; CSpace/GrantGraph remain
    // the only destination-slot and sponsorship owners.
    [[nodiscard]] auto exercise_typed_delegate() noexcept -> bool {
        myos_cap_t descriptor_memory{};
        myos::deploy::LocalSlot descriptor_slot{};
        if (!create_memory(
                TypedDescriptorSize,
                MYOS_VM_READ | MYOS_VM_WRITE,
                descriptor_memory,
                &descriptor_slot)) {
            return false;
        }

        uint8_t bytes[MYOS_CAP_ATTENUATION_SIZE]{};
        const auto fill = [&](myos_word_t rights) noexcept {
            for (uint8_t& value : bytes) {
                value = 0;
            }
            put_le16(
                bytes,
                MYOS_CAP_ATTENUATION_VERSION_OFFSET,
                MYOS_CAP_ATTENUATION_VERSION_CURRENT);
            put_le16(
                bytes,
                MYOS_CAP_ATTENUATION_KIND_OFFSET,
                MYOS_OBJECT_KIND_MEMORY);
            put_le32(
                bytes,
                MYOS_CAP_ATTENUATION_SIZE_OFFSET,
                MYOS_CAP_ATTENUATION_SIZE);
            put_le64(
                bytes,
                MYOS_CAP_ATTENUATION_RIGHTS_OFFSET,
                rights);
            put_le64(
                bytes,
                MYOS_CAP_ATTENUATION_WORD0_OFFSET,
                0);
            put_le64(
                bytes,
                MYOS_CAP_ATTENUATION_WORD1_OFFSET,
                1);
            put_le64(
                bytes,
                MYOS_CAP_ATTENUATION_WORD2_OFFSET,
                MYOS_VM_READ);
            put_le64(
                bytes,
                MYOS_CAP_ATTENUATION_WORD3_OFFSET,
                MYOS_VM_NORMAL);
        };

        const size_t before_remote = task_.remote_live_size();
        const auto typed_call = [&](
            myos_cap_t destination,
            myos_cap_t descriptor,
            myos_status_t expected,
            libk::optional<size_t>& remote_index,
            myos_cap_t& result_selector) noexcept -> bool {
            remote_index = libk::nullopt;
            result_selector = 0;
            const auto manager = task_.lookup(
                task_.manager_slot(), MYOS_OBJECT_KIND_CSPACE);
            if (!manager || manager->selector != child_cspace_
                || !task_.can_adopt_remote()) {
                return false;
            }
            const auto result = myos::cap_typed_delegate(
                code_memory_, destination, descriptor, 0);
            if (result.value != 0) {
                typename Task::owner_type owner{myos::cap::CapRef{
                    result.value, child_cspace_}};
                if (result.status != MYOS_STATUS_OK
                    || expected != MYOS_STATUS_OK) {
                    const myos_status_t closed = owner.close();
                    if (closed != MYOS_STATUS_OK) {
                        Backend::ownership_fault(closed);
                    }
                    return false;
                }
                const auto adopted = task_.adopt_remote_index(
                    libk::move(owner));
                if (!adopted) {
                    return false;
                }
                remote_index = *adopted;
                result_selector = result.value;
                return true;
            }
            return result.status == expected;
        };
        fill(MYOS_RIGHT_CONTROL);
        if (!write_memory(
                descriptor_memory,
                TypedDescriptorSize,
                bytes,
                MYOS_CAP_ATTENUATION_SIZE)) {
            return false;
        }
        libk::optional<size_t> remote_index{};
        myos_cap_t ignored_selector{};
        if (!typed_call(
                child_cspace_, descriptor_memory, MYOS_STATUS_DENIED,
                remote_index, ignored_selector)
            || remote_index.has_value()
            || task_.remote_live_size() != before_remote) {
            return false;
        }

        fill(MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_INSPECT);
        if (!write_memory(
                descriptor_memory,
                TypedDescriptorSize,
                bytes,
                MYOS_CAP_ATTENUATION_SIZE)) {
            return false;
        }
        myos_cap_t delegated_selector{};
        if (!typed_call(
                child_cspace_, descriptor_memory, MYOS_STATUS_OK,
                remote_index, delegated_selector)
            || !remote_index.has_value()) {
            return false;
        }
        const size_t valid_remote_index = remote_index.value();

        const auto wrong_manager = myos::cap_delegate(
            child_cspace_,
            0,
            MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_INSPECT);
        if (wrong_manager.status != MYOS_STATUS_OK
            || wrong_manager.value == 0) {
            return false;
        }
        const auto wrong_manager_slot = adopt_local_selector(
            wrong_manager.value, MYOS_OBJECT_KIND_CSPACE);
        if (!wrong_manager_slot) {
            return false;
        }
        if (myos::cap_close(delegated_selector, wrong_manager.value).status
            != MYOS_STATUS_BAD_RIGHTS) {
            return false;
        }
        const myos_status_t wrong_manager_close = task_.close_slot(
            *wrong_manager_slot);
        if (wrong_manager_close != MYOS_STATUS_OK) {
            return false;
        }
        const auto replacement_manager = myos::cap_delegate(
            child_cspace_,
            0,
            MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_INSPECT);
        if (replacement_manager.status != MYOS_STATUS_OK) {
            return false;
        }
        const auto replacement_manager_slot = adopt_local_selector(
            replacement_manager.value, MYOS_OBJECT_KIND_CSPACE);
        if (!replacement_manager_slot) {
            return false;
        }
        // CSpace reuses the just-closed free slot and advances its generation;
        // the journal index is an ownership index, not a kernel selector.
        if (replacement_manager.value == wrong_manager.value) {
            return false;
        }
        remote_index = libk::nullopt;
        if (!typed_call(
                wrong_manager.value, descriptor_memory,
                MYOS_STATUS_INVALID_CAP, remote_index, ignored_selector)
            || remote_index.has_value()
            || task_.remote_live_size() != before_remote + 1) {
            return false;
        }
        if (task_.close_remote(valid_remote_index) != MYOS_STATUS_OK) {
            return false;
        }
        if (task_.remote_live_size() != before_remote) {
            return false;
        }
        if (task_.close_slot(descriptor_slot) != MYOS_STATUS_OK) {
            return false;
        }
        myos_cap_t descriptor_replacement{};
        myos::deploy::LocalSlot descriptor_replacement_slot{};
        if (!create_memory(
                TypedDescriptorSize,
                MYOS_VM_READ | MYOS_VM_WRITE,
                descriptor_replacement,
                &descriptor_replacement_slot)
            || descriptor_replacement == descriptor_memory) {
            return false;
        }
        remote_index = libk::nullopt;
        if (!typed_call(
                child_cspace_, descriptor_memory,
                MYOS_STATUS_INVALID_CAP, remote_index, ignored_selector)
            || remote_index.has_value()
            || task_.remote_live_size() != before_remote) {
            return false;
        }
        return true;
    }

    /*luna change: construct one Pager-backed target and a resident staging page, reason: the proof must drive the existing request/claim/supply path rather than a test-only mapping*/
    [[nodiscard]] auto make_pager() noexcept -> bool {
        const auto pager = myos::pager_create(
            pool_, PagerBackingKey, 1);
        if (pager.status != MYOS_STATUS_OK
            || !retain_local(pager.value, MYOS_OBJECT_KIND_PAGER)) {
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
                )) {
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
            || !retain_local(target.value, MYOS_OBJECT_KIND_MEMORY)) {
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
                ) {
                return false;
            }
        }

        const auto notification = myos::notification_create(
            pool_, PagerBadge);
        if (notification.status != MYOS_STATUS_OK
            || !retain_local(notification.value,
                MYOS_OBJECT_KIND_NOTIFICATION)) {
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
            || !retain_local(created.value, MYOS_OBJECT_KIND_NOTIFICATION)) {
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
        myos_cap_t delegated{};
        if (!delegate_remote(
                notification_,
                MYOS_RIGHT_SIGNAL | MYOS_RIGHT_DUPLICATE
                    | MYOS_RIGHT_DELEGATE,
                delegated)) {
            return false;
        }
        shared_.store(NotificationSlot, delegated);

        // The focused pressure and resilience graphs have no ordinary
        // notification exercise.
        if (pressure_ || resilience_) {
            return true;
        }

        const auto vproc = myos::notification_create(pool_, VprocBadge);
        if (vproc.status != MYOS_STATUS_OK
            || !retain_local(vproc.value, MYOS_OBJECT_KIND_NOTIFICATION)) {
            return false;
        }
        vproc_notification_ = vproc.value;
        myos_cap_t waiter{};
        if (!delegate_remote(
                vproc_notification_, MYOS_RIGHT_RECEIVE, waiter)) {
            return false;
        }
        shared_.store(VprocNotificationSlot, waiter);
        shared_.store(VprocKeySlot, 0);
        shared_.store(VprocStateSlot, 0);
        return true;
    }

    [[nodiscard]] auto make_channel() noexcept -> bool {
        const auto created = myos::channel_create(
            pool_, 1, MYOS_CHANNEL_MAX_WORDS, 1, MYOS_CHANNEL_MAX_RELATIONS);
        if (created.status != MYOS_STATUS_OK
            || created.value == 0 || created.value2 == 0) {
            return false;
        }
        typename Task::owner_type channel_a{
            myos::cap::CapRef{created.value, 0}};
        typename Task::owner_type channel_b{
            myos::cap::CapRef{created.value2, 0}};
        const auto channel_a_slot = task_.adopt_local(
            libk::move(channel_a), MYOS_OBJECT_KIND_CHANNEL);
        if (!channel_a_slot.has_value()) {
            return false;
        }
        const auto channel_b_slot = task_.adopt_local(
            libk::move(channel_b), MYOS_OBJECT_KIND_CHANNEL);
        if (!channel_b_slot.has_value()) {
            return false;
        }
        const auto channel_a_ref = task_.lookup(
            channel_a_slot.value(), MYOS_OBJECT_KIND_CHANNEL);
        const auto channel_b_ref = task_.lookup(
            channel_b_slot.value(), MYOS_OBJECT_KIND_CHANNEL);
        if (!channel_a_ref.has_value() || !channel_b_ref.has_value()) {
            return false;
        }
        const auto sender = myos::channel_mint(
            channel_a_ref->selector,
            child_cspace_,
            10,
            MYOS_RIGHT_SEND | MYOS_RIGHT_CLOSE);
        myos_cap_t sender_cap{};
        if (!retain_remote(sender, sender_cap)) {
            return false;
        }
        const auto sender_alt = myos::channel_mint(
            channel_a_ref->selector,
            child_cspace_,
            20,
            MYOS_RIGHT_SEND | MYOS_RIGHT_CLOSE);
        myos_cap_t sender_alt_cap{};
        if (!retain_remote(sender_alt, sender_alt_cap)) {
            return false;
        }
        const auto receiver = myos::channel_mint(
            channel_b_ref->selector,
            child_cspace_,
            30,
            MYOS_RIGHT_RECEIVE | MYOS_RIGHT_CLOSE);
        myos_cap_t receiver_cap{};
        if (!retain_remote(receiver, receiver_cap)) {
            return false;
        }
        shared_.store(ChannelSenderSlot, sender_cap);
        shared_.store(ChannelSenderAltSlot, sender_alt_cap);
        shared_.store(ChannelReceiverSlot, receiver_cap);

        const auto notify_r = myos::notification_create(
            pool_, ChannelNotifyRBadge);
        if (notify_r.status != MYOS_STATUS_OK || notify_r.value == 0) {
            return false;
        }
        const auto notify_r_slot = adopt_local_selector(
            notify_r.value, MYOS_OBJECT_KIND_NOTIFICATION);
        if (!notify_r_slot.has_value()) {
            return false;
        }
        const auto notify_s = myos::notification_create(
            pool_, ChannelNotifySBadge);
        if (notify_s.status != MYOS_STATUS_OK || notify_s.value == 0) {
            return false;
        }
        const auto notify_s_slot = adopt_local_selector(
            notify_s.value, MYOS_OBJECT_KIND_NOTIFICATION);
        if (!notify_s_slot.has_value()) {
            return false;
        }
        const auto notify_r_ref = task_.lookup(
            notify_r_slot.value(), MYOS_OBJECT_KIND_NOTIFICATION);
        const auto notify_s_ref = task_.lookup(
            notify_s_slot.value(), MYOS_OBJECT_KIND_NOTIFICATION);
        if (!notify_r_ref.has_value() || !notify_s_ref.has_value()) {
            return false;
        }
        myos_cap_t notify_r_child{};
        myos_cap_t notify_s_child{};
        if (!delegate_remote(
                notify_r_ref->selector,
                MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE
                    | MYOS_RIGHT_DUPLICATE,
                notify_r_child)
            || !delegate_remote(
                notify_s_ref->selector,
                MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE
                    | MYOS_RIGHT_DUPLICATE,
                notify_s_child)) {
            return false;
        }
        shared_.store(ChannelNotifyRSlot, notify_r_child);
        shared_.store(ChannelNotifySSlot, notify_s_child);
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
                || !make_region(
                    child_vspace_,
                    event_address,
                    PageSize,
                    MYOS_VM_READ,
                    MYOS_RIGHT_MAP,
                    event_region)
                || !make_region(
                    child_vspace_,
                    ipc_address,
                    PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE,
                    MYOS_RIGHT_MAP,
                    ipc_region)
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

    template<typename Descriptor, typename Constructor>
    [[nodiscard]] auto construct_descriptor(
        Materializer& materializer,
        const Descriptor& descriptor,
        myos_object_kind_t kind,
        Constructor&& constructor,
        myos::deploy::LocalSlot& output) noexcept -> myos::SysResult {
        output = {};
        myos::deploy::LocalSlot slot{};
        const myos_status_t populated = materializer.materialize_descriptor(
            &descriptor, sizeof(descriptor), slot);
        if (populated != MYOS_STATUS_OK) {
            return {populated, 0, 0};
        }
        const auto reference = task_.lookup(slot, MYOS_OBJECT_KIND_MEMORY);
        if (!reference.has_value()) {
            (void)task_.close_slot(slot);
            return {MYOS_STATUS_INVALID_CAP, 0, 0};
        }
        const myos::SysResult result = constructor(reference->selector);
        if (result.status != MYOS_STATUS_OK) {
            (void)task_.close_slot(slot);
            return result;
        }
        const auto produced = adopt_local_selector(result.value, kind);
        if (!produced.has_value()) {
            (void)task_.close_slot(slot);
            return {MYOS_STATUS_NO_MEMORY, 0, 0};
        }
        output = produced.value();
        const myos_status_t closed = task_.close_slot(slot);
        if (closed != MYOS_STATUS_OK) {
            return {closed, 0, 0};
        }
        return result;
    }

    [[nodiscard]] auto make_executions(Materializer& materializer) noexcept
        -> bool {
        stage_ = 110;
        const myos_word_t active_vprocs = vproc_count();
        const myos_word_t active_channels = channel_count();
        const myos_word_t active_endpoints = endpoint_count();
        const myos_word_t worker_total = worker_count();
        target_count_ = 0;
        for (auto& target : targets_) {
            target = {};
        }
        myos_cap_t child_pool_cap{};
        myos_cap_t child_cspace_cap{};
        myos_cap_t arm_memory_cap{};
        myos_cap_t code_cap{};
        myos_cap_t pager_cap{};
        myos_cap_t target_cap{};
        myos_cap_t staging_cap{};
        myos_cap_t pager_notification_cap{};
        myos_cap_t staging_region_cap{};
        myos_cap_t stress_memory_cap{};
        myos_cap_t stress_region_cap{};
        myos_cap_t pager_region_cap{};
        if (!delegate_remote(pool_, MYOS_RIGHT_CREATE, child_pool_cap)
            || !delegate_remote(
                child_cspace_, MYOS_RIGHT_MANAGE, child_cspace_cap)
            || !delegate_remote(
                shared_memory_, MYOS_RIGHT_INSPECT, arm_memory_cap)
            || !delegate_remote(code_memory_, MYOS_RIGHT_MAP, code_cap)
            || !delegate_remote(
                pager_, MYOS_RIGHT_SERVE | MYOS_RIGHT_SUPPLY, pager_cap)
            || !delegate_remote(
                target_memory_, MYOS_RIGHT_MANAGE, target_cap)
            || !delegate_remote(
                staging_memory_,
                MYOS_RIGHT_MANAGE | (pressure_ ? MYOS_RIGHT_MAP : 0),
                staging_cap)
            || !delegate_remote(
                pager_notification_, MYOS_RIGHT_RECEIVE,
                pager_notification_cap)) {
            return false;
        }
        if (pressure_
            && (!delegate_remote(
                    staging_region_, MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP,
                    staging_region_cap)
                || !delegate_remote(
                    stress_memory_, MYOS_RIGHT_MAP, stress_memory_cap)
                || !delegate_remote(
                    stress_region_,
                    MYOS_RIGHT_MAP | MYOS_RIGHT_UNMAP,
                    stress_region_cap)
                || !delegate_remote(
                    pager_region_, MYOS_RIGHT_PROTECT, pager_region_cap))) {
            return false;
        }
        if (code_memory_ == 0 || code_size_ == 0
            || entry_ < code_address_
            || entry_ - code_address_ >= code_size_) {
            return false;
        }
        shared_.store(PoolSlot, child_pool_cap);
        shared_.store(CSpaceSlot, child_cspace_cap);
        shared_.store(PagerCapSlot, pager_cap);
        shared_.store(PagerTargetCapSlot, target_cap);
        shared_.store(PagerSourceCapSlot, staging_cap);
        shared_.store(PagerNotifyCapSlot, pager_notification_cap);
        if (pressure_) {
            shared_.store(PagerStagingRegionSlot, staging_region_cap);
            shared_.store(PagerRegionSlot, pager_region_cap);
        }

        myos_cap_t upcall_stacks[VprocCount]{};
        for (myos_word_t index = 0; index < active_vprocs; ++index) {
            const myos_word_t stack_index = thread_count_ + 3 * index + 2;
            if (!delegate_remote(
                    stack_memory_[stack_index], MYOS_RIGHT_MAP,
                    upcall_stacks[index])) {
                return false;
            }
        }

        auto make_thread = [&](const myos_thread_start& start,
                               myos_word_t home,
                               myos_word_t step,
                               myos_object_kind_t kind,
                               Target& target) noexcept -> bool {
            stage_ = step;
            const auto thread = construct_descriptor(
                materializer, start,
                kind,
                [&](myos_cap_t descriptor) noexcept {
                    return myos::thread_create(
                        pool_, child_vspace_, child_cspace_, descriptor, 0);
                },
                target.slot);
            if (thread.status != MYOS_STATUS_OK) {
                return false;
            }
            stage_ = step + 1;
            const auto context = myos::sc_create(
                pool_, domain_, 1'000'000, 10'000'000, 30, home);
            if (context.status != MYOS_STATUS_OK) {
                return false;
            }
            stage_ = step + 2;
            const auto context_slot = adopt_local_selector(
                context.value, MYOS_OBJECT_KIND_SCHED_CONTEXT);
            if (!context_slot.has_value()) {
                return false;
            }
            stage_ = step + 3;
            const auto thread_ref = task_.lookup(target.slot, kind);
            const auto context_ref = task_.lookup(
                context_slot.value(), MYOS_OBJECT_KIND_SCHED_CONTEXT);
            if (!thread_ref.has_value() || !context_ref.has_value()
                || myos::sc_bind(
                       context_ref->selector, thread_ref->selector).status
                != MYOS_STATUS_OK) {
                return false;
            }
            target.kind = kind;
            return true;
        };

        for (myos_word_t index = 0; index < thread_count_; ++index) {
            myos_thread_start start{};
            start.version = MYOS_THREAD_START_VERSION;
            start.entry = entry_;
            start.stack = stack_tops_[index];
            start.arguments[0] = SharedAddress;
            start.arguments[1] = index;
            if (pressure_ && index == 0) {
                start.arguments[2] = stress_region_cap;
                start.arguments[3] = stress_memory_cap;
            }
            start.ipc.memory = stack_memory_[index];
            start.ipc.address = stack_bases_[index];
            start.ipc.pages = 1;
            if (!make_thread(
                    start, index, 120 + index * 5,
                    MYOS_OBJECT_KIND_THREAD, targets_[index])) {
                return false;
            }
        }

        const myos_word_t channel_stack_base =
            thread_count_ + 3 * active_vprocs + active_endpoints;
        for (myos_word_t index = 0; index < active_channels; ++index) {
            const myos_word_t descriptor_index = thread_count_ + index;
            const myos_word_t stack_index = channel_stack_base + index;
            myos_thread_start start{};
            start.version = MYOS_THREAD_START_VERSION;
            start.entry = entry_;
            start.stack = stack_tops_[stack_index];
            start.arguments[0] = SharedAddress;
            start.arguments[1] = index == 0
                ? ChannelSenderMagic
                : ChannelReceiverMagic;
            start.arguments[2] = stack_bases_[stack_index];
            start.ipc.memory = stack_memory_[stack_index];
            start.ipc.address = stack_bases_[stack_index];
            start.ipc.pages = 1;
            const myos_word_t home = index == 1 && thread_count_ > 1
                ? 1 : 0;
            if (!make_thread(
                    start, home, 130 + index * 5,
                    MYOS_OBJECT_KIND_THREAD, targets_[descriptor_index])) {
                return false;
            }
        }

        const myos_word_t worker_descriptor = thread_count_ + active_channels;
        const myos_word_t worker_stack =
            thread_count_ + 3 * active_vprocs + active_endpoints
            + active_channels;
        for (myos_word_t index = 0; index < worker_total; ++index) {
            const myos_word_t stack = worker_stack + index;
            myos_thread_start start{};
            start.version = MYOS_THREAD_START_VERSION;
            start.entry = entry_;
            start.stack = stack_tops_[stack];
            start.arguments[0] = SharedAddress;
            start.arguments[1] = index == 0
                ? PagerWorkerMagic : PagerWorkerBMagic;
            if (index != 0) {
                start.arguments[4] = stack_bases_[worker_stack];
                start.arguments[5] = stack_bases_[stack];
            } else {
                start.arguments[5] = stack_bases_[stack];
            }
            start.ipc.memory = stack_memory_[stack];
            start.ipc.address = stack_bases_[stack];
            start.ipc.pages = 1;
            if (!make_thread(
                    start, 0, 138 + index,
                    MYOS_OBJECT_KIND_THREAD,
                    targets_[worker_descriptor + index])) {
                return false;
            }
            if (index == 0) {
                worker_slot_ = targets_[worker_descriptor].slot;
            }
        }
        if (resilience_) {
            const auto worker = task_.lookup(
                worker_slot_, MYOS_OBJECT_KIND_THREAD);
            if (!worker.has_value()
                || myos::terminal_observe_bind(
                       worker->selector, notification_, WorkerDeathBadge)
                       .status != MYOS_STATUS_OK) {
                return false;
            }
        }

        for (myos_word_t index = 0; index < active_vprocs; ++index) {
            const myos_word_t upcall_stack = thread_count_ + 3 * index + 2;
            myos_vproc_start start{};
            start.version = MYOS_VPROC_START_VERSION;
            start.entry = entry_;
            start.stack = stack_tops_[thread_count_ + 3 * index];
            start.arguments[0] = arm_memory_cap;
            start.arguments[1] = ArmDescriptorOffset
                + index * ArmDescriptorStride;
            start.arguments[2] = SharedAddress;
            start.arguments[3] = index == TargetVproc
                ? VprocMagic : SourceVprocMagic;
            start.arguments[4] = stack_tops_[thread_count_ + 3 * index + 1];
            start.arguments[5] = ChannelVprocIpcAddress + index * PageSize;
            start.control_memory = control_memory_[index];
            start.control_address = ControlAddress
                + index * VprocRuntimeStride;
            start.event_memory = event_memory_[index];
            start.event_address = EventAddress + index * VprocRuntimeStride;
            start.ipc.memory = ipc_memory_[index];
            start.ipc.address = ChannelVprocIpcAddress + index * PageSize;
            start.ipc.pages = 1;
            const auto vproc = construct_descriptor(
                materializer, start,
                MYOS_OBJECT_KIND_VPROC,
                [&](myos_cap_t descriptor) noexcept {
                    return myos::vproc_create(
                        pool_, child_vspace_, child_cspace_, descriptor, 0);
                },
                targets_[thread_count_ + active_channels + worker_total
                    + index].slot);
            if (vproc.status != MYOS_STATUS_OK) {
                return false;
            }
            const auto home = index == SourceVproc && thread_count_ > 1
                ? 1 : 0;
            const auto context = myos::sc_create(
                pool_, domain_, 1'000'000, 10'000'000, 30, home);
            if (context.status != MYOS_STATUS_OK) {
                return false;
            }
            const auto context_slot = adopt_local_selector(
                context.value, MYOS_OBJECT_KIND_SCHED_CONTEXT);
            const auto vproc_slot = targets_[
                thread_count_ + active_channels + worker_total + index].slot;
            if (!context_slot.has_value()) {
                return false;
            }
            const auto vproc_ref = task_.lookup(
                vproc_slot, MYOS_OBJECT_KIND_VPROC);
            const auto context_ref = task_.lookup(
                context_slot.value(), MYOS_OBJECT_KIND_SCHED_CONTEXT);
            if (!vproc_ref.has_value()
                || !context_ref.has_value()
                || myos::sc_bind(
                       context_ref->selector, vproc_ref->selector).status
                    != MYOS_STATUS_OK) {
                return false;
            }
            const myos_word_t code_page =
                (entry_ - code_address_) / PageSize;
            auto* const arm = reinterpret_cast<myos_vproc_arm*>(
                SharedAddress + ArmDescriptorOffset
                + index * ArmDescriptorStride);
            arm->version = MYOS_VPROC_ARM_VERSION;
            arm->flags = 0;
            arm->entry = entry_;
            arm->code_memory = code_cap;
            arm->code_page = code_page;
            arm->code_address = code_address_ + code_page * PageSize;
            arm->code_pages = 1;
            arm->stack_memory = upcall_stacks[index];
            arm->stack_page = StackSize / PageSize - 1;
            arm->stack_address = stack_bases_[upcall_stack] + StackSize
                - PageSize;
            arm->stack_pages = 1;
            arm->stack_top = stack_tops_[upcall_stack];
            targets_[thread_count_ + active_channels + worker_total + index]
                .kind = MYOS_OBJECT_KIND_VPROC;
        }

        if (!pressure_ && !resilience_) {
            const myos_word_t endpoint_stack =
                thread_count_ + 3 * active_vprocs;
            myos_cap_t endpoint_ipc_region{};
            if (!create_memory(
                    PageSize, MYOS_VM_READ | MYOS_VM_WRITE,
                    endpoint_ipc_memory_)
                || !make_region(
                    child_vspace_, EndpointIpcAddress, PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE, MYOS_RIGHT_MAP,
                    endpoint_ipc_region)
                || !map(
                    endpoint_ipc_region, endpoint_ipc_memory_,
                    EndpointIpcAddress, PageSize,
                    MYOS_VM_READ | MYOS_VM_WRITE)) {
                return false;
            }
            myos_endpoint_desc descriptor{};
            descriptor.version = MYOS_ENDPOINT_VERSION;
            descriptor.flags = MYOS_ENDPOINT_FLAGS_NONE;
            descriptor.entry = entry_;
            descriptor.code_memory = code_memory_;
            descriptor.code_address = code_address_;
            descriptor.code_pages = 1;
            descriptor.stack_memory = stack_memory_[endpoint_stack];
            descriptor.stack_address = stack_bases_[endpoint_stack];
            descriptor.stack_pages = StackSize / PageSize;
            descriptor.stack_stride = StackSize;
            descriptor.ipc.memory = endpoint_ipc_memory_;
            descriptor.ipc.address = EndpointIpcAddress;
            descriptor.ipc.pages = 1;
            descriptor.ipc_stride = PageSize;
            descriptor.activation_count = EndpointActivations;
            descriptor.queue_capacity = 2;
            descriptor.max_depth = 4;
            descriptor.budget_floor_ns = 1'000;
            descriptor.urgency_ceiling = 30;
            const auto endpoint = construct_descriptor(
                materializer, descriptor,
                MYOS_OBJECT_KIND_ENDPOINT,
                [&](myos_cap_t descriptor_memory) noexcept {
                    return myos::endpoint_create(
                        pool_, child_vspace_, child_cspace_,
                        descriptor_memory, 0);
                },
                endpoint_slot_);
            if (endpoint.status != MYOS_STATUS_OK) {
                return false;
            }
            const auto endpoint_ref = task_.lookup(
                endpoint_slot_, MYOS_OBJECT_KIND_ENDPOINT);
            if (!endpoint_ref.has_value()) {
                return false;
            }
            const auto caller = myos::endpoint_mint(
                endpoint_ref->selector, child_cspace_, EndpointBadge, 1,
                MYOS_RIGHT_CALL);
            myos_cap_t caller_cap{};
            if (!retain_remote(caller, caller_cap)) {
                return false;
            }
            shared_.store(EndpointSlot, caller_cap);
        }

        target_count_ = thread_count_ + active_channels
            + worker_total + active_vprocs;
        return true;
    }

    [[nodiscard]] auto start_executions() noexcept -> bool {
        myos::cap::CapRef resolved[
            MaxThreads + ChannelThreadCount + MaxPagerWorkers + VprocCount]{};
        for (myos_word_t index = 0; index < target_count_; ++index) {
            const auto target = task_.lookup(
                targets_[index].slot, targets_[index].kind);
            if (!target.has_value()) {
                return false;
            }
            resolved[index] = target.value();
        }
        for (myos_word_t index = 0; index < target_count_; ++index) {
            stage_ = 160 + index;
            if (myos::execution_start(resolved[index].selector).status
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
        if (task_.close() != MYOS_STATUS_OK) {
            return false;
        }
        closed_ = true;
        pool_ = 0;
        return true;
    }

    template<typename Lease>
    static auto close_lease(Lease& lease) noexcept -> bool {
        for (;;) {
            const myos_status_t status = lease.close();
            if (status == MYOS_STATUS_OK) {
                return true;
            }
            if (!retryable(status)) {
                Backend::ownership_fault(status);
            }
            myos::yield();
        }
    }

    [[nodiscard]] auto close_task() noexcept -> bool {
        for (;;) {
            const myos_status_t status = task_.close();
            if (status == MYOS_STATUS_OK) {
                return true;
            }
            if (!retryable(status)) {
                Backend::ownership_fault(status);
            }
            myos::yield();
        }
    }

    myos_cap_t root_vspace_{};
    myos_cap_t domain_{};
    myos_cap_t bundle_{};
    myos_cap_t parent_pool_{};
    myos_word_t bundle_size_{};
    myos_word_t thread_count_{};
    Task task_{};
    Bundle bundle_view_{};
    Scratch scratch_{};
    Image image_{};
    /*luna change: retain the parsed root-role modes for one-way construction gates, reason: init must not infer run-mode semantics from child progress*/
    bool pressure_{};
    bool resilience_{};
    /*luna change: keep the doomed worker's stable TaskSpace slot for terminal
      evidence, reason: the execution selector is a derived lookup only*/
    myos::deploy::LocalSlot worker_slot_{};
    myos_cap_t pool_{};
    myos_cap_t child_vspace_{};
    myos_cap_t child_cspace_{};
    myos_cap_t shared_memory_{};
    /*luna change: retain Pager transport and exact region capabilities,
      reason: child delegation must preserve the AddressRegion authority that
      directly owns PagerAddress*/
    myos_cap_t pager_{};
    myos_cap_t target_memory_{};
    myos_cap_t pager_region_{};
    myos_cap_t staging_memory_{};
    /*luna change: retain the worker's initially-unmapped source region,
      reason: the TaskSpace owns this authority across formal staging
      rematerialization*/
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
    Target targets_[
        MaxThreads + ChannelThreadCount + MaxPagerWorkers + VprocCount]{};
    myos_word_t target_count_{};
    myos::deploy::LocalSlot endpoint_slot_{};
    bool closed_{};
    myos_word_t stage_{};
    bool observe_enabled_{};
    myos_word_t observe_frequency_{};
    myos_word_t observe_last_tick_{};
    myos_word_t observe_last_epoch_{};
    myos_word_t observe_hard_ticks_{};
    myos_word_t diagnostic_code_{};
};

// UART keeps its capability/bootstrap policy here, while image, stack and
// descriptor population use the same bounded deployment path as proof.
class UartLoader final {
public:
    using Backend = myos::cap::SyscallBackend;
    using Task = myos::cap::TaskSpace<48, 8>;
    using Bundle = myos::cap::MappedBundle;
    using Scratch = myos::cap::ScratchWindow;
    using Materializer = myos::deploy::ImageMaterializer<
        48, 8, Backend, 16, 4>;
    using Image = Materializer::Image;

    UartLoader(
        const myos::bootstrap::BootstrapView& bootstrap,
        myos_cap_t parent_pool) noexcept
        : root_vspace_{bootstrap.selector(MYOS_BOOTSTRAP_CAP_VSPACE), 0},
          domain_{bootstrap.selector(MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN), 0},
          bundle_{bootstrap.selector(MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE), 0},
          parent_pool_{parent_pool, 0},
          bundle_size_{bootstrap.bundle_size()},
          uart_memory_{bootstrap.selector(MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY), 0},
          uart_irq_{bootstrap.selector(MYOS_BOOTSTRAP_CAP_IRQ), 0} {}

    [[nodiscard]] auto run() noexcept -> bool {
        stage_ = 1;
        if (!root_vspace_ || !domain_ || !bundle_ || !parent_pool_
            || !uart_memory_ || !uart_irq_
            || bundle_size_ == 0) {
            return false;
        }
        const myos_word_t bundle_window_size = page_round(bundle_size_);
        if (bundle_window_size == 0
            || bundle_view_.open(
                root_vspace_, bundle_,
                myos::deploy::Window{
                    .address = UartBundleAddress,
                    .size = bundle_window_size},
                bundle_size_) != MYOS_STATUS_OK) {
            return false;
        }

        const auto* const package = bundle_view_.view();
        myos::boot::Module module{};
        if (package == nullptr || !package->find("uart", module)
            || module.segment_count() == 0
            || module.segment_count() > MaxSegments) {
            return false;
        }
        scratch_size_ = PageSize;
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
        if (scratch_.open(
                root_vspace_,
                myos::deploy::Window{
                    .address = UartScratchAddress,
                    .size = scratch_size_}) != MYOS_STATUS_OK) {
            return false;
        }

        stage_ = 2;
        if (task_.open(
                parent_pool_, ChildMemory, ChildCaps,
                MYOS_RESOURCE_E7_KINDS, 64, 8) != MYOS_STATUS_OK) {
            return false;
        }
        /* The UART service relation is owned by this fixture's child pool.
         * The worker receives only the attenuated view delegated below; no
         * root bootstrap notification is involved. */
        const auto child_pool = task_.pool();
        if (!child_pool.has_value()) {
            return false;
        }
        const auto created = myos::notification_create(
            child_pool->selector, NotificationBadge);
        if (created.status != MYOS_STATUS_OK || created.value == 0) {
            return false;
        }
        myos::cap::OwnedCap notification_owner{myos::cap::CapRef{
            static_cast<myos_cap_t>(created.value), 0}};
        if (!task_.adopt_local(
                libk::move(notification_owner),
                MYOS_OBJECT_KIND_NOTIFICATION).has_value()) {
            return false;
        }
        uart_notification_ = myos::cap::CapRef{
            static_cast<myos_cap_t>(created.value), 0};
        const auto readiness_created = myos::notification_create(
            child_pool->selector, NotificationBadge);
        if (readiness_created.status != MYOS_STATUS_OK
            || readiness_created.value == 0) {
            return false;
        }
        myos::cap::OwnedCap readiness_owner{myos::cap::CapRef{
            static_cast<myos_cap_t>(readiness_created.value), 0}};
        if (!task_.adopt_local(
                libk::move(readiness_owner),
                MYOS_OBJECT_KIND_NOTIFICATION).has_value()) {
            return false;
        }
        uart_readiness_ = myos::cap::CapRef{
            static_cast<myos_cap_t>(readiness_created.value), 0};

        stage_ = 3;
        Materializer materializer{task_, bundle_view_, scratch_};
        if (materializer.materialize("uart", image_) != MYOS_STATUS_OK
            || materializer.materialize_stacks(
                1, UartStackAddress, StackSize, StackSize, image_)
                != MYOS_STATUS_OK) {
            return false;
        }
        entry_ = image_.entry;
        if (!make_info(materializer) || !make_start(materializer)) {
            return false;
        }

        stage_ = 4;
        const auto prepared_thread = prepare();
        if (!prepared_thread.has_value()) {
            return false;
        }
        thread_slot_ = prepared_thread.value();
        if (materializer.retire_sources(image_) != MYOS_STATUS_OK) {
            return false;
        }
        image_.clear();
        if (!publish()) {
            return false;
        }

        // Construction sources are retired after Thread snapshotting; the
        // child owns only its prepared mappings and delegated capabilities.
        if (scratch_.close() != MYOS_STATUS_OK
            || bundle_view_.close() != MYOS_STATUS_OK) {
            return false;
        }
        stage_ = 0;
        return true;
    }

    [[nodiscard]] auto cleanup() noexcept -> bool {
        return close_lease(scratch_)
            && close_lease(bundle_view_)
            && close_task();
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
    static constexpr myos_word_t UartStackAddress = 0x3002'0000;
    [[nodiscard]] auto manager() const noexcept
        -> libk::optional<myos::cap::CapRef> {
        return task_.lookup(
            task_.manager_slot(), MYOS_OBJECT_KIND_CSPACE);
    }

    [[nodiscard]] auto vspace() const noexcept
        -> libk::optional<myos::cap::CapRef> {
        return task_.lookup(
            task_.vspace_slot(), MYOS_OBJECT_KIND_VSPACE);
    }

    [[nodiscard]] auto pool() const noexcept
        -> libk::optional<myos::cap::CapRef> {
        return task_.pool();
    }

    [[nodiscard]] auto delegate_remote(
        myos::cap::CapRef source,
        myos_word_t rights,
        myos_cap_t& output) noexcept -> bool {
        const auto destination = manager();
        if (!source || !destination.has_value()
            || !task_.can_adopt_remote()) {
            return false;
        }
        const auto delegated = myos::cap_delegate(
            source.selector, destination->selector, rights);
        if (delegated.status != MYOS_STATUS_OK || delegated.value == 0) {
            return false;
        }
        output = delegated.value;
        myos::cap::OwnedCap owner{myos::cap::CapRef{
            delegated.value, destination->selector}};
        if (!task_.adopt_remote(libk::move(owner))) {
            return false;
        }
        return true;
    }

    [[nodiscard]] auto make_info(Materializer& materializer) noexcept -> bool {
        const auto child_vspace = vspace();
        const auto child_cspace = manager();
        if (!child_vspace.has_value() || !child_cspace.has_value()) {
            return false;
        }

        myos_cap_t vspace_cap{};
        myos_cap_t memory_cap{};
        myos_cap_t irq_cap{};
        myos_cap_t notification_cap{};
        myos_cap_t readiness_cap{};
        if (!delegate_remote(
                child_vspace.value(),
                MYOS_RIGHT_CREATE_REGION | MYOS_RIGHT_MAP,
                vspace_cap)
            || !delegate_remote(
                uart_memory_, MYOS_RIGHT_MAP,
                memory_cap)
            || !delegate_remote(
                uart_irq_, MYOS_RIGHT_ROUTE | MYOS_RIGHT_OBSERVE
                    | MYOS_RIGHT_ACK,
                irq_cap)
            || !delegate_remote(
                uart_notification_, MYOS_RIGHT_SIGNAL | MYOS_RIGHT_RECEIVE,
                notification_cap)
            || !delegate_remote(
                uart_readiness_, MYOS_RIGHT_SIGNAL,
                readiness_cap)) {
            return false;
        }

        myos_bootstrap_info info{};
        info.magic = MYOS_BOOTSTRAP_MAGIC;
        info.major = MYOS_BOOTSTRAP_MAJOR;
        info.minor = MYOS_BOOTSTRAP_MINOR;
        info.size = sizeof(info);
        info.cap_count = 5;
        info.cpu_count = 1;
        info.stack_base = UartStackAddress;
        info.stack_size = StackSize;
        info.boot_bundle_size = bundle_size_;
        info.caps[0] = myos_bootstrap_cap{
            .kind = MYOS_BOOTSTRAP_CAP_VSPACE,
            .flags = 0,
            .handle = vspace_cap,
        };
        info.caps[1] = myos_bootstrap_cap{
            .kind = MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY,
            .flags = 0,
            .handle = memory_cap,
        };
        info.caps[2] = myos_bootstrap_cap{
            .kind = MYOS_BOOTSTRAP_CAP_IRQ,
            .flags = 0,
            .handle = irq_cap,
        };
        info.caps[3] = myos_bootstrap_cap{
            .kind = MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION,
            .flags = 0,
            .handle = notification_cap,
        };
        info.caps[4] = myos_bootstrap_cap{
            .kind = MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION,
            .flags = 0,
            .handle = readiness_cap,
        };
        return materializer.materialize_readonly(
                   UartInfoAddress, &info, sizeof(info), info_mapping_)
            == MYOS_STATUS_OK;
    }

    [[nodiscard]] auto make_start(Materializer& materializer) noexcept -> bool {
        myos_thread_start start{};
        start.version = MYOS_THREAD_START_VERSION;
        start.flags = 0;
        start.entry = entry_;
        if (image_.stacks.empty()) {
            return false;
        }
        start.stack = image_.stacks[0].top;
        start.arguments[0] = UartInfoAddress;
        start.arguments[1] = sizeof(myos_bootstrap_info);
        start.ipc = myos_ipc_binding{};
        return materializer.materialize_descriptor(
                   &start, sizeof(start), start_slot_)
            == MYOS_STATUS_OK;
    }

    [[nodiscard]] auto prepare() noexcept
        -> libk::optional<myos::deploy::LocalSlot> {
        const auto pool_cap = pool();
        const auto child_vspace = vspace();
        const auto child_cspace = manager();
        const auto descriptor = task_.lookup(
            start_slot_, MYOS_OBJECT_KIND_MEMORY);
        if (!pool_cap.has_value() || !child_vspace.has_value()
            || !child_cspace.has_value() || !descriptor.has_value()) {
            return libk::nullopt;
        }
        const auto thread = myos::thread_create(
            pool_cap->selector, child_vspace->selector,
            child_cspace->selector, descriptor->selector);
        if (thread.status != MYOS_STATUS_OK || thread.value == 0) {
            return libk::nullopt;
        }
        myos::cap::OwnedCap thread_owner{
            myos::cap::CapRef{
                static_cast<myos_cap_t>(thread.value), 0}};
        const auto thread_slot = task_.adopt_local(
            libk::move(thread_owner), MYOS_OBJECT_KIND_THREAD);
        if (!thread_slot.has_value()) {
            return libk::nullopt;
        }
        if (task_.close_slot(start_slot_) != MYOS_STATUS_OK) {
            return libk::nullopt;
        }
        start_slot_ = {};

        const auto context = myos::sc_create(
            pool_cap->selector, domain_.selector,
            1'000'000, 10'000'000, 20, 0);
        if (context.status != MYOS_STATUS_OK || context.value == 0) {
            return libk::nullopt;
        }
        myos::cap::OwnedCap context_owner{
            myos::cap::CapRef{
                static_cast<myos_cap_t>(context.value), 0}};
        const auto context_slot = task_.adopt_local(
            libk::move(context_owner), MYOS_OBJECT_KIND_SCHED_CONTEXT);
        if (!context_slot.has_value()
            || myos::sc_bind(context.value, thread.value).status
                != MYOS_STATUS_OK) {
            return libk::nullopt;
        }
        return thread_slot;
    }

    [[nodiscard]] auto publish() noexcept -> bool {
        const auto retained_thread = task_.lookup(
            thread_slot_, MYOS_OBJECT_KIND_THREAD);
        return retained_thread.has_value()
            && myos::execution_start(retained_thread->selector).status
                == MYOS_STATUS_OK;
    }

    template<typename Lease>
    static auto close_lease(Lease& lease) noexcept -> bool {
        for (;;) {
            const myos_status_t status = lease.close();
            if (status == MYOS_STATUS_OK) {
                return true;
            }
            if (!retryable(status)) {
                Backend::ownership_fault(status);
            }
            myos::yield();
        }
    }

    [[nodiscard]] auto close_task() noexcept -> bool {
        for (;;) {
            const myos_status_t status = task_.close();
            if (status == MYOS_STATUS_OK) {
                return true;
            }
            if (!retryable(status)) {
                Backend::ownership_fault(status);
            }
            myos::yield();
        }
    }

    myos::cap::CapRef root_vspace_{};
    myos::cap::CapRef domain_{};
    myos::cap::CapRef bundle_{};
    myos::cap::CapRef parent_pool_{};
    myos_word_t bundle_size_{};
    myos::cap::CapRef uart_memory_{};
    myos::cap::CapRef uart_irq_{};
    myos::cap::CapRef uart_notification_{};
    myos::cap::CapRef uart_readiness_{};
    Task task_{};
    Bundle bundle_view_{};
    Scratch scratch_{};
    Image image_{};
    typename Image::Mapping info_mapping_{};
    myos::deploy::LocalSlot start_slot_{};
    myos::deploy::LocalSlot thread_slot_{};
    myos_word_t scratch_size_{PageSize};
    myos_word_t entry_{};
    myos_word_t stage_{};
};

} // namespace

//Confirmatory experiment.
// Exit condition: replace the proof child and shared flag rendezvous with the
// first persistent process service once Endpoint IPC exists.
extern "C" void myos_main(
    myos_word_t bootstrap_address,
    myos_word_t bootstrap_size) noexcept {
    const auto bootstrap = myos::bootstrap::BootstrapView::parse(
        reinterpret_cast<const void*>(bootstrap_address), bootstrap_size);
    if (!bootstrap || bootstrap->cpu_count() == 0
        || bootstrap->bundle_size() == 0) {
        fault(FailureFault);
    }
    const myos_cap_t parent_pool = bootstrap->selector(
        MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    if (parent_pool == 0) {
        fault(FailureFault);
    }

    UartRoot uart_root{};
    if (!uart_root.open(
            myos::cap::CapRef{
                bootstrap->selector(MYOS_BOOTSTRAP_CAP_VSPACE), 0},
            myos::cap::CapRef{
                bootstrap->selector(MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY), 0})) {
        fault(FailureFault);
    }
    Loader loader{*bootstrap, parent_pool};
    const bool complete = loader.run();
    if (!loader.cleanup()) {
        myos::cap::SyscallBackend::ownership_fault(MYOS_STATUS_BUSY);
    }
    if (!complete) {
        if (!uart_root.close()) {
            myos::cap::SyscallBackend::ownership_fault(MYOS_STATUS_BUSY);
        }
        fault(FailureFault + loader.failure_code());
    }
    UartLoader uart{*bootstrap, parent_pool};
    if (!uart.run()) {
        if (!uart.cleanup()) {
            myos::cap::SyscallBackend::ownership_fault(MYOS_STATUS_BUSY);
        }
        if (!uart_root.close()) {
            myos::cap::SyscallBackend::ownership_fault(MYOS_STATUS_BUSY);
        }
        fault(FailureFault + 0x100 + uart.failure_code());
    }
    if (!uart_root.close()) {
        myos::cap::SyscallBackend::ownership_fault(MYOS_STATUS_BUSY);
    }
    fault(SuccessFault);
}
