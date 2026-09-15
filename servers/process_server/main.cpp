#include <servers/process_server/image.hpp>
#include <servers/process_server/policy.hpp>
#include <user/lib/imports.hpp>
#include <user/lib/supervisor.hpp>
#include <user/lib/file_client.hpp>

namespace {
using namespace myos;
constexpr size_t Capacity = 4;
constexpr size_t PackageLimit = 4 * 1024 * 1024;
using Supervisor = deploy::Supervisor<Capacity>;
struct Waiter final { service::Message reply; uint64_t deadline{}; };
struct Job final {
    files::FileMemory package;
    deploy::Program program;
    process::Image image;
    libk::optional<Supervisor::Handle> child;
    libk::optional<Waiter> waiter;

    void release_image() noexcept {
        image.close();
        service::require(program.close());
        package = {};
    }
};
Job jobs[Capacity];
Supervisor supervisor;
files::Client filesystem;
process::PageBuffer page_buffer;
cap::OwnedCap file_events;

// Replies retain their own bytes while the peer is backpressured. Admission
// stops at this bound; paging and teardown continue independently.
class Replies final {
    service::Message messages_[16]{};
    size_t first_{}, size_{};
public:
    auto available() const noexcept -> size_t { return 16 - size_; }
    auto full() const noexcept -> bool { return available() == 0; }
    auto empty() const noexcept -> bool { return size_ == 0; }
    void push(service::Message message) noexcept {
        if (full()) exit(MYOS_STATUS_INTERNAL);
        messages_[(first_ + size_++) % 16] = message;
    }
    auto flush(service::Connection& channel) noexcept -> myos_status_t {
        while (!empty()) {
            const auto sent = channel.try_send(messages_[first_]);
            if (sent.status != MYOS_STATUS_OK) return sent.status;
            first_ = (first_ + 1) % 16;
            --size_;
        }
        return MYOS_STATUS_OK;
    }
} replies;

// Mounted media is immutable. Package names and manifests never grant rights.
auto package_name(const char* name, size_t size, char (&path)[12]) noexcept -> bool {
    if (size == 0 || size > 8) return false;
    for (size_t i = 0; i < size; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        path[i] = c;
    }
    service::copy(path + size, ".PKG", 4);
    return true;
}
auto find(uint64_t token) noexcept -> Job* {
    for (auto& job : jobs) if (job.child && job.child->token() == token) return &job;
    return nullptr;
}
auto spawn(const bootstrap::BootstrapView& info, const service::Message& request) noexcept -> service::Message {
    service::Message reply{.operation = request.operation, .status = MYOS_STATUS_BUSY};
    Job* job{};
    for (auto& candidate : jobs) if (!candidate.child) { job = &candidate; break; }
    if (job == nullptr) return reply;
    char path[12]{};
    bootstrap::Arguments arguments;
    reply.status = MYOS_STATUS_BAD_ARGS;
    if (!arguments.decode(request.data, request.size) || arguments.count() == 0) return reply;
    const auto* name = arguments.argument(0);
    const auto length = service::length(name);
    if (!package_name(name, length, path)) { reply.status = MYOS_STATUS_DENIED; return reply; }
    files::File file;
    auto status = filesystem.open(path, length + 4, file);
    if (status != MYOS_STATUS_OK) { reply.status = status; return reply; }
    if (file.size == 0 || file.size > PackageLimit) status = MYOS_STATUS_BAD_ARGS;
    if (status == MYOS_STATUS_OK) {
        auto backing = filesystem.backing(file, MYOS_VM_READ | MYOS_VM_EXECUTE);
        if (backing) job->package = libk::move(backing).value();
        else status = backing.error();
    }
    const auto closed = filesystem.close(file);
    if (status == MYOS_STATUS_OK) status = closed;
    const auto address = 0x10000000 + (job - jobs) * 0x1000000;
    if (status == MYOS_STATUS_OK) status = supervisor.load(job->program, info,
        job->package.memory.selector(), job->package.size, address, address + 0x800000);
    if (status == MYOS_STATUS_OK) job->child = supervisor.launch(job->program,
        {reinterpret_cast<const uint8_t*>(name), length}, status,
        {.image_source = job->image.source(page_buffer, job->package.memory.selector(), address, job->package.size),
         .arguments = &arguments,
         .terminal_events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION),
         .close_badge = uint64_t{1} << (8 + job - jobs), .admit = process::admit});
    reply.status = status;
    if (job->child) reply.id = job->child->token();
    else job->release_image();
    return reply;
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = service::bootstrap(address, size);
    const auto events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    supervisor.open(info);
    const auto file_notification = notification_create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL), 1);
    service::require(file_notification.status);
    file_events = cap::OwnedCap{{file_notification.value, 0}};
    service::require(filesystem.connect(info, 0x70000000, file_events.selector()));
    service::require(page_buffer.open(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
        service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE), events));
    service::require(supervisor.add("domain", service::capability(info, MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN),
        MYOS_OBJECT_KIND_SCHED_DOMAIN, MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_CONTROL));
    service::require(supervisor.add("console.sender", service::capability(info, bootstrap::imports::ConsoleOutput),
        MYOS_OBJECT_KIND_CHANNEL, MYOS_RIGHT_SEND | MYOS_RIGHT_DUPLICATE, 0, 1, UINT64_MAX));
    service::require(supervisor.add("files.directory", service::capability(info, bootstrap::imports::FilesRead),
        MYOS_OBJECT_KIND_CHANNEL, MYOS_RIGHT_SEND | MYOS_RIGHT_DUPLICATE, 0, 1, UINT64_MAX));
    service::Connection channel{service::capability(info, bootstrap::imports::Process), events};
    service::require(channel.enable_writable());
    for (;;) {
        bool closing = false;
        uint64_t deadline{};
        const auto now = clock_now();
        service::require(now.status);
        for (auto& job : jobs) {
            if (!job.child) continue;
            service::require(job.image.poll());
            const auto status = supervisor.poll(*job.child);
            if (status == MYOS_STATUS_OK) {
                // Ready proves TaskTable has released both the pool and plan.
                job.release_image();
                if (job.waiter && !replies.full()) {
                    const auto result = supervisor.collect(*job.child);
                    service::require(result.status);
                    job.waiter->reply.status = static_cast<myos_status_t>(result.value);
                    replies.push(job.waiter->reply);
                    job.waiter.reset();
                    job.child.reset();
                }
            } else if (!deploy::retryable(status) && status != MYOS_STATUS_WOULD_BLOCK) {
                exit(status);
            }
            if (job.waiter && job.waiter->deadline != 0) {
                if (job.waiter->deadline <= now.value && !replies.full()) {
                    job.waiter->reply.status = MYOS_STATUS_TIMED_OUT;
                    replies.push(job.waiter->reply);
                    job.waiter.reset();
                } else if (deadline == 0 || job.waiter->deadline < deadline) {
                    deadline = job.waiter->deadline;
                }
            }
            closing |= job.child && supervisor.closing_needs_poll(*job.child);
        }
        const auto sent = replies.flush(channel);
        if (sent != MYOS_STATUS_OK && sent != MYOS_STATUS_WOULD_BLOCK) exit(sent);
        if (replies.available() >= 2) {
            service::Message request{};
            const auto received = channel.try_receive(request);
            if (received.status == MYOS_STATUS_OK) {
                service::Message reply{.operation = request.operation, .id = request.id, .status = MYOS_STATUS_BAD_ARGS};
                switch (static_cast<service::Process>(request.operation)) {
                case service::Process::Spawn:
                    reply = spawn(info, request);
                    break;
                case service::Process::CancelWait: {
                    auto* job = find(request.id);
                    if (job == nullptr || !job->waiter) { reply.status = MYOS_STATUS_INVALID_CAP; break; }
                    job->waiter->reply.status = MYOS_STATUS_CANCELED;
                    replies.push(job->waiter->reply);
                    job->waiter.reset();
                    reply.status = MYOS_STATUS_OK;
                    break;
                }
                case service::Process::Wait:
                case service::Process::Stop: {
                    auto* job = find(request.id);
                    if (job == nullptr) { reply.status = MYOS_STATUS_INVALID_CAP; break; }
                    const bool stop = request.operation == static_cast<uint64_t>(service::Process::Stop);
                    if (stop) service::require(supervisor.request_stop(*job->child));
                    if (job->waiter) { reply.status = stop ? MYOS_STATUS_OK : MYOS_STATUS_BUSY; break; }
                    uint64_t expires{};
                    if (!stop && request.size != 0) {
                        if (request.size != sizeof(expires)) break;
                        service::copy(&expires, request.data, sizeof(expires));
                    }
                    job->waiter = Waiter{reply, expires};
                    continue;
                }
                }
                replies.push(reply);
                continue;
            }
            if (received.status != MYOS_STATUS_WOULD_BLOCK) exit(received.status);
            service::require(channel.arm().status);
        }
        if (!replies.empty()) service::require(channel.arm_writable().status);
        if (closing) yield();
        else {
            const auto wake = notification_wait(events, deadline);
            if (wake.status != MYOS_STATUS_OK && wake.status != MYOS_STATUS_TIMED_OUT) exit(wake.status);
            if (wake.status == MYOS_STATUS_OK)
                for (auto& job : jobs) if (job.child) supervisor.notify(*job.child, wake.value);
        }
    }
}
