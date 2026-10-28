#include <thread.hpp>
#include <proc.hpp>
#include <rpc.hpp>
#include <libk/elf.hpp>
#include <uapi/riscv64.h>
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

static constinit _mutex mutex;
static constinit _cond cond;
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
        (void)write(2, message.data(), message.size()); _thread_exit(STATUS_INTERNAL);
    }
}
static void say(const char* text) { check(write(1, text, strlen(text)) == ssize_t(strlen(text))); }
static void sample(const char* name, word_t ticks) {
    const auto text = std::string{"[api] "} + name + "=" + std::to_string(ticks) + " ticks @"
        + std::to_string(_startup().frequency) + "Hz\n";
    say(text.c_str());
}
static auto worker(void* argument) -> void* {
    thread_local thread_value value;
    (void)value;
    check(local == 42 && aligned[0] == 0 && aligned[sizeof(aligned) - 1] == 0 && uintptr_t(aligned) % 128 == 0);
    local = reinterpret_cast<uintptr_t>(argument); aligned[128] = char(local);
    {
        std::unique_lock lock{mutex}; ++ready; _cond_wake(&cond, true);
        while (!go) check(!_cond_wait(&cond, &mutex));
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
    const auto self = _thread_join(_thread_self()); check(!self && self.error() == STATUS_DEADLOCK);
    const auto target = _thread_create([](void*) -> void* { return nullptr; }, nullptr); check(bool(target));
    std::atomic<bool> start{};
    status_t joined{}, detached{};
    std::thread a([&] { start.wait(false); const auto result = _thread_join(*target); joined = result ? STATUS_OK : result.error(); });
    std::thread b([&] { start.wait(false); detached = _thread_detach(*target); });
    start.store(true); start.notify_all(); a.join(); b.join();
    check((joined == STATUS_OK) != (detached == STATUS_OK));
    const auto no_op = [](void*) -> void* { return nullptr; };
    const auto reap_detached = [&] {
        auto made = _thread_create(no_op, nullptr, {.detached=true}); check(bool(made));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto status = _thread_detach(*made);
            if (status == STATUS_NOT_FOUND) break;
            check(status == STATUS_BAD_ARGS && std::chrono::steady_clock::now() < deadline); _yield();
        }
    };
    reap_detached(); check(_quota_info(_startup().quota).status == STATUS_OK);
    const auto before = *reinterpret_cast<const QuotaInfo*>(_ipc());
    for (unsigned i = 0; i < 12; ++i) reap_detached();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        check(_quota_info(_startup().quota).status == STATUS_OK);
        if (reinterpret_cast<const QuotaInfo*>(_ipc())->allocations <= before.allocations) break;
        check(std::chrono::steady_clock::now() < deadline); _yield();
    }
}
static void cpu_runtime() {
    const auto read = [] {
        const auto value = _cap_copy<VSpaceRuntime>(_vspace_runtime(_startup().vspace));
        check(value && value->version == 1 && value->size == sizeof(VSpaceRuntime));
        return *value;
    };
    const auto before = read(); auto previous = before;
    std::atomic<uint64_t> used{};
    uint64_t longest{};
    for (unsigned wave = 0; wave < 6; ++wave) {
        std::array<std::thread, 4> workers;
        for (auto& thread : workers) thread = std::thread([&] {
            const auto start = _cap_copy<ThreadInfo>(_thread_info(0)); check(bool(start));
            const auto due = _after(std::chrono::milliseconds{3}); check(bool(due));
            while (_clock_now().value < *due) std::atomic_signal_fence(std::memory_order_relaxed);
            const auto end = _cap_copy<ThreadInfo>(_thread_info(0)); check(end && end->runtime >= start->runtime);
            used.fetch_add(end->runtime - start->runtime, std::memory_order_relaxed);
        });
        for (unsigned i = 0; i < 16; ++i) {
            const auto begin = _clock_now().value;
            const auto sample = read();
            longest = std::max(longest, _clock_now().value - begin);
            check(sample.sampled >= previous.sampled && sample.runtime >= previous.runtime);
            previous = sample;
            const auto made = _memory_create(_startup().alloc(), 4096, VM_READ | VM_WRITE);
            check(made.status == STATUS_OK); _cap_owner memory{made.value};
            check(_object_destroy(memory.selector()).status == STATUS_OK);
        }
        for (auto& thread : workers) thread.join();
        const auto after = read();
        check(after.runtime >= previous.runtime && after.runtime - before.runtime >= used.load());
        previous = after;
    }
    sample("runtime-query-max", longest);
    say("[threads] CPU runtime survives short threads and object churn\n");
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
// This entry deliberately has no CRT/TLS registration or _thread record.
[[noreturn]] static void raw_entry(word_t ready, word_t failure) {
    (void)_notification_signal(ready);
    if (failure == 1) _thread_exit(STATUS_INTERNAL);
    if (failure == 2) asm volatile(".word 0");
    if (failure == 3) asm volatile("ld zero, 0(zero)" ::: "memory");
    if (failure == 4) asm volatile("ebreak");
    for (;;) _yield();
}
[[noreturn]] static void raw_child(std::string_view mode) {
    const auto& env = _startup();
    _rpc_op request; const word_t words[]{42};
    check(request.begin(env.bootstrap, env.alloc(), &_events(), words) == STATUS_OK);
    _ch_packet reply; check(request.poll(reply) == STATUS_WOULD_BLOCK || reply.count == 1);
    while (request.active()) { const auto status = request.poll(reply); if (status == STATUS_OK) break; check(status == STATUS_WOULD_BLOCK); _yield(); }
    check(reply.count == 1);
    const auto scope = _scope_create(env.alloc()); check(scope.status == STATUS_OK); _cap_owner nested{scope.value};
    auto stack = _vm_area::create(env.alloc(), _vm(), 0, 16 * 1024); check(bool(stack));
    auto wired = _vm(); wired.pager = 0;
    auto ipc = _vm_area::create(env.alloc(), wired, 0, 4096); check(bool(ipc));
    check(_memory_populate(ipc->memory.selector(), 0).status == STATUS_OK);
    const auto descriptor = _memory_create(env.alloc(), 4096, VM_READ | VM_WRITE); check(descriptor.status == STATUS_OK);
    _cap_owner start{descriptor.value};
    const auto ready = _notification_create(env.alloc(), 1); check(ready.status == STATUS_OK); _cap_owner notice{ready.value};
    ThreadInit init{.version=THREAD_START_VERSION, .flags=0, .entry=reinterpret_cast<word_t>(&raw_entry),
        .stack=stack->address + stack->size - 16,
        .arguments={notice.selector(), mode == "raw-fault" ? word_t{1} : mode == "raw-trap" ? word_t{2}
            : mode == "raw-page" ? word_t{3} : mode == "raw-break" ? word_t{4} : word_t{0}},
        .ipc={.memory=ipc->memory.selector(), .page=0, .address=ipc->address, .pages=1}};
    check(_mem_write(start.selector(), 0, std::as_bytes(std::span{&init, 1})).status == STATUS_OK);
    const auto made = _thread_create({env.quota, nested.selector()}, env.vspace, env.cspace, start.selector(), 0, 0);
    check(made.status == STATUS_OK); _cap_owner thread{made.value};
    check(_sc_bind(reply.capabilities[0].selector(), thread.selector()).status == STATUS_OK);
    check(_execution_set(thread.selector(), true).status == STATUS_OK);
    check(_wait_one(notice.selector(), SOURCE_READY).status == STATUS_OK);
    const word_t ready_words[]{4}; check(_ch_send(env.bootstrap, ready_words).status == STATUS_OK);
    for (;;) _yield();
}
static void execution_domain() {
    const int fd = open("/boot/thread-test.elf", O_RDONLY); check(fd >= 0);
    uint64_t bytes{}; check(!px_size(fd, &bytes));
    std::vector<std::byte> image(bytes); check(read(fd, image.data(), bytes) == ssize_t(bytes) && !close(fd));
    for (const std::string_view mode : {"child", "limit", "raw", "raw-fault", "raw-trap", "raw-page", "raw-break"}) {
        const auto fault = mode == "raw-trap" ? FAULT_ILLEGAL : mode == "raw-page" ? FAULT_UNMAPPED
            : mode == "raw-break" ? FAULT_BREAKPOINT : FAULT_NONE;
        const bool limited = mode == "limit", raw = mode.starts_with("raw"), failure = mode == "raw-fault" || fault != FAULT_NONE;
        const auto expected = mode == "raw-fault" ? STATUS_INTERNAL : fault != FAULT_NONE ? STATUS_PEER_FAULT : limited ? STATUS_OK : STATUS_CANCELED;
        _cap_owner external;
        if (raw) {
            const auto sc = _sc_create(_startup().alloc(), _startup().domain, 0, 0, 1024, (_startup().cpus >= 64 ? ~word_t{} : (word_t{1} << _startup().cpus) - 1));
            check(sc.status == STATUS_OK); external = _cap_owner{sc.value};
        }
        _ch_pair signal; check(signal.open(_startup().alloc(), 4, 2, 1) == STATUS_OK);
        auto minted = _channel_mint(signal.ends[1].selector(), _startup().cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
        check(minted.status == STATUS_OK); _cap_owner endpoint{minted.value};
        const std::string_view args[]{"thread-test", mode};
        _proc_opts opts{.argv=args, .bootstrap=endpoint.selector(),
            .resources={.bytes=4 * 1024 * 1024, .caps=192, .slots=256, .pages=36}, .events=&_events()};
        auto made = _proc_create({_startup().alloc(), _startup().domain}, {.bytes=image}, opts);
        check(bool(made)); auto child = std::move(*made);
        const auto slice = _vm_slice(child.vspace(), 0x100000000, 4096, VM_READ, RIGHT_CONTROL | RIGHT_OBSERVE | RIGHT_INSPECT);
        check(slice.status == STATUS_OK); _cap_owner restricted{slice.value};
        check(_vspace_stop(restricted.selector()).status == STATUS_DENIED);
        check(_vspace_exit(restricted.selector()).status == STATUS_DENIED);
        check(_vspace_runtime(restricted.selector()).status == STATUS_DENIED);
        check(child.start() == STATUS_OK);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            check(child.poll() == STATUS_OK);
            if (child.result()) { check(failure && child.result()->code == expected); break; }
            _ch_packet packet;
            if (_ch_try_recv(signal.ends[0].selector(), packet).status == STATUS_OK) {
                if (packet.called) {
                    check(raw && packet.count == 1 && packet.words[0] == 42);
                    const CapXfer cap{external.selector(), RIGHT_ATTACH | RIGHT_INSPECT, CAP_COPY, 0};
                    check(_ch_send(packet.capabilities[0].selector(), {}, std::span{&cap, 1}).status == STATUS_OK);
                } else { check(packet.words[0] == (limited ? 0u : 4u)); break; }
            }
            check(std::chrono::steady_clock::now() < deadline); _yield();
        }
        if (!limited && !failure) {
            for (bool enabled : {false, true, false}) {
                const auto intent = _domain_execution(child.domain(), enabled); check(intent.status == STATUS_OK);
                for (;;) {
                    check(child.poll() == STATUS_OK && !child.result());
                    const auto info = _cap_copy<DomainInfo>(_domain_info(child.domain())); check(bool(info));
                    if (info->applied == intent.value) { check(info->status == STATUS_OK && bool(info->flags & DOMAIN_ENABLED) == enabled); break; }
                    check(std::chrono::steady_clock::now() < deadline); _yield();
                }
                const auto before = _cap_copy<DomainInfo>(_domain_info(child.domain()))->runtime;
                const auto cpu_before = _cap_copy<VSpaceRuntime>(_vspace_runtime(child.vspace())); check(bool(cpu_before));
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const auto after = _cap_copy<DomainInfo>(_domain_info(child.domain()))->runtime;
                check(enabled ? after > before : after == before);
                const auto cpu_after = _cap_copy<VSpaceRuntime>(_vspace_runtime(child.vspace())); check(bool(cpu_after));
                if (!raw) check(enabled ? cpu_after->runtime > cpu_before->runtime : cpu_after->runtime == cpu_before->runtime);
            }
            check(child.retire() == STATUS_OK);
        }
        for (;;) {
            check(child.poll() == STATUS_OK);
            if (const auto result = child.result()) {
                check(result->code == expected);
                if (failure) check(result->thread && result->exit.status == expected
                    && result->exit.origin == (fault != FAULT_NONE ? EXIT_FAULT : EXIT_NORMAL)
                    && result->exit.kind == fault);
                if (fault != FAULT_NONE) check(result->exit.pc != 0);
                check(child.retire() == STATUS_OK);
                if (child.retired()) break;
            }
            check(std::chrono::steady_clock::now() < deadline); _yield();
        }
        if (external) {
            check(_sc_info(external.selector()).status == STATUS_OK);
            check(!reinterpret_cast<const ScInfo*>(_ipc())->bound);
            check(_object_destroy(external.selector()).status == STATUS_OK);
        }
    }
}
static void address_claims() {
    _vm_area area;
    check(area.open(_vm(), 0, 3 * 4096) == STATUS_OK);
    check(area.guard(4096) == STATUS_OK);
    const auto made = _memory_create(_startup().alloc(), 8192, VM_READ | VM_WRITE);
    check(made.status == STATUS_OK); _cap_owner memory{made.value};
    check(area.attach(memory.selector(), 0, 8192, VM_READ | VM_WRITE, 4096) == STATUS_OK);
    *reinterpret_cast<volatile word_t*>(area.address + 4096) = 73;
    const auto reserve = [&](word_t address, word_t bytes) {
        const VmAlloc request{VM_ALLOC_VERSION, sizeof(VmAlloc), address, bytes, 4096,
            4096, word_t{1} << 38, VM_READ | VM_WRITE};
        ::memcpy(reinterpret_cast<void*>(_ipc()), &request, sizeof(request));
        return _vm_reserve(_startup().vspace);
    };
    check(reserve(area.address, area.size).status == STATUS_CONFLICT);
    check(area.detach() == STATUS_OK);
    check(reserve(area.address, area.size).status == STATUS_CONFLICT);
    check(area.attach(memory.selector(), 0, 8192, VM_READ | VM_WRITE, 4096) == STATUS_OK);
    const auto slice = _vm_slice(_startup().vspace, area.address, area.size, VM_READ | VM_WRITE, RIGHT_UNMAP);
    check(slice.status == STATUS_OK); _cap_owner admin{slice.value};
    check(_vm_wait(admin.selector(), VM_OP_CLEAR) == STATUS_OK);
    _vm_area replacement;
    check(replacement.open(_vm(), area.address, area.size) == STATUS_OK);
    check(replacement.attach(memory.selector(), 0, 8192, VM_READ | VM_WRITE) == STATUS_OK);
    check(_vm_wait(area.region.selector(), VM_OP_MAP,
        {memory.selector(), area.address, 4096, 0, VM_READ | VM_WRITE}) == STATUS_STALE);
    check(area.close() == STATUS_OK);
    check(*reinterpret_cast<volatile word_t*>(replacement.address) == 73);
    check(replacement.close() == STATUS_OK);
    std::array<std::byte, 8> output{};
    const auto copied = _mem_read(memory.selector(), 8190, output);
    check(copied.bytes == 2 && copied.status != STATUS_OK);
    _vm_area reclaimed;
    check(reclaimed.open(_vm(), 0, 4096) == STATUS_OK);
    const auto backing = _memory_create(_startup().alloc(), 4096, VM_READ | VM_WRITE);
    check(backing.status == STATUS_OK); _cap_owner original{backing.value};
    check(reclaimed.attach(original.selector(), 0, 4096, VM_READ | VM_WRITE) == STATUS_OK);
    check(_cap_revoke(original.selector()).status == STATUS_OK);
    check(reclaimed.detach() == STATUS_OK);
    check(reserve(reclaimed.address, reclaimed.size).status == STATUS_CONFLICT);
    check(reclaimed.attach(memory.selector(), 0, 4096, VM_READ | VM_WRITE) == STATUS_OK);
    check(_object_destroy(memory.selector()).status == STATUS_OK);
    const auto retired = *_after(std::chrono::seconds{2});
    for (;;) {
        const auto records = _cap_copy<VmRecords>(_vspace_records(reclaimed.region.selector()));
        check(bool(records));
        if (records->count == 1 && records->records[0].kind == VM_RECORD_RESERVED) break;
        check(_deadline_status(retired) == STATUS_OK); _yield();
    }
    check(reserve(reclaimed.address, reclaimed.size).status == STATUS_CONFLICT);
    check(reclaimed.close() == STATUS_OK);
}
static void thread_capacity() {
    // More live threads than the reaper's private watch batch. No one exits
    // until every creation succeeds, so this exercises the former hard limit.
    const auto serial = _clock_now().value;
    for (unsigned i = 0; i < 12; ++i) {
        auto thread = _thread_create([](void*) -> void* { return nullptr; }, nullptr);
        check(bool(thread)); check(bool(_thread_join(*thread)));
    }
    sample("create_join_12", _clock_now().value - serial);
    std::atomic<bool> release{};
    std::vector<_thread> threads;
    const auto begin = _clock_now().value;
    for (unsigned i = 0; i < 72; ++i) {
        auto thread = _thread_create([](void* gate) -> void* {
            static_cast<std::atomic<bool>*>(gate)->wait(false); return nullptr;
        }, &release);
        check(bool(thread)); threads.push_back(*thread);
    }
    const auto released = _clock_now().value;
    release.store(true); release.notify_all();
    for (auto thread : threads) check(bool(_thread_join(thread)));
    const auto end = _clock_now().value;
    sample("create_72", released - begin); sample("reclaim_72", end - released);
}
static void event_dispatch() {
    for (const word_t hints : {word_t{1}, word_t{80}}) {
        _ev_port port; check(port.open(_startup().alloc(), 80, hints) == STATUS_OK);
        const auto made = _notification_create(_startup().alloc(), 1);
        check(made.status == STATUS_OK); _cap_owner notice{made.value};
        std::array<_ev_watch, 72> watches;
        for (auto& watch : watches) check(watch.open(&port, notice.selector(), SOURCE_READY) == STATUS_OK);
        const auto begin = _clock_now().value;
        check(_notification_signal(notice.selector()).status == STATUS_OK);
        std::array<bool, 72> seen{};
        size_t count{};
        const auto deadline = *_after(std::chrono::seconds{2});
        while (count != watches.size()) {
            check(_ev_wait(&port, deadline).status == STATUS_OK);
            for (size_t i = 0; i < watches.size(); ++i) if (!seen[i]) {
                const auto event = watches[i].event();
                if (event && (event->condition & SOURCE_READY)) { seen[i] = true; ++count; watches[i].ack(); }
            }
        }
        check(_notification_take(notice.selector()).status == STATUS_OK);
        for (auto& watch : watches) check(watch.arm().status == STATUS_OK);
        while (_source_info(port.selector()).value & (PORT_READABLE | PORT_OVERFLOW)) check(_ev_wait(&port).status == STATUS_OK);
        sample(hints == 1 ? "overflow_72" : "events_72", _clock_now().value - begin);
        for (auto& watch : watches) { check(!watch.pending()); check(watch.close() == STATUS_OK); }
    }
}
static void rpc_reuse() {
    _ch_pair server; check(server.open(_startup().alloc(), 1, 1, 1) == STATUS_OK);
    const auto minted = _channel_mint(server.ends[1].selector(), _startup().cspace, 1, RIGHT_SEND);
    check(minted.status == STATUS_OK); _cap_owner peer{minted.value};
    _rpc_op call;
    check(call.begin(peer.selector(), _startup().alloc(), nullptr, {}) == STATUS_OK && call.sent());
    _ch_packet request, response;
    check(_ch_try_recv(server.ends[0].selector(), request).status == STATUS_OK && request.called);
    check(_ch_send(request.capabilities[0].selector(), {}).status == STATUS_OK);
    check(call.poll(response) == STATUS_OK && call.sent());
    check(call.begin(peer.selector(), _startup().alloc(), nullptr, {}, {}, 0, _clock_now().value) == STATUS_TIMED_OUT);
    check(!call.sent() && !call.active());
}
static void loader_contract() {
    const int fd = open("/boot/thread-test.elf", O_RDONLY); check(fd >= 0);
    uint64_t size{}; check(!px_size(fd, &size));
    std::vector<std::byte> bytes(size);
    check(read(fd, bytes.data(), size) == ssize_t(size) && !close(fd));
    auto original = libk::elf_view::parse(bytes);
    check(bool(original));
    std::vector<libk::elf_seg> headers(original.segment_count() + 1);
    auto snapshot = libk::elf_view::parse(bytes, headers);
    check(bool(snapshot));
    std::vector<std::byte> image(bytes.begin(), bytes.end());
    const uint64_t phoff = (image.size() + 7) & ~uint64_t{7};
    const uint16_t count = headers.size();
    headers.back() = {.type=1, .flags=6, .offset=0, .va=0x100000000,
        .pa=0, .file_size=0, .size=4096, .align=4096};
    image.resize(phoff + headers.size() * sizeof(headers[0]));
    ::memcpy(image.data() + phoff, headers.data(), headers.size() * sizeof(headers[0]));
    ::memcpy(image.data() + 32, &phoff, sizeof(phoff));
    ::memcpy(image.data() + 56, &count, sizeof(count));
    std::vector<libk::elf_seg> saved(headers.size());
    const auto captured = libk::elf_view::parse(image, saved);
    check(bool(captured));
    const auto first = *captured.segment(0);
    auto changed = first; changed.offset = UINT64_MAX;
    ::memcpy(image.data() + phoff, &changed, sizeof(changed));
    check(captured.segment(0)->offset == first.offset);
    ::memcpy(image.data() + phoff, &first, sizeof(first));
    const std::string_view args[]{"thread-test", "layout"};
    auto prepared = _proc_create({.bytes=image}, {.argv=args, .resources={.bytes=4 * 1024 * 1024, .caps=256}});
    check(bool(prepared));
    check(!(_source_info(prepared->initial_thread()).value & THREAD_ENABLED));
    check(prepared->start() == STATUS_OK);
    auto result = prepared->join(*_after(std::chrono::seconds{5}));
    check(result && result->code == 23 && prepared->retired());
    headers.back().va = 0x3000000000;
    ::memcpy(image.data() + phoff, headers.data(), headers.size() * sizeof(headers[0]));
    auto outside = _proc_spawn({.bytes=image}, {.argv=args, .resources={.bytes=4 * 1024 * 1024, .caps=256}});
    check(bool(outside)); result = outside->join(*_after(std::chrono::seconds{5}));
    check(result && result->code == 23);
    headers.back().va = RISCV64_LOW_GUARD_END;
    ::memcpy(image.data() + phoff, headers.data(), headers.size() * sizeof(headers[0]));
    auto low = _proc_spawn({.bytes=image}, {.argv=args, .resources={.bytes=4 * 1024 * 1024, .caps=256}});
    check(bool(low)); result = low->join(*_after(std::chrono::seconds{5}));
    check(result && result->code == 23);
    const std::string_view delayed[]{"thread-test", "join"};
    auto child = _proc_spawn({.bytes=image}, {.argv=delayed, .resources={.bytes=4 * 1024 * 1024, .caps=256}});
    check(bool(child));
    const auto early = child->join(*_after(std::chrono::milliseconds{1}));
    check(!early && early.error() == STATUS_TIMED_OUT && !child->stopping());
    result = child->join(*_after(std::chrono::seconds{5}));
    check(result && result->code == 23 && child->retired());
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "layout") return 23;
    if (argc == 2 && std::string_view{argv[1]} == "join") { _sleep_ns(30000000); return 23; }
    if (argc == 2 && std::string_view{argv[1]}.starts_with("raw")) raw_child(argv[1]);
    if (argc == 2 && (std::string_view{argv[1]} == "child" || std::string_view{argv[1]} == "limit")) {
        if (std::string_view{argv[1]} == "limit") {
            std::atomic<bool> release{};
            std::vector<_thread> threads;
            for (;;) {
                auto result = _thread_create([](void* gate) -> void* {
                    static_cast<std::atomic<bool>*>(gate)->wait(false); return nullptr;
                }, &release, {.stack_size=4 * 1024 * 1024});
                if (!result) { check(result.error() == STATUS_NO_MEMORY || result.error() == STATUS_EXHAUSTED); break; }
                threads.push_back(*result);
            }
            release.store(true); release.notify_all();
            for (auto id : threads) check(bool(_thread_join(id)));
            const word_t value[]{0}; check(_ch_send(_startup().bootstrap, value).status == STATUS_OK); return 0;
        }
        for (unsigned i = 0; i < 4; ++i) check(bool(_thread_create([](void*) -> void* {
            for (;;) (void)__atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
        }, nullptr)));
        const word_t value[]{4}; check(_ch_send(_startup().bootstrap, value).status == STATUS_OK);
        for (;;) _yield();
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
        check(bool(_thread_create([](void* argument) -> void* {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(std::string_view{static_cast<char*>(argument)} == "last");
            say("[threads] initial thread exit preserves argv, fd and workers\n"); return nullptr;
        }, argv[1])));
        pthread_exit(nullptr);
    }
    if (argc == 2 && std::string_view{argv[1]} == "exit") {
        check(bool(_thread_create([](void*) -> void* { _exit(23); }, nullptr)));
        for (;;) _yield();
    }
    if (argc == 2 && std::string_view{argv[1]} == "fault") {
        check(bool(_thread_create([](void*) -> void* { mutex.lock(); _thread_exit(STATUS_INTERNAL); }, nullptr)));
        for (;;) _yield();
    }
    _thread threads[4];
    for (size_t i = 0; i < 4; ++i) {
        auto thread = _thread_create(worker, reinterpret_cast<void*>(i + 1));
        check(bool(thread)); threads[i] = *thread;
    }
    {
        std::unique_lock lock{mutex};
        while (ready != 4) check(!_cond_wait(&cond, &mutex));
        go = true; _cond_wake(&cond, true);
    }
    for (size_t i = 0; i < 4; ++i) {
        const auto result = _thread_join(threads[i]);
        check(result && *result == reinterpret_cast<void*>(i + 1));
    }
    check(count == 400 && destroyed.load() == 4 && local == 42);
    say("[threads] native TLS, heap, mutex, cond, join and destructors ok\n");
    address_claims(); event_dispatch(); rpc_reuse();
    standard_threads(); native_lifetime(); cpu_runtime();
    say("[threads] C++ jthread, shared_ptr, once, atomic wait and deadlines ok\n");
    check(!pthread_key_create(&key, key_cleanup));
    const auto fd = open("/boot/README.TXT", O_RDONLY); check(fd >= 0);
    shared_fd = dup(fd); check(shared_fd >= 0 && !close(fd));
    pthread_t handles[4];
    for (size_t i = 0; i < 4; ++i) check(!pthread_create(&handles[i], nullptr, io_worker, reinterpret_cast<void*>(i + 1)));
    for (size_t i = 0; i < 4; ++i) { void* result{}; check(!pthread_join(handles[i], &result) && result == reinterpret_cast<void*>(i + 1)); }
    check(cleaned == 4 && keyed == 4); check(!pthread_key_delete(key) && !close(shared_fd));
    say("[threads] pthread, TLS errno, cleanup, shared fd and parallel RPC ok\n");
    const auto authority = _cap_copy<CapInfo>(_cap_info(_startup().quota)); check(bool(authority));
    if (authority->view.rights & RIGHT_SPLIT) {
        thread_capacity();
        loader_contract();
        execution_domain();
        say("[threads] execution pause, resume, paused retirement and admission rollback ok\n");
    }
    return 0;
}
