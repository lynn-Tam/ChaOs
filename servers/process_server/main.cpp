#include <user/lib/supervisor.hpp>
#include <user/lib/file_client.hpp>

namespace {
myos::deploy::Supervisor<1> supervisor;
myos::files::Client filesystem;
myos::MappedMemory snapshot;
constexpr size_t PackageLimit = 4 * 1024 * 1024;
libk::optional<myos::deploy::Supervisor<1>::Handle> child;
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    service::require(filesystem.connect(info));
    auto memory = MappedMemory::create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
        service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE), 0x60000000, PackageLimit);
    if (!memory) exit(memory.error());
    snapshot = libk::move(memory).value();
    service::require(supervisor.add("domain", service::capability(info, MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN),
        MYOS_OBJECT_KIND_SCHED_DOMAIN, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONTROL));
    service::require(supervisor.add("console.sender",
        service::capability(info, MYOS_BOOTSTRAP_CAP_CONSOLE_OUTPUT),
        MYOS_OBJECT_KIND_CHANNEL, MYOS_RIGHT_SEND | MYOS_RIGHT_DUPLICATE,
        0, 1, UINT64_MAX));
    service::Connection channel{
        service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL),
        service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION)};
    for (;;) {
        service::Message request{};
        service::require(channel.receive(request).status);
        service::Message reply{};
        reply.operation = request.operation;
        reply.id = request.id;
        reply.status = MYOS_STATUS_BAD_ARGS;
        switch (static_cast<service::Process>(request.operation)) {
        case service::Process::Spawn: {
            if (child) { reply.status = MYOS_STATUS_BUSY; break; }
            // Application names select disk packages under service policy.
            // A package can only bind the domain and console ceilings above.
            const deploy::ByteView application{
                reinterpret_cast<const uint8_t*>(request.data), request.size};
            if (!application.equals(deploy::Supervisor<1>::name("hello"))) {
                reply.status = MYOS_STATUS_DENIED;
                break;
            }
            myos_status_t status = supervisor.unload();
            if (status != MYOS_STATUS_OK) { reply.status = status; break; }
            files::File package;
            status = filesystem.open("HELLO.PKG", 9, package);
            if (status != MYOS_STATUS_OK) { reply.status = status; break; }
            if (package.size == 0 || package.size > PackageLimit) status = MYOS_STATUS_BAD_ARGS;
            if (status == MYOS_STATUS_OK) {
                const auto writable = vm_protect(snapshot.region.selector(), snapshot.address,
                    snapshot.size, MYOS_VM_READ | MYOS_VM_WRITE).status;
                if (!deploy::committed(writable)) status = writable;
            }
            if (status == MYOS_STATUS_OK)
                status = filesystem.read(package, [](uint64_t offset, const uint8_t* data, size_t bytes) {
                    service::copy(reinterpret_cast<uint8_t*>(snapshot.address) + offset, data, bytes);
                });
            const auto closed = filesystem.close(package);
            if (status == MYOS_STATUS_OK) status = closed;
            if (status == MYOS_STATUS_OK) {
                const auto readonly = vm_protect(snapshot.region.selector(), snapshot.address,
                    snapshot.size, MYOS_VM_READ).status;
                if (!deploy::committed(readonly)) status = readonly;
            }
            if (status == MYOS_STATUS_OK) status = supervisor.open(info, snapshot.memory.selector(), package.size);
            if (status == MYOS_STATUS_OK) child = supervisor.launch("hello", status);
            reply.status = status;
            if (child) reply.id = child->token();
            break;
        }
        case service::Process::Wait:
        case service::Process::Stop:
            if (!child || child->token() != request.id) {
                reply.status = MYOS_STATUS_INVALID_CAP;
                break;
            }
            reply.status = request.operation == static_cast<uint64_t>(service::Process::Wait)
                ? supervisor.wait(*child) : supervisor.stop(*child);
            child.reset();
            break;
        }
        service::require(channel.send(reply).status);
    }
}
