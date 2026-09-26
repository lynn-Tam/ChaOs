#include <arch/io_platform.hpp>

#include <diag/console.hpp>
#include <libk/utility.hpp>
#include <object/object_store.hpp>
#include <irq/irq.hpp>

namespace arch {

auto IoPlatform::initialize(const kernel::boot::BootInfo& boot, kernel::mm::Pmm& pmm,
    const kernel::time::Clock& clock, kernel::object::ObjectStore& objects) noexcept -> bool {
    if (!boot.iommu) return true;
    if (boot.iommu->first().base().raw() != virt_iommu_base
        || boot.iommu->page_count() != 1) return false;
    auto discovered = PciFunction::discover_blocks(discovery_);
    if (!discovered) return discovered.error() == PciError::Absent;
    if (!iommu_.start(pmm)) return false;
    const auto timeout = clock.duration_from_nanoseconds(100'000'000);
    if (!timeout) return false;
    const auto begin = clock.now().ticks();
    // One-time bootstrap before scheduling starts. Runtime hardware waits use
    // retained IOSpace work and never spin here while a worker owns the device.
    for (;;) {
        const auto status = iommu_.initialize();
        if (status == IoStatus::Failed) return false;
        if (status == IoStatus::Complete) break;
        if (clock.now().ticks() - begin >= timeout->ticks()) return false;
    }
    for (auto& function : discovery_) {
        auto pending = objects.create_device(libk::move(function), iommu_, clock);
        if (!pending) return false;
        auto device = libk::move(pending).value().publish();
        kernel::diag::console::print<"io: isolated PCI function ready requester={:#x}\n">(
            device->requester());
        if (!devices_.try_push_back(libk::move(device))) return false;
    }
    discovery_.clear();
    return kernel::irq::register_kernel_source(virt_iommu_fault_irq, this, fault_irq);
}

auto IoPlatform::fault_irq(void* context) noexcept -> bool {
    auto& platform = *static_cast<IoPlatform*>(context);
    const bool healthy = platform.iommu_.handle_fault_irq() == IoStatus::Complete;
    const bool global = !healthy || platform.iommu_.fault_overflow();
    for (auto& device : platform.devices_) {
        if (global || platform.iommu_.fault_pending(device->requester()))
            device->signal_fault();
    }
    return healthy;
}

} // namespace arch
