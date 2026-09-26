#pragma once

#include <arch/iommu.hpp>
#include <arch/pci.hpp>
#include <boot/boot_info.hpp>
#include <io/device.hpp>
#include <libk/inplace_vector.hpp>
#include <object/object_ref.hpp>
#include <time/clock.hpp>

namespace kernel::object { class ObjectStore; }

namespace arch {

// Machine-lifetime hardware ownership. Device capabilities reference this
// platform; worker pools own IOSpace generations, never the controller pages.
class IoPlatform final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto initialize(const kernel::boot::BootInfo& boot, kernel::mm::Pmm& pmm,
        const kernel::time::Clock& clock, kernel::object::ObjectStore& objects) noexcept -> bool;
    [[nodiscard]] auto count() const noexcept -> usize { return devices_.size(); }
    [[nodiscard]] auto present() const noexcept -> bool { return !devices_.empty(); }
    [[nodiscard]] auto device(usize index = 0) noexcept -> kernel::io::Device& { return devices_[index].get(); }
    [[nodiscard]] auto reference(usize index = 0) const noexcept { return devices_[index].ref(); }

private:
    static auto fault_irq(void* context) noexcept -> bool;
    Iommu iommu_{};
    PciFunction::Functions discovery_{};
    libk::InplaceVector<kernel::object::ObjectHold<kernel::io::Device>, virt_pci_irq_count> devices_{};
};

} // namespace arch
