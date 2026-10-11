#include <string_view>
#include <string.h>
#include <libk/inplace_ring.hpp>
#include <sys/handle.hpp>
#include <optional>
#include <bit>
#include <utility>
#include <sys/protocol.hpp>
#include <sys/channel.hpp>
#include <sys/start.hpp>
#include <sys/task.hpp>

namespace {
using namespace sys;
constexpr size_t Capacity = 4;
constexpr size_t PackageLimit = 4 * 1024 * 1024;
using Supervisor = proc::tasks<Capacity>;
struct Waiter final { sys::Message reply; uint64_t deadline{}; };
struct Job final {
    fs::FileMemory package;
    proc::package program;
    cap_owner output;
    word_t writable{};
    uint64_t sequence{};
    std::optional<status_t> end;
    auto finish_output() noexcept -> status_t {
        if (!end) return STATUS_OK;
        const auto sent = sys::send(output.selector(), {
            .operation = static_cast<uint64_t>(sys::stream::Frame::End), .status = *end}, false);
        if (sent.status == STATUS_WOULD_BLOCK) {
            const auto armed = sys::channel_arm(output.selector(), writable, sequence);
            if (armed.status == STATUS_OK) sequence = armed.value;
            return armed.status == STATUS_OK ? STATUS_WOULD_BLOCK : armed.status;
        }
        if (sent.status != STATUS_OK && sent.status != STATUS_CLOSED
            && sent.status != STATUS_PEER_CLOSED) return sent.status;
        return close_output();
    }
    auto close_output() noexcept -> status_t {
        if (!output) return STATUS_OK;
        const auto s = sys::channel_unbind(output.selector(), writable).status;
        if (s != STATUS_OK && s != STATUS_CLOSED && s != STATUS_PEER_CLOSED) return s;
        const auto closed = output.close();
        if (closed == STATUS_OK) end.reset();
        return closed;
    }
    bool discard{};
    std::optional<proc::task_id> child;
    std::optional<Waiter> waiter;

