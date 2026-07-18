#pragma once

#include <boot/boot_info.hpp>
#include <image/boot_bundle.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <mm/direct_map.hpp>
#include <mm/pmm.hpp>
#include <object/object_store.hpp>
#include <uapi/bootstrap.h>

namespace kernel::init {

class RootTask;

} // namespace kernel::init

namespace kernel {
class KernelState;
struct CpuRuntime;
}

namespace kernel::init {

enum class RootTaskError : u8 {
    InvalidModule,
    InvalidBundle,
    Ownership,
    OutOfMemory,
    InvalidState,
    MappingFailed,
    CapabilityFailed,
    SchedulingFailed,
};

// Owns the boot package and, later, the one root user deployment assembled
// from it. It is separate from KernelState because boot policy is not a
// permanent kernel service or a general process loader.
class RootTask final : private libk::noncopyable_nonmovable {
    class ConstructionKey {
        friend class RootTask;
        constexpr ConstructionKey() noexcept = default;
    };

public:
    [[nodiscard]] static auto initialize_in(
        libk::ManualLifetime<RootTask>& storage,
        kernel::object::ObjectStore& objects,
        kernel::mm::Pmm& pmm,
        kernel::mm::DirectMap& direct_map,
        kernel::object::ObjectStore::ResourceHold&& pool,
        kernel::boot::BootModule module,
        kernel::mm::BootReservation&& reservation) noexcept
        -> libk::Expected<void, RootTaskError>;

    RootTask(
        [[maybe_unused]] ConstructionKey key,
        kernel::mm::DirectMap& direct_map,
        kernel::boot::BootModule module) noexcept
        : direct_map_(&direct_map), module_(module) {}

    [[nodiscard]] auto bundle() const noexcept
        -> libk::Expected<kernel::image::BootBundle, kernel::image::BundleError>;
    [[nodiscard]] auto start(
        kernel::KernelState& kernel,
        kernel::CpuRuntime& runtime) noexcept
        -> libk::Expected<void, RootTaskError>;
    [[nodiscard]] auto image() const noexcept
        -> const kernel::object::ObjectStore::MemoryHold& {
        return image_;
    }

private:
    [[nodiscard]] auto reserve(kernel::resource::Budget charge) noexcept
        -> libk::Expected<kernel::resource::Reservation, RootTaskError>;
    [[nodiscard]] auto prepare_bootstrap(kernel::KernelState& kernel) noexcept
        -> libk::Expected<void, RootTaskError>;
    void rollback(kernel::KernelState& kernel) noexcept;

    kernel::mm::DirectMap* direct_map_{};
    kernel::boot::BootModule module_{};
    kernel::object::ObjectStore::ResourceHold pool_{};
    kernel::object::ObjectStore::MemoryHold image_{};
    libk::InplaceVector<
        kernel::object::ObjectStore::MemoryHold,
        kernel::image::max_boot_segments> segments_{};
    kernel::object::ObjectStore::MemoryHold stack_{};
    kernel::object::ObjectStore::MemoryHold info_{};
    kernel::object::ObjectStore::VSpaceHold vspace_{};
    kernel::object::ObjectStore::CSpaceHold cspace_{};
    kernel::object::ObjectStore::ThreadHold thread_{};
    kernel::object::ObjectStore::SchedulingContextHold context_{};
    bool started_{};
};

} // namespace kernel::init
