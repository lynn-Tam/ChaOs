#pragma once

#include <arch/iommu.hpp>
#include <arch/pci.hpp>
#include <io/device.hpp>
#include <object/device_pool.hpp>
#include <boot/boot_info.hpp>
#include <time/clock.hpp>

namespace kernel::object { class ObjectStore; }

namespace kernel::io {

// Machine-lifetime hardware ownership. Device capabilities reference this
// platform; worker pools own IOSpace generations, never the controller pages.
class Platform final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto initialize(const boot::BootInfo& boot, mm::Pmm& pmm,
        const time::Clock& clock, object::ObjectStore& objects) noexcept -> bool;
    [[nodiscard]] auto present() const noexcept -> bool { return static_cast<bool>(device_); }
    [[nodiscard]] auto device() noexcept -> Device& { return device_.get(); }
    [[nodiscard]] auto reference() const noexcept { return device_.ref(); }

private:
    arch::Iommu iommu_{};
    object::DeviceHold device_{};
};

} // namespace kernel::io
