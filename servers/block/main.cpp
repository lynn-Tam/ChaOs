#include <servers/block/device.hpp>
#include <user/lib/io_session.hpp>

namespace {
using namespace myos;
block::Device device;
io::ServerSession session;

auto validate(const io::Request& request) noexcept -> myos_status_t {
    if (request.operation != static_cast<uint64_t>(io::Operation::Read)
        || request.object != 0 || request.buffer != 0 || request.flags != 0
        || request.length == 0 || request.length > io::BufferSize
        || request.buffer_offset > io::PayloadSize
        || request.length > io::PayloadSize - request.buffer_offset)
        return MYOS_STATUS_BAD_ARGS;
    return MYOS_STATUS_OK;
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto pool = service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const auto vspace = service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    const auto cspace = service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE);
    const auto events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    service::require(device.open(pool, vspace, service::capability(info, MYOS_BOOTSTRAP_CAP_DEVICE), events));
    service::require(session.open(service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL),
        events, pool, vspace, cspace, 0x70000000, device.capacity()));
    bool hardware_closed{};

    for (;;) {
        // Complete the interrupt handshake before the final ring drain.
        if (!hardware_closed) service::require(device.acknowledge());
        service::require(session.poll([](const io::ControlMessage&, io::ControlMessage& reply) {
            reply.status = MYOS_STATUS_INVALID_OP;
        }));
        auto* queue = session.queue();

        for (size_t count = 0; device.active() != 0 && count < io::QueueDepth; ++count) {
            block::Device::Completion completion;
            const auto status = device.take(completion);
            if (status == MYOS_STATUS_WOULD_BLOCK) break;
            service::require(status);
            const auto* request = queue->request(completion.ticket);
            if (request == nullptr) exit(MYOS_STATUS_INTERNAL);
            const bool cancelled = queue->cancelled(completion.ticket);
            if (!cancelled && completion.status == MYOS_STATUS_OK)
                service::copy(session.payload() + request->buffer_offset, completion.data, completion.size);
            if (!queue->finish(completion.ticket, cancelled ? MYOS_STATUS_CANCELED : completion.status,
                cancelled ? 0 : completion.size)) exit(MYOS_STATUS_PEER_FAULT);
        }
        if (queue && !session.closing()) {
            for (size_t count = 0; count < io::QueueDepth; ++count) {
                io::Ticket ticket{};
                const auto admitted = queue->admit(ticket);
                if (admitted == io::Admission::Empty || admitted == io::Admission::Backpressure) break;
                if (admitted != io::Admission::Ready) exit(MYOS_STATUS_PEER_FAULT);
                const auto& request = *queue->request(ticket);
                auto status = validate(request);
                if (status == MYOS_STATUS_OK) status = device.submit(ticket, request.offset, request.length);
                if (status != MYOS_STATUS_OK && !queue->finish(ticket, status, 0)) exit(MYOS_STATUS_PEER_FAULT);
            }
            device.publish();
        }
        if (session.closing() && queue->active() == 0 && !hardware_closed) {
            service::require(device.close());
            hardware_closed = true;
        }
        service::require(session.flush(hardware_closed));
        service::require(session.arm());
        service::require(notification_wait(events).status);
    }
}
