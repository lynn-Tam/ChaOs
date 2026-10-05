#pragma once

#include <base/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <optional>
#include <libk/sync/atomic.hpp>
#include <mm/pmm.hpp>

class CpuSetup;
struct CpuRuntime;

enum class CpuState : u32 {
    Possible,
    Present,
    Prepared,
    Starting,
    Online,
    Failed,
};

enum class CpuFailure : u8 {
    None,
    FirmwareReported,
    MetadataAllocation,
    StackAllocation,
    ObjectAllocation,
    HsmUnavailable,
    InvalidHardwareId,
    InvalidEntryAddress,
    AlreadyStarted,
    FirmwareRejected,
};

struct CpuSnapshot final {
    usize possible{};
    usize present{};
    usize prepared{};
    usize starting{};
    usize online{};
    usize failed{};
};

class CpuDescriptor final {
public:
    constexpr CpuDescriptor() noexcept = default;

    [[nodiscard]] constexpr auto logical_id() const noexcept -> CpuId {
        return logical_id_;
    }
    [[nodiscard]] constexpr auto hardware_id() const noexcept
        -> CpuHwId {
        return hardware_id_;
    }
    [[nodiscard]] constexpr auto availability() const noexcept
        -> CpuAvail {
        return availability_;
    }

    // Acquire is the only public lifecycle observation. Seeing Prepared or a
    // later publication permits borrowing the stable runtime association.
    [[nodiscard]] auto state() const noexcept -> CpuState {
        return state_.load<libk::MemoryOrder::Acquire>();
    }

    [[nodiscard]] auto failure() const noexcept
        -> std::optional<CpuFailure> {
        return state() == CpuState::Failed
            ? std::optional<CpuFailure>{failure_}
            : std::nullopt;
    }

    // Runtime-backed diagnostic leases use this stable descriptor as their
    // lifetime witness.  The registry clears runtime_ after destroying the
    // runtime, so late lease destruction can become a harmless no-op instead
    // of touching a released diagnostics page.
    [[nodiscard]] auto runtime_alive() const noexcept -> bool {
        return runtime_ != nullptr;
    }

private:
    friend class CpuRegistry;

    CpuId logical_id_{};
    CpuHwId hardware_id_{};
    CpuAvail availability_{CpuAvail::Disabled};
    libk::Atomic<CpuState> state_{CpuState::Possible};
    CpuFailure failure_{CpuFailure::None};
    CpuRuntime* runtime_{};
};

class CpuRegistry final : private libk::noncopyable_nonmovable {
    class ConstructionKey {
        friend class CpuRegistry;
        constexpr ConstructionKey() noexcept = default;
    };

public:
    enum class Error : u8 {
        InvalidTopology,
        DuplicateHardwareId,
        MetadataAllocation,
        AlreadyFinished,
    };

    using Result = std::expected<void, Error>;

    class Builder final {
    public:
        Builder(const Builder&) = delete;
        auto operator=(const Builder&) -> Builder& = delete;
        Builder(Builder&& other) noexcept;
        auto operator=(Builder&&) -> Builder& = delete;
        ~Builder() noexcept;

        [[nodiscard]] auto append(
            CpuHwId hardware_id,
            CpuAvail availability) noexcept -> Result;
        [[nodiscard]] auto finish() noexcept -> Result;

    private:
        friend class CpuRegistry;
        Builder(
            libk::ManualLifetime<CpuRegistry>& storage,
            CpuRegistry& registry) noexcept;

        libk::ManualLifetime<CpuRegistry>* storage_{};
        CpuRegistry* registry_{};
    };

    using BeginResult = std::expected<Builder, Error>;

    [[nodiscard]] static auto begin(
        libk::ManualLifetime<CpuRegistry>& storage,
        mm::Pmm& pmm,
        CpuTopo summary) noexcept -> BeginResult;

    explicit CpuRegistry(
        ConstructionKey key,
        mm::Pmm& pmm,
        CpuTopo summary) noexcept;
    ~CpuRegistry() noexcept;

    [[nodiscard]] auto count() const noexcept -> usize { return count_; }
    [[nodiscard]] auto boot_id() const noexcept -> CpuId {
        return CpuId{boot_index_};
    }

    [[nodiscard]] auto descriptor(CpuId id) const noexcept
        -> const CpuDescriptor*;
    [[nodiscard]] auto runtime(CpuId id) noexcept -> CpuRuntime*;
    [[nodiscard]] auto runtime(CpuId id) const noexcept -> const CpuRuntime*;
    [[nodiscard]] auto runtime_by_hardware_id(
        CpuHwId hardware_id) noexcept -> CpuRuntime*;
    // AcqRel consumes Prepared publication and commits one start authority.
    [[nodiscard]] auto begin_start(CpuId id) noexcept -> bool;
    [[nodiscard]] auto fail_start(CpuId id, CpuFailure failure) noexcept
        -> bool;
    [[nodiscard]] auto publish_online(CpuRuntime& runtime) noexcept -> bool;

    [[nodiscard]] auto snapshot() const noexcept -> CpuSnapshot;

private:
    friend class CpuSetup;

    struct CpuRecordBlock;
    struct RuntimeTarget final {
        CpuDescriptor* descriptor{};
        void* storage{};
    };
    enum class RuntimeReserveError : u8 {
        InvalidState,
        MetadataAllocation,
    };

    using RuntimeTargetResult =
        std::expected<RuntimeTarget, RuntimeReserveError>;

    [[nodiscard]] auto allocate_blocks() noexcept -> Result;
    [[nodiscard]] auto allocate_storage(
        usize size,
        usize alignment) noexcept -> void*;
    [[nodiscard]] auto extend_storage() noexcept -> bool;
    [[nodiscard]] auto append_record(
        CpuHwId hardware_id,
        CpuAvail availability) noexcept -> Result;
    [[nodiscard]] auto finish_records() noexcept -> Result;
    [[nodiscard]] auto slot_at(usize index) noexcept -> CpuDescriptor*;
    [[nodiscard]] auto slot_at(usize index) const noexcept
        -> const CpuDescriptor*;
    [[nodiscard]] auto record_at(usize index) noexcept -> CpuDescriptor*;
    [[nodiscard]] auto record_at(usize index) const noexcept
        -> const CpuDescriptor*;
    [[nodiscard]] auto find_record(CpuHwId hardware_id) noexcept
        -> CpuDescriptor*;
    [[nodiscard]] auto find_record(CpuHwId hardware_id) const noexcept
        -> const CpuDescriptor*;
    [[nodiscard]] auto mutable_descriptor(CpuId id) noexcept
        -> CpuDescriptor*;
    [[nodiscard]] auto reserve_runtime(CpuId id) noexcept
        -> RuntimeTargetResult;

    void fail_unpublished(CpuDescriptor& cpu, CpuFailure failure) noexcept;
    void fail_runtime(RuntimeTarget target, CpuFailure failure) noexcept;
    void publish_runtime(
        RuntimeTarget target,
        CpuRuntime& runtime) noexcept;

    // Registry-private monotonic storage keeps published descriptor/runtime
    // addresses stable. The complete page group is reclaimed with the registry.
    mm::PageGroup storage_pages_;
    uptr storage_cursor_{};
    uptr storage_limit_{};
    CpuRecordBlock* first_block_{};
    CpuRecordBlock* last_block_{};
    usize count_{};
    usize appended_{};
    usize boot_index_{};
    bool finished_{};
};
