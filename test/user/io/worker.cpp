#include <util.hpp>
#include <start.hpp>
#include <ipc.hpp>
#include <servers/virtio_blk.hpp>

int main() {

    const auto& info = rt_startup();
    const auto events = info.events;
    blk_dev device;
    if (const auto rc = device.open(info.pool, info.vspace,
                                    info.selector(io_host), events, rt_event_badge);
        rc != STATUS_OK)
        sys::exit(rc);
    if (device.capacity() != 1024 * 1024)
        sys::exit(STATUS_BAD_ARGS);
    constexpr size_t Batches = 16;
    uint64_t next_id{};
    for (size_t batch = 0; batch < Batches; ++batch) {
        uint64_t expected[blk_dev::Depth]{};
        for (size_t slot = 0; slot < blk_dev::Depth; ++slot) {
            expected[slot] = ++next_id;
            if (const auto rc = device.submit({slot, next_id}, io_operation::Read,
                                              slot * blk_dev::MaxTransfer, blk_dev::MaxTransfer);
                rc != STATUS_OK)
                sys::exit(rc);
        }
        if (device.active() != blk_dev::Depth)
            sys::exit(STATUS_INTERNAL);
        device.publish();
        while (device.active() != 0) {
            // Clear/unmask before the final queue observation: clearing ISR
            // after an empty observation could consume a newer completion.
            if (const auto rc = device.acknowledge(); rc != STATUS_OK)
                sys::exit(rc);
            blk_dev::Completion completion{};
            for (;;) {
                const auto status = device.take(completion);
                if (status == STATUS_WOULD_BLOCK)
                    break;
                if (status != STATUS_OK) sys::exit(status);
                if (completion.status != STATUS_OK) sys::exit(completion.status);
                if (completion.ticket.slot >= blk_dev::Depth ||
                    completion.ticket.id != expected[completion.ticket.slot] ||
                    completion.size != blk_dev::MaxTransfer)
                    sys::exit(STATUS_INTERNAL);
                expected[completion.ticket.slot] = 0;
                for (size_t index = 0; index < completion.size; ++index)
                    if (completion.data[index] != 0)
                        sys::exit(STATUS_BACKING_FAILED);
            }
            if (device.active() != 0)
                if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
                    sys::exit(rc);
        }
    }
    // Leave live BAR CPU mappings and a full submitted batch to Task teardown.
    // The next worker may acquire the device only after the kernel drains it.
    for (size_t slot = 0; slot < blk_dev::Depth; ++slot)
        if (const auto rc = device.submit({slot, ++next_id}, io_operation::Read,
                                          slot * blk_dev::MaxTransfer, blk_dev::MaxTransfer);
            rc != STATUS_OK)
            sys::exit(rc);
    device.publish();
    sys::exit();
}
