#pragma once

#include <arch/iommu.hpp>
#include <arch/pci.hpp>
#include <io/device.hpp>
#include <libk/inplace_vector.hpp>
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
    [[nodiscard]] auto count() const noexcept -> usize { return devices_.size(); }
    [[nodiscard]] auto present() const noexcept -> bool { return !devices_.empty(); }
    [[nodiscard]] auto device(usize index = 0) noexcept -> Device& { return devices_[index].get(); }
    [[nodiscard]] auto reference(usize index = 0) const noexcept { return devices_[index].ref(); }

private:
    static auto fault_irq(void* context) noexcept -> bool;
    arch::Iommu iommu_{};
    arch::PciFunction::Functions discovery_{};
    libk::InplaceVector<object::DeviceHold, arch::virt_pci_irq_count> devices_{};
};

} // namespace kernel::io
