#include <servers/block/device.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    block::Device device;
    service::require(device.open(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
        service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE), events));
    if (device.capacity() != 1024 * 1024) exit(MYOS_STATUS_BAD_ARGS);
    constexpr size_t Batches = 16;
    uint64_t next_id{};
    for (size_t batch = 0; batch < Batches; ++batch) {
        uint64_t expected[block::Device::Depth]{};
        for (size_t slot = 0; slot < block::Device::Depth; ++slot) {
            expected[slot] = ++next_id;
            service::require(device.submit({slot, next_id}, slot * block::Device::MaxRead,
                block::Device::MaxRead));
        }
        if (device.active() != block::Device::Depth) exit(MYOS_STATUS_INTERNAL);
        device.publish();
        while (device.active() != 0) {
            // Clear/unmask before the final queue observation: clearing ISR
            // after an empty observation could consume a newer completion.
            service::require(device.acknowledge());
            block::Device::Completion completion{};
            for (;;) {
                const auto status = device.take(completion);
                if (status == MYOS_STATUS_WOULD_BLOCK) break;
                service::require(status);
                service::require(completion.status);
                if (completion.ticket.slot >= block::Device::Depth
                    || completion.ticket.id != expected[completion.ticket.slot]
                    || completion.size != block::Device::MaxRead) exit(MYOS_STATUS_INTERNAL);
                expected[completion.ticket.slot] = 0;
                for (size_t index = 0; index < completion.size; ++index)
                    if (completion.data[index] != 0) exit(MYOS_STATUS_BACKING_FAILED);
            }
            if (device.active() != 0) service::require(notification_wait(events).status);
        }
    }
    // Leave live BAR CPU mappings and a full submitted batch to Task teardown.
    // The next worker may acquire the device only after the kernel drains it.
    for (size_t slot = 0; slot < block::Device::Depth; ++slot)
        service::require(device.submit({slot, ++next_id}, slot * block::Device::MaxRead,
            block::Device::MaxRead));
    device.publish();
    exit();
}
