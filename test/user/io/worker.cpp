#include <sys/start.hpp>
#include <sys/channel.hpp>
#include <servers/virtio_blk.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    const auto events = sys::capability(info, BOOT_EVENTS);
    block::Device device;
    sys::require(device.open(sys::capability(info, BOOT_POOL),
        sys::capability(info, BOOT_VSPACE),
        sys::capability(info, BOOT_IO_HOST), events, boot::event_badge));
    if (device.capacity() != 1024 * 1024) exit(STATUS_BAD_ARGS);
    constexpr size_t Batches = 16;
    uint64_t next_id{};
    for (size_t batch = 0; batch < Batches; ++batch) {
        uint64_t expected[block::Device::Depth]{};
        for (size_t slot = 0; slot < block::Device::Depth; ++slot) {
            expected[slot] = ++next_id;
            sys::require(device.submit({slot, next_id}, io::Operation::Read,
                slot * block::Device::MaxTransfer, block::Device::MaxTransfer));
        }
        if (device.active() != block::Device::Depth) exit(STATUS_INTERNAL);
        device.publish();
        while (device.active() != 0) {
            // Clear/unmask before the final queue observation: clearing ISR
            // after an empty observation could consume a newer completion.
            sys::require(device.acknowledge());
            block::Device::Completion completion{};
            for (;;) {
                const auto status = device.take(completion);
                if (status == STATUS_WOULD_BLOCK) break;
                sys::require(status);
                sys::require(completion.status);
                if (completion.ticket.slot >= block::Device::Depth
                    || completion.ticket.id != expected[completion.ticket.slot]
                    || completion.size != block::Device::MaxTransfer) exit(STATUS_INTERNAL);
                expected[completion.ticket.slot] = 0;
                for (size_t index = 0; index < completion.size; ++index)
                    if (completion.data[index] != 0) exit(STATUS_BACKING_FAILED);
            }
            if (device.active() != 0) sys::require(notification_wait(events).status);
        }
    }
    // Leave live BAR CPU mappings and a full submitted batch to Task teardown.
    // The next worker may acquire the device only after the kernel drains it.
    for (size_t slot = 0; slot < block::Device::Depth; ++slot)
        sys::require(device.submit({slot, ++next_id}, io::Operation::Read,
            slot * block::Device::MaxTransfer, block::Device::MaxTransfer));
    device.publish();
    exit();
}
