#include <thread.hpp>
#include <proc.hpp>
#include <posix.hpp>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <memory>
#include <string.h>
#include <string_view>
#include <sys/syscall.hpp>
#include <source_location>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

static constinit rt_mutex mutex;
static constinit rt_cond cond;
static unsigned ready;
static bool go;
static uint64_t count;
static std::atomic<unsigned> destroyed, constructed, once_count, cleaned, keyed;
static thread_local uint64_t local{42};
alignas(128) static thread_local char aligned[72 * 1024];
struct thread_value { ~thread_value() { destroyed.fetch_add(1, std::memory_order_relaxed); } };
static void check(bool value, std::source_location source = std::source_location::current()) {
    if (!value) {
        const auto message = "[threads] failed at line " + std::to_string(source.line()) + "\n";
        (void)write(2, message.data(), message.size()); sys::exit(STATUS_INTERNAL);
    }
}
static void say(const char* text) { check(write(1, text, strlen(text)) == ssize_t(strlen(text))); }
static auto worker(void* argument) -> void* {
    thread_local thread_value value;
    (void)value;
    check(local == 42 && aligned[0] == 0 && aligned[sizeof(aligned) - 1] == 0 && uintptr_t(aligned) % 128 == 0);
    local = reinterpret_cast<uintptr_t>(argument); aligned[128] = char(local);
    {
        std::unique_lock lock{mutex}; ++ready; rt_cond_wake(&cond, true);
        while (!go) check(!rt_cond_wait(&cond, &mutex));
    }
    for (unsigned i = 0; i < 100; ++i) {
        std::vector<uint64_t> memory(32, local);
        check(memory.back() == local && aligned[128] == char(local));
        std::lock_guard lock{mutex}; ++count;
    }
    return argument;
}
static pthread_key_t key;
static int shared_fd;
static void increment(void* value) { static_cast<std::atomic<unsigned>*>(value)->fetch_add(1); }
static void key_cleanup(void* value) {
    char data[8]{}; check(pread(shared_fd, data, sizeof(data), 0) == sizeof(data) && !memcmp(data, "Disk fil", 8));
    increment(value);
}
static auto io_worker(void* argument) -> void* {
    const auto id = uintptr_t(argument);
    errno = int(100 + id);
    check(!pthread_setspecific(key, &keyed));
    pthread_cleanup_push(increment, &cleaned);
    for (unsigned i = 0; i < 4; ++i) {
        char buffer[32]{};
        check(pread(shared_fd, buffer, sizeof(buffer), 0) == sizeof(buffer));
        check(!memcmp(buffer, "Disk file service", 17));
        const int own = open("/boot/README.TXT", O_RDONLY); check(own >= 0);
        check(read(own, buffer, sizeof(buffer)) == sizeof(buffer)); check(!close(own));
        auto* dir = opendir("/boot"); check(dir && readdir(dir)); check(!closedir(dir));
        check(errno == int(100 + id));
    }
    char first[8]{}, second[8]{};
    io_request a{shared_fd, IO_READ, first, sizeof(first), 0}, b{shared_fd, IO_READ, second, sizeof(second), 8};
    request_id x{}, y{}; check(!px_submit(&a, &x) && !px_submit(&b, &y) && x != y);
    for (unsigned i = 0; i < 2; ++i) { io_completion done{}; check(px_completion(&done, IO_WAIT) == 1 && done.result == 8 && (done.id == x || done.id == y)); }
    check(!memcmp(first, "Disk fil", 8) && !memcmp(second, "e servic", 8));
    pthread_exit(argument);
    pthread_cleanup_pop(0);
    return nullptr;
}
static void standard_threads() {
    std::mutex gate;
    std::condition_variable wake;
    std::once_flag once;
    auto shared = std::make_shared<unsigned>(73);
    unsigned finished{};
    std::vector<std::jthread> threads;
    for (unsigned i = 0; i < 4; ++i) threads.emplace_back([&, shared](std::stop_token stop) {
        static const auto initialized = [] { ++constructed; return 91; }();
        check(initialized == 91);
        std::call_once(once, [] { ++once_count; });
        for (unsigned j = 0; j < 200; ++j) { auto copy = shared; check(*copy == 73 && !stop.stop_requested()); }
        std::unique_lock lock{gate}; ++finished;
        std::notify_all_at_thread_exit(wake, std::move(lock));
    });
    {
        std::unique_lock lock{gate};
        check(wake.wait_for(lock, std::chrono::seconds(5), [&] { return finished == 4; }));
    }
    for (auto& thread : threads) thread.join();
    check(constructed == 1 && once_count == 1 && shared.use_count() == 1);
    std::atomic<unsigned> state{};
    std::thread waiter([&] { state.wait(0); check(state == 1); });
    state.store(1); state.notify_all(); waiter.join();
    std::unique_lock lock{gate};
    check(wake.wait_for(lock, std::chrono::milliseconds(2)) == std::cv_status::timeout);
}
static void native_lifetime() {
    const auto self = rt_thread_join(rt_thread_self()); check(!self && self.error() == STATUS_DEADLOCK);
    const auto target = rt_thread_create([](void*) -> void* { return nullptr; }, nullptr); check(bool(target));
    std::atomic<bool> start{};
    status_t joined{}, detached{};
    std::thread a([&] { start.wait(false); const auto result = rt_thread_join(*target); joined = result ? STATUS_OK : result.error(); });
    std::thread b([&] { start.wait(false); detached = rt_thread_detach(*target); });
    start.store(true); start.notify_all(); a.join(); b.join();
    check((joined == STATUS_OK) != (detached == STATUS_OK));
    const auto no_op = [](void*) -> void* { return nullptr; };
    const auto reap_detached = [&] {
        auto made = rt_thread_create(no_op, nullptr, {.detached=true}); check(bool(made));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto status = rt_thread_detach(*made);
            if (status == STATUS_NOT_FOUND) break;
            check(status == STATUS_BAD_ARGS && std::chrono::steady_clock::now() < deadline); sys::yield();
        }
    };
    reap_detached(); check(sys::quota_info(rt_startup().quota).status == STATUS_OK);
    const auto before = *reinterpret_cast<const QuotaInfo*>(rt_current().ipc);
    for (unsigned i = 0; i < 12; ++i) reap_detached();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        check(sys::quota_info(rt_startup().quota).status == STATUS_OK);
        if (reinterpret_cast<const QuotaInfo*>(rt_current().ipc)->allocations <= before.allocations) break;
        check(std::chrono::steady_clock::now() < deadline); sys::yield();
    }
}
static void file_writes() {
    const int fd = open("thread-data", O_RDWR | O_CREAT | O_TRUNC, 0); check(fd >= 0);
    std::vector<std::jthread> threads;
    for (unsigned i = 0; i < 4; ++i) threads.emplace_back([fd, i] {
        std::vector<char> data(8192, 'A' + i); check(write(fd, data.data(), data.size()) == ssize_t(data.size()));
    });
    for (auto& thread : threads) thread.join();
    threads.clear();
    std::vector<char> data(4 * 8192); check(pread(fd, data.data(), data.size(), 0) == ssize_t(data.size()));
    unsigned seen{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto value = data[i * 8192]; check(value >= 'A' && value <= 'D');
        check(!(seen & (1u << (value - 'A')))); seen |= 1u << (value - 'A');
        check(std::ranges::all_of(std::span{data}.subspan(i * 8192, 8192), [value](char ch) { return ch == value; }));
    }
    for (unsigned i = 0; i < 4; ++i) threads.emplace_back([fd, i] {
        std::vector<char> buffer(8192, 'a' + i); check(pwrite(fd, buffer.data(), buffer.size(), i * 8192) == ssize_t(buffer.size()));
    });
    for (auto& thread : threads) thread.join();
    check(write(fd, "end", 3) == 3 && !fsync(fd));
    check(pread(fd, data.data(), data.size(), 0) == ssize_t(data.size()));
    for (size_t i = 0; i < data.size(); ++i) check(data[i] == char('a' + i / 8192));
    char end[3]; check(pread(fd, end, 3, data.size()) == 3 && !memcmp(end, "end", 3));
    check(!close(fd) && !unlink("thread-data"));
}
static void file_barriers() {
    const int fd = open("thread-barrier", O_RDWR | O_CREAT | O_TRUNC, 0); check(fd >= 0);
    std::atomic<bool> submitted{}, release{};
    std::thread worker([&] {
        char data[]{"ordered"}; io_request request{fd, IO_WRITE, data, sizeof(data), 0}; request_id id{};
        check(!px_submit(&request, &id)); submitted.store(true); submitted.notify_one();
        release.wait(false); io_completion result{};
        check(px_completion(&result, IO_WAIT) == 1 && result.id == id && result.result == sizeof(data));
    });
    submitted.wait(false);
    check(!fsync(fd) && !close(fd)); // No completion polling by the submitting thread.
    release.store(true); release.notify_one(); worker.join();
    const int saved = open("thread-barrier", O_RDONLY); check(saved >= 0);
    char data[8]{}; check(read(saved, data, sizeof(data)) == sizeof(data) && !memcmp(data, "ordered", 8));
    check(!close(saved) && !unlink("thread-barrier"));
}
// This entry deliberately has no CRT/TLS registration or rt_thread record.
[[noreturn]] static void raw_entry(word_t ready, word_t failure) {
    (void)sys::notification_signal(ready);
    if (failure == 1) sys::exit(STATUS_INTERNAL);
    if (failure == 2) asm volatile(".word 0");
    for (;;) sys::yield();
}
[[noreturn]] static void raw_child(std::string_view mode) {
    const auto& env = rt_startup();
    rpc_op request; const word_t words[]{42};
    check(request.begin(env.environment, env.alloc(), rt_events(), words) == STATUS_OK);
    ch_packet reply; check(request.poll(reply) == STATUS_WOULD_BLOCK || reply.count == 1);
    while (request.active()) { const auto status = request.poll(reply); if (status == STATUS_OK) break; check(status == STATUS_WOULD_BLOCK); sys::yield(); }
    check(reply.count == 1);
    const auto scope = sys::scope_create(env.alloc()); check(scope.status == STATUS_OK); cap_owner nested{scope.value};
    auto stack = vm_area::create(env.alloc(), rt_vm(), 0, 16 * 1024); check(bool(stack));
    auto ipc = vm_area::create(env.alloc(), rt_vm(), 0, 4096); check(bool(ipc));
    check(sys::memory_populate(ipc->memory.selector(), 0).status == STATUS_OK);
    const auto descriptor = sys::memory_create(env.alloc(), 4096, VM_READ | VM_WRITE); check(descriptor.status == STATUS_OK);
    cap_owner start{descriptor.value};
    const auto ready = sys::notification_create(env.alloc(), 1); check(ready.status == STATUS_OK); cap_owner notice{ready.value};
    ThreadInit init{.version=THREAD_START_VERSION, .flags=0, .entry=reinterpret_cast<word_t>(&raw_entry),
        .stack=stack->address + stack->size - 16,
        .arguments={notice.selector(), mode == "raw-fault" ? word_t{1} : mode == "raw-trap" ? word_t{2} : word_t{0}},
        .ipc={.memory=ipc->memory.selector(), .page=0, .address=ipc->address, .pages=1}};
    check(mem_write(start.selector(), 0, std::as_bytes(std::span{&init, 1})) == STATUS_OK);
    const auto made = sys::thread_create({env.quota, nested.selector()}, env.vspace, env.cspace, start.selector(), 0, 0);
    check(made.status == STATUS_OK); cap_owner thread{made.value};
    check(sys::sc_bind(reply.capabilities[0].selector(), thread.selector()).status == STATUS_OK);
    check(sys::execution_set(thread.selector(), true).status == STATUS_OK);
    check(sys::wait_one(notice.selector(), SOURCE_READY).status == STATUS_OK);
    const word_t ready_words[]{4}; check(ch_send(env.environment, ready_words).status == STATUS_OK);
    for (;;) sys::yield();
}
static void execution_domain() {
    const int fd = open("/boot/thread-test.elf", O_RDONLY); check(fd >= 0);
    uint64_t bytes{}; check(!px_size(fd, &bytes));
    std::vector<std::byte> image(bytes); check(read(fd, image.data(), bytes) == ssize_t(bytes) && !close(fd));
    for (const std::string_view mode : {"child", "limit", "raw", "raw-fault", "raw-trap"}) {
        const bool limited = mode == "limit", raw = mode.starts_with("raw"), failure = mode == "raw-fault" || mode == "raw-trap";
        const auto expected = mode == "raw-fault" ? STATUS_INTERNAL : mode == "raw-trap" ? STATUS_PEER_FAULT : limited ? STATUS_OK : STATUS_CANCELED;
        cap_owner external;
        if (raw) {
            const auto sc = sys::sc_create(rt_startup().alloc(), rt_startup().domain, 0, 0, 1024, (rt_startup().cpus >= 64 ? ~word_t{} : (word_t{1} << rt_startup().cpus) - 1));
            check(sc.status == STATUS_OK); external = cap_owner{sc.value};
        }
        ch_pair signal; check(signal.open(rt_startup().alloc(), 4, 2, 1) == STATUS_OK);
        auto minted = sys::channel_mint(signal.ends[1].selector(), rt_startup().cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
        check(minted.status == STATUS_OK); cap_owner endpoint{minted.value};
        proc_task child;
        const std::string_view args[]{"thread-test", mode};
        proc_opts opts{.argv=args, .environment=endpoint.selector(),
            .resources={.bytes=4 * 1024 * 1024, .caps=192, .slots=256, .pages=36}, .events=rt_events()};
        check(proc_spawn(child, {rt_startup().alloc(), uint32_t(rt_startup().cpus), rt_startup().domain}, {.bytes=image}, opts) == STATUS_OK);
        const auto slice = sys::vm_slice(child.vspace(), 0x100000000, 4096, VM_READ, RIGHT_CONTROL | RIGHT_OBSERVE);
        check(slice.status == STATUS_OK); cap_owner restricted{slice.value};
        check(sys::vspace_stop(restricted.selector()).status == STATUS_DENIED);
        check(sys::vspace_exit(restricted.selector()).status == STATUS_DENIED);
        check(child.start() == STATUS_OK);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto progress = child.step(); check(bool(progress));
            if (child.result()) { check(failure && child.result()->code == expected); break; }
            ch_packet packet;
            if (ch_recv(signal.ends[0].selector(), packet, false).status == STATUS_OK) {
                if (packet.called) {
                    check(raw && packet.count == 1 && packet.words[0] == 42);
                    const CapXfer cap{external.selector(), RIGHT_ATTACH | RIGHT_INSPECT, CAP_COPY, 0};
                    check(ch_send(packet.capabilities[0].selector(), {}, true, std::span{&cap, 1}).status == STATUS_OK);
                } else { check(packet.words[0] == (limited ? 0u : 4u)); break; }
            }
            check(std::chrono::steady_clock::now() < deadline); sys::yield();
        }
        if (!limited && !failure) {
            for (bool enabled : {false, true, false}) {
                const auto intent = child.execution(enabled); check(intent.status == STATUS_OK);
                for (;;) {
                    check(bool(child.step()) && !child.result());
                    const auto info = child.execution_info(); check(bool(info));
                    if (info->applied == intent.value) { check(info->status == STATUS_OK && bool(info->flags & DOMAIN_ENABLED) == enabled); break; }
                    check(std::chrono::steady_clock::now() < deadline); sys::yield();
                }
                const auto before = child.execution_info()->runtime;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const auto after = child.execution_info()->runtime;
                check(enabled ? after > before : after == before);
            }
            check(child.retire() == STATUS_OK);
        }
        for (;;) {
            check(bool(child.step()));
            if (const auto result = child.result()) {
                check(result->code == expected);
                if (failure) check(result->thread && result->exit.status == expected
                    && result->exit.origin == (mode == "raw-trap" ? EXIT_FAULT : EXIT_NORMAL));
                if (mode == "raw-trap") check(result->exit.pc != 0);
                check(child.retire(result->code) == STATUS_OK);
                if (child.drained()) break;
            }
            check(std::chrono::steady_clock::now() < deadline); sys::yield();
        }
        if (external) {
            check(sys::sc_info(external.selector()).status == STATUS_OK);
            check(!reinterpret_cast<const ScInfo*>(rt_current().ipc)->bound);
            check(sys::object_destroy(external.selector()).status == STATUS_OK);
        }
    }
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]}.starts_with("raw")) raw_child(argv[1]);
    if (argc == 2 && (std::string_view{argv[1]} == "child" || std::string_view{argv[1]} == "limit")) {
        if (std::string_view{argv[1]} == "limit") {
            std::atomic<bool> release{};
            std::vector<rt_thread> threads;
            for (;;) {
                auto result = rt_thread_create([](void* gate) -> void* {
                    static_cast<std::atomic<bool>*>(gate)->wait(false); return nullptr;
                }, &release, {.stack_size=4 * 1024 * 1024});
                if (!result) { check(result.error() == STATUS_NO_MEMORY || result.error() == STATUS_EXHAUSTED); break; }
                threads.push_back(*result);
            }
            release.store(true); release.notify_all();
            for (auto id : threads) check(bool(rt_thread_join(id)));
            const word_t value[]{0}; check(ch_send(rt_startup().environment, value).status == STATUS_OK); return 0;
        }
        for (unsigned i = 0; i < 4; ++i) check(bool(rt_thread_create([](void*) -> void* {
            for (;;) (void)__atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
        }, nullptr)));
        const word_t value[]{4}; check(ch_send(rt_startup().environment, value).status == STATUS_OK);
        for (;;) sys::yield();
    }
    check(px_init() == STATUS_OK);
    if (argc == 2 && std::string_view{argv[1]} == "write") {
        file_writes(); file_barriers(); say("[threads] sequential writes and parallel explicit offsets ok\n"); return 0;
    }
    if (argc == 2 && std::string_view{argv[1]} == "hold") {
        std::jthread cpu([](std::stop_token stop) { while (!stop.stop_requested()) (void)__atomic_add_fetch(&count, 1, __ATOMIC_RELAXED); });
        std::jthread timer([](std::stop_token stop) { while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
        say("[threads] ready\n"); char byte{};
        while (read(0, &byte, 1) == 1 && byte != 'q') {}
        return 0;
    }
    if (argc == 2 && std::string_view{argv[1]} == "last") {
        check(bool(rt_thread_create([](void* argument) -> void* {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(std::string_view{static_cast<char*>(argument)} == "last");
            say("[threads] initial thread exit preserves argv, fd and workers\n"); return nullptr;
        }, argv[1])));
        pthread_exit(nullptr);
    }
    if (argc == 2 && std::string_view{argv[1]} == "exit") {
        check(bool(rt_thread_create([](void*) -> void* { rt_exit(23); }, nullptr)));
        for (;;) sys::yield();
    }
    if (argc == 2 && std::string_view{argv[1]} == "fault") {
        check(bool(rt_thread_create([](void*) -> void* { mutex.lock(); sys::exit(STATUS_INTERNAL); }, nullptr)));
        for (;;) sys::yield();
    }
    rt_thread threads[4];
    for (size_t i = 0; i < 4; ++i) {
        auto thread = rt_thread_create(worker, reinterpret_cast<void*>(i + 1));
        check(bool(thread)); threads[i] = *thread;
    }
    {
        std::unique_lock lock{mutex};
        while (ready != 4) check(!rt_cond_wait(&cond, &mutex));
        go = true; rt_cond_wake(&cond, true);
    }
    for (size_t i = 0; i < 4; ++i) {
        const auto result = rt_thread_join(threads[i]);
        check(result && *result == reinterpret_cast<void*>(i + 1));
    }
    check(count == 400 && destroyed.load() == 4 && local == 42);
    say("[threads] native TLS, heap, mutex, cond, join and destructors ok\n");
    standard_threads(); native_lifetime();
    say("[threads] C++ jthread, shared_ptr, once, atomic wait and deadlines ok\n");
    check(!pthread_key_create(&key, key_cleanup));
    const auto fd = open("/boot/README.TXT", O_RDONLY); check(fd >= 0);
    shared_fd = dup(fd); check(shared_fd >= 0 && !close(fd));
    pthread_t handles[4];
    for (size_t i = 0; i < 4; ++i) check(!pthread_create(&handles[i], nullptr, io_worker, reinterpret_cast<void*>(i + 1)));
    for (size_t i = 0; i < 4; ++i) { void* result{}; check(!pthread_join(handles[i], &result) && result == reinterpret_cast<void*>(i + 1)); }
    check(cleaned == 4 && keyed == 4); check(!pthread_key_delete(key) && !close(shared_fd));
    say("[threads] pthread, TLS errno, cleanup, shared fd and parallel RPC ok\n");
    const auto authority = cap_info(rt_startup().quota); check(bool(authority));
    if (authority->view.rights & RIGHT_SPLIT) {
        execution_domain();
        say("[threads] execution pause, resume, paused retirement and admission rollback ok\n");
    }
    return 0;
}