    void release_image() noexcept {
        sys::require(program.close());
        package = {};
    }
};
Job jobs[Capacity];
Supervisor supervisor;
io::ClientSession filesystem;
cap_owner file_events;
// Replies retain their own bytes while the peer is backpressured. Admission
// stops at this bound; paging and teardown continue independently.
libk::inplace_ring<sys::Message, 16> replies;

// Mounted media is immutable. Package names and manifests never grant rights.
auto package_name(const char* name, size_t size, char (&path)[12]) noexcept -> bool {
    if (size == 0 || size > 8) return false;
    for (size_t i = 0; i < size; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        path[i] = c;
    }
    ::memcpy(path + size, ".PKG", 4);
    return true;
}
auto find(uint64_t id) noexcept -> Job* {
    for (auto& job : jobs) if (job.child && job.child->key() == id) return &job;
    return nullptr;
}
auto spawn(const boot::BootView& info, const sys::Message& request,
           std::span<cap_owner> stdio) noexcept -> sys::Message {
    sys::Message reply{.operation = request.operation, .status = STATUS_BUSY};
    Job* job{};
    for (auto& candidate : jobs) if (!candidate.child && !candidate.output) { job = &candidate; break; }
    if (job == nullptr) return reply;
    char path[12]{};
    boot::args arguments;
    reply.status = STATUS_BAD_ARGS;
    if (!arguments.decode(request.data, request.size) || arguments.count() == 0) return reply;
    const auto* name = arguments.argument(0);
    const auto length = ::strlen(name);
    if (!package_name(name, length, path)) { reply.status = STATUS_DENIED; return reply; }
    fs::File file;
    auto status = fs::open(filesystem, path, length + 4, file);
    if (status != STATUS_OK) { reply.status = status; return reply; }
    if (file.size == 0 || file.size > PackageLimit) status = STATUS_BAD_ARGS;
    if (status == STATUS_OK) {
        auto backing = fs::backing(filesystem, file, VM_READ | VM_EXECUTE);
        if (backing) job->package = std::move(*backing);
        else status = backing.error();
    }
    const auto closed = fs::close(filesystem, file);
    if (status == STATUS_OK) status = closed;
    const auto address = word_t{0x10000000} + (job - jobs) * 0x1000000;
    if (status == STATUS_OK) {
        status = job->program.open(info.selector(BOOT_VSPACE),
            job->package.memory.selector(), {address, proc::window::round_size(job->package.size)}, job->package.size, VM_READ | VM_EXECUTE | VM_MAP_PRIVATE);
    }
    if ((request.id & ~uint64_t{15}) || stdio.size() != size_t(std::popcount(request.id & 7))
        || ((request.id & proc::end_stdout) && !(request.id & 2))) status = STATUS_BAD_ARGS;
    std::array<proc::cap_src, 6> caps{};
    size_t count{}, imported{};
    const auto binding = [](boot::Import contract, cap_t cap, word_t rights) noexcept {
        return proc::cap_src{.contract = contract, .cap = cap, .limit = rights};
    };
    for (size_t i = 0; i < 3 && status == STATUS_OK; ++i) if (request.id & (1u << i)) {
        auto& source = stdio[imported++];
        caps[count++] = binding(i == 0 ? boot::Stdin : i == 1 ? boot::Stdout : boot::Stderr,
            source.selector(), i == 0 ? RIGHT_RECEIVE : RIGHT_SEND);
        if (i == 1 && (request.id & proc::end_stdout)) {
            const auto bound = sys::channel_bind(source.selector(), info.selector(BOOT_EVENTS), CHANNEL_WRITABLE);
            status = bound.status;
            if (status == STATUS_OK) { job->output = std::move(source); job->writable = bound.value; job->sequence = 0; }
        }
    }
    for (const auto contract : {boot::Vfs, boot::VfsRead})
        caps[count++] = binding(contract, info.selector(contract), RIGHT_SEND);
    // Formatting remains an explicit privilege of this immutable boot-disk tool.
    if (std::string_view{name, length} == "mkfs" && info.selector(boot::StoreAdmin))
        caps[count++] = binding(boot::StoreAdmin, info.selector(boot::StoreAdmin), RIGHT_SEND);
    if (status == STATUS_OK) {
        const auto spawned = supervisor.spawn(job->program, {name, length}, {
            .args = &arguments, .caps = std::span{caps}.first(count),
            .resources = {.bytes = 4 * 1024 * 1024, .caps = 128, .kinds = proc::base_kinds | OBJ_BIT(ObjChannel)},
            .scratch = address + 0x800000,
            .events = sys::capability(info, BOOT_EVENTS),
            .close_badge = uint64_t{1} << (8 + job - jobs)});
        status = spawned.status;
        if (spawned.id.valid()) job->child = spawned.id;
    }
    reply.status = status;
    if (job->child) {
        // Failed construction still owns teardown; no process handle was published.
        job->discard = status != STATUS_OK;
        reply.id = job->child->key();
    } else { sys::require(job->close_output()); job->release_image(); }
    return reply;
}

}

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    const auto info = sys::bootstrap(address, size);
    const auto events = sys::capability(info, BOOT_EVENTS);
    supervisor.open(info);
    const auto file_notification = notification_create(sys::capability(info, BOOT_POOL), 1);
    sys::require(file_notification.status);
    file_events = cap_owner{file_notification.value};
    uint64_t value{};
    sys::require(filesystem.connect(sys::capability(info, boot::Files),
        sys::capability(info, BOOT_POOL), sys::capability(info, BOOT_CSPACE),
        file_events.selector(), sys::capability(info, BOOT_VSPACE), 0x70000000, value));
    sys::Connection channel{sys::capability(info, boot::Process), events};
    sys::require(channel.enable_writable());
    for (;;) {
        bool closing = false;
        uint64_t deadline{};
        const auto now = clock_now();
        sys::require(now.status);
        for (auto& job : jobs) {
            const auto flushed = job.finish_output();
            if (flushed != STATUS_WOULD_BLOCK) sys::require(flushed);
            if (!job.child) continue;
            const auto status = supervisor.poll(*job.child);
            if (status == STATUS_OK) {
                // Ready proves the task pool has drained; package mappings can now close.
                const auto result = supervisor.result(*job.child);
                if (!result) exit(STATUS_INTERNAL);
                // Stream completion outlives the task result when End is
                // backpressured. Stop/Wait must still be able to reap the task.
                if (job.discard) sys::require(job.close_output());
                else if (job.output && !job.end) {
                    job.end = result->status;
                    const auto flushed = job.finish_output();
                    if (flushed != STATUS_WOULD_BLOCK) sys::require(flushed);
                }
                job.release_image();
                if (job.discard) {
                    sys::require(supervisor.collect(*job.child).status);
                    job.child.reset(); job.discard = false;
                    continue;
                }
                if (job.waiter && !replies.full()) {
                    const auto result = supervisor.collect(*job.child);
                    sys::require(result.status);
                    job.waiter->reply.status = static_cast<status_t>(result.value);
                    replies.emplace_back(job.waiter->reply);
                    job.waiter.reset();
                    job.child.reset();
                }
            } else if (!proc::retryable(status) && status != STATUS_WOULD_BLOCK) {
                exit(status);
            }
            if (job.waiter && job.waiter->deadline != 0) {
                if (job.waiter->deadline <= now.value && !replies.full()) {
                    job.waiter->reply.status = STATUS_TIMED_OUT;
                    replies.emplace_back(job.waiter->reply);
                    job.waiter.reset();
                } else if (deadline == 0 || job.waiter->deadline < deadline) {
                    deadline = job.waiter->deadline;
                }
            }
            closing |= job.child && supervisor.closing_needs_poll(*job.child);
        }
        while (!replies.empty()) {
            const auto sent = channel.try_send(replies.front());
            if (sent.status == STATUS_WOULD_BLOCK) break;
            sys::require(sent.status);
            replies.pop_front();
        }
        if (replies.capacity() - replies.size() >= 2) {
            sys::Message request{};
            std::array<cap_owner, 3> stdio;
            size_t imported{};
            const auto received = channel.try_receive(request, stdio, imported);
            if (received.status == STATUS_OK) {
                sys::Message reply{.operation = request.operation, .id = request.id, .status = STATUS_BAD_ARGS};
                switch (static_cast<proc::op>(request.operation)) {
                case proc::op::Spawn:
                    reply = spawn(info, request, std::span{stdio}.first(imported));
                    break;
                case proc::op::CancelWait: {
                    auto* job = find(request.id);
                    if (job == nullptr || !job->waiter) { reply.status = STATUS_INVALID_CAP; break; }
                    job->waiter->reply.status = STATUS_CANCELED;
                    replies.emplace_back(job->waiter->reply);
                    job->waiter.reset();
                    reply.status = STATUS_OK;
                    break;
                }
                case proc::op::Wait:
                case proc::op::Stop: {
                    auto* job = find(request.id);
                    if (job == nullptr) { reply.status = STATUS_INVALID_CAP; break; }
                    const bool stop = request.operation == static_cast<uint64_t>(proc::op::Stop);
                    if (stop) sys::require(supervisor.request_stop(*job->child));
                    if (job->waiter) { reply.status = stop ? STATUS_OK : STATUS_BUSY; break; }
                    uint64_t expires{};
                    if (!stop && request.size != 0) {
                        if (request.size != sizeof(expires)) break;
                        ::memcpy(&expires, request.data, sizeof(expires));
                    }
                    job->waiter = Waiter{reply, expires};
                    continue;
                }
                }
                if (reply.status != STATUS_OK) reply.id = 0;
                replies.emplace_back(reply);
                continue;
            }
            if (received.status != STATUS_WOULD_BLOCK) exit(received.status);
            sys::require(channel.arm().status);
        }
        if (!replies.empty()) sys::require(channel.arm_writable().status);
        if (closing) yield();
        else {
            const auto wake = notification_wait(events, deadline);
            if (wake.status != STATUS_OK && wake.status != STATUS_TIMED_OUT) exit(wake.status);
            if (wake.status == STATUS_OK)
                for (auto& job : jobs) if (job.child) supervisor.notify(*job.child, wake.value);
        }
    }
}
