#include <io/platform.hpp>

#include <diag/console.hpp>
#include <libk/utility.hpp>
#include <object/object_store.hpp>

namespace kernel::io {

auto Platform::initialize(const boot::BootInfo& boot, mm::Pmm& pmm,
    const time::Clock& clock, object::ObjectStore& objects) noexcept -> bool {
    if (!boot.iommu) return true;
    if (boot.iommu->first().base().raw() != arch::virt_iommu_base
        || boot.iommu->page_count() != 1) return false;
    auto discovered = arch::PciFunction::discover_block();
    if (!discovered) return discovered.error() == arch::PciError::Absent;
    auto device = libk::move(discovered).value();
    if (!iommu_.start(pmm, device.requester())) return false;
    const auto timeout = clock.duration_from_nanoseconds(100'000'000);
    if (!timeout) return false;
    const auto begin = clock.now().ticks();
    // One-time bootstrap before scheduling starts. Runtime hardware waits use
    // retained IOSpace work and never spin here while a worker owns the device.
    for (;;) {
        const auto status = iommu_.initialize();
        if (status == arch::IoStatus::Failed) return false;
        if (status == arch::IoStatus::Complete) break;
        if (clock.now().ticks() - begin >= timeout->ticks()) return false;
    }
    auto pending = objects.create_device(libk::move(device), iommu_, clock);
    if (!pending) return false;
    device_ = libk::move(pending).value().publish();
    diag::console::print<"io: isolated PCI function ready requester={:#x}\n">(
        device_->requester());
    return true;
}

} // namespace kernel::io
