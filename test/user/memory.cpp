#include <posix.hpp>
#include <libk/parse.hpp>
#include <libk/fmt.hpp>
#include <thread.hpp>
#include <unistd.h>
#include <memory>
#include <ubus/ubus.hpp>
#include <atomic>
#include <thread>

static auto pattern(size_t i, size_t pass) -> uint64_t { return (uint64_t{i} * 0x9e3779b97f4a7c15ULL) ^ (pass * 0x123456789abcdefULL); }
static auto contract_error(unsigned line) -> status_t {
    char text[96]; const auto result = libk::fmt::format_to_n(text, "memory contract failed line={}\n", line);
    if (result) (void)::write(1, text, result.produced);
    return STATUS_INTERNAL;
}
// Exercise public content immutability and async destination authority.
static auto contracts() -> status_t {
    {
        const auto auth = _startup().alloc();
        const auto pg = _pager_create(auth), src = _memory_create(auth, 4096, VM_READ | VM_WRITE);
        if (pg.status != STATUS_OK || src.status != STATUS_OK) return STATUS_NO_MEMORY;
        _cap_owner pager{pg.value}, source{src.value};
        const std::byte data{0x6b};
        if (_mem_write(source.selector(), 0, {&data, 1}).status != STATUS_OK
            || _memory_seal(source.selector()).status != STATUS_OK) return contract_error(__LINE__);
        const auto cloned = _memory_clone(auth, source.selector(), 0, 1);
        if (cloned.status != STATUS_OK) return cloned.status;
        _cap_owner clone{cloned.value};
        const auto made = _memory_create_pager(auth, 4096, VM_READ | VM_WRITE, pager.selector(), 0, 1);
        if (made.status != STATUS_OK && made.status != STATUS_PENDING) return made.status;
        _cap_owner target{made.value};
        if (made.status == STATUS_PENDING && _op_wait(target.selector()).status != STATUS_OK) return contract_error(__LINE__);
        const auto object = _cap_copy<ObjectInfo>(_object_info(target.selector())); if (!object) return object.error();
        if (_pager_restore(pager.selector(), object->object_id, 0).status != STATUS_PENDING) return contract_error(__LINE__);
        if (_pager_claim(pager.selector()).status != STATUS_OK) return contract_error(__LINE__);
        const auto request = *reinterpret_cast<const PagerReq*>(_ipc());
        if (_pager_supply(pager.selector(), request.id, source.selector(), 0).status != STATUS_BAD_ARGS
            || _pager_supply(pager.selector(), request.id, source.selector(), 0, PAGER_SUPPLY_COPY).status != STATUS_OK)
            return contract_error(__LINE__);
        for (auto memory : {source.selector(), clone.selector(), target.selector()}) {
            std::byte output{};
            if (_mem_read(memory, 0, {&output, 1}).status != STATUS_OK || output != data) return contract_error(__LINE__);
        }
    }
    for (size_t trial = 0; trial < 32; ++trial) {
        const auto made = _scope_create(_startup().alloc()); if (made.status != STATUS_OK) return made.status;
        _cap_owner scope{made.value}; const CreateAuth auth{_startup().quota, scope.selector()};
        const auto pg = _pager_create(auth), src = _memory_create(auth, 8192, VM_READ | VM_WRITE);
        if (pg.status != STATUS_OK || src.status != STATUS_OK) return STATUS_NO_MEMORY;
        _cap_owner pager{pg.value}, source{src.value};
        const std::byte data{0x6b};
        if (_mem_write(source.selector(), 0, {&data, 1}).status != STATUS_OK) return contract_error(__LINE__);
        std::atomic<bool> go{}; status_t written{};
        std::thread writer([&] { while (!go.load(std::memory_order_acquire)) _thread_yield(); written = _mem_write(source.selector(), 4096, {&data, 1}).status; });
        go.store(true, std::memory_order_release);
        auto bound = _memory_bind_pager(source.selector(), pager.selector()).status;
        writer.join();
        if (bound == STATUS_BUSY) bound = _memory_bind_pager(source.selector(), pager.selector()).status;
        if (bound != STATUS_OK || (written != STATUS_OK && written != STATUS_BAD_ARGS)) return contract_error(__LINE__);
        const auto object = _cap_copy<ObjectInfo>(_object_info(source.selector())); if (!object) return object.error();
        if (written == STATUS_OK) {
            const auto records = _cap_copy<PagerRecords>(_pager_records(pager.selector(), object->object_id));
            if (!records || std::ranges::none_of(std::span{records->pages, records->count}, [](const auto& page) {
                return page.page == 1 && (page.flags & PAGER_DIRTY);
            })) return contract_error(__LINE__);
        }
        const auto dst = _memory_create(auth, 4096, VM_READ | VM_WRITE);
        if (dst.status != STATUS_OK) return dst.status;
        _cap_owner target{dst.value};
        if (_memory_populate(target.selector(), 0).status != STATUS_OK) return contract_error(__LINE__);
        if (trial % 3 == 0 && _memory_seal(target.selector()).status != STATUS_OK) return contract_error(__LINE__);
        const auto delegated = _cap_delegate(target.selector(), _startup().cspace, RIGHT_MAP | RIGHT_REVOKE | RIGHT_DUPLICATE);
        if (delegated.status != STATUS_OK) return delegated.status;
        _cap_owner branch{delegated.value};
        if (_pager_reclaim(pager.selector(), object->object_id, 0, branch.selector()).status != STATUS_PENDING) return contract_error(__LINE__);
        if (trial % 3 == 1 && _memory_seal(target.selector()).status != STATUS_OK) return contract_error(__LINE__);
        if (trial % 3 == 2 && _cap_revoke(branch.selector()).status != STATUS_OK) return contract_error(__LINE__);
        std::byte before{};
        if (_mem_read(target.selector(), 0, {&before, 1}).status != STATUS_OK) return contract_error(__LINE__);
        const auto due = _after(std::chrono::seconds{5}); if (!due) return contract_error(__LINE__);
        PagerReq request{};
        for (;;) {
            const auto claimed = _pager_claim(pager.selector());
            if (claimed.status == STATUS_OK) { request = *reinterpret_cast<const PagerReq*>(_ipc()); break; }
            if (claimed.status != STATUS_WOULD_BLOCK || _wait_one(pager.selector(), SOURCE_READY, 0, *due, 0, SOURCE_READ).status != STATUS_OK) return contract_error(__LINE__);
        }
        std::byte after{};
        if (_mem_read(target.selector(), 0, {&after, 1}).status != STATUS_OK || before != after
            || (request.kind != PAGER_REQUEST_RECLAIMED && request.kind != PAGER_REQUEST_WRITEBACK)
            || (trial % 3 == 0 && (after != std::byte{} || request.kind != PAGER_REQUEST_RECLAIMED))
            || _pager_complete(pager.selector(), request.id).status != STATUS_OK) return contract_error(__LINE__);
        if (trial % 3 == 0) {
            std::byte original{};
            const auto records = _cap_copy<PagerRecords>(_pager_records(pager.selector(), object->object_id));
            if (!records || !records->count || !(records->pages[0].flags & PAGER_DIRTY)
                || _mem_read(source.selector(), 0, {&original, 1}).status != STATUS_OK || original != data) return contract_error(__LINE__);
        }
        if (_scope_close(scope.selector()).status != STATUS_OK) return contract_error(__LINE__);
    }
    constexpr auto ok = "memory write/bind, sealed snapshots and destination revoke verified\n";
    return ::write(1, ok, ::strlen(ok)) == ssize_t(::strlen(ok)) ? STATUS_OK : STATUS_INTERNAL;
}
static auto session_check() -> status_t {
    ubus_conn environment, memory, admin;
    auto status = environment.bind(_startup().bootstrap); if (status != STATUS_OK) return status;
    for (const auto& [name, target] : std::array{std::pair{"memory", &memory}, std::pair{"memory.admin", &admin}}) {
        ubus_msg q, r; q.data.put("name", name);
        status = environment.invoke("Get", std::move(q), r); if (status != STATUS_OK) return status;
        status = target->bind(r.capability(r.data["connector"])); if (status != STATUS_OK) return status;
    }
    constexpr size_t bytes = 1024 * 1024;
    std::array<_vm_area, 2> areas;
    std::array<_cap_owner, 2> pagers, owners, scopes;
    std::array<uint64_t, 2> ids{};
    for (size_t i = 0; i < areas.size(); ++i) {
        const auto created = _scope_create(_startup().alloc()); if (created.status != STATUS_OK) return created.status;
        scopes[i] = _cap_owner{created.value};
        const auto budget = _quota_create({_startup().quota, scopes[i].selector()}, 4 * bytes, 32, OBJ_BIT(ObjPager) | OBJ_BIT(ObjScope));
        if (budget.status != STATUS_OK) return budget.status;
        _cap_owner allocation{budget.value};
        ubus_msg q, r;
        const auto quota = q.attach(allocation.selector(), RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        const auto scope = q.attach(scopes[i].selector(), RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (!quota || !scope) return STATUS_NO_MEMORY;
        q.data.cap("quota", *quota).cap("scope", *scope).put("logical", uint64_t{4 * bytes});
        status = memory.invoke("Open", std::move(q), r); if (status != STATUS_OK) return status;
        ids[i] = r.data["session"].as<uint64_t>().value_or(0); if (!ids[i]) return STATUS_INTERNAL;
        pagers[i] = r.take(r.data["pager"]);
        owners[i] = r.take(r.data["owner"]);
        auto arena = _vm(); arena.pager = pagers[i].selector();
        auto area = _vm_area::create(_startup().alloc(), arena, 0, bytes); if (!area) return area.error();
        areas[i] = std::move(*area);
        q = {}; r = {}; q.data.put("name", "resident").put("value", uint64_t{65536}).put("session", ids[i]);
        status = admin.invoke("Configure", std::move(q), r); if (status != STATUS_OK) return status;
        auto* data = reinterpret_cast<uint64_t*>(areas[i].address);
        for (size_t n = 0; n < bytes / sizeof(*data); ++n) data[n] = pattern(n, i + 1);
    }
    const auto held = _after(std::chrono::milliseconds{1500}); if (!held) return STATUS_INTERNAL;
    status = _ev_wait(&_events(), *held).status; if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    for (size_t i = 0; i < areas.size(); ++i) {
        auto* data = reinterpret_cast<uint64_t*>(areas[i].address);
        for (size_t n = 0; n < bytes / sizeof(*data); ++n) if (data[n] != pattern(n, i + 1)) return STATUS_INTERNAL;
        status = (i ? _object_destroy(owners[i].selector()) : _scope_close(scopes[i].selector())).status;
        if (status != STATUS_OK) return status;
        const auto due = _after(std::chrono::seconds{5}); if (!due) return STATUS_INTERNAL;
        for (;;) {
            ubus_msg r; status = admin.invoke("Status", {}, r); if (status != STATUS_OK) return status;
            if (std::ranges::none_of(r.data["sessions"].values(), [&](auto row) { return row["id"].template as<uint64_t>() == ids[i]; })) break;
            if (_clock_now().value >= *due) return STATUS_TIMED_OUT;
            const auto next = _after(std::chrono::milliseconds{10}); if (!next) return STATUS_INTERNAL;
            status = _ev_wait(&_events(), *next).status; if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
        }
    }
    status = _scope_close(scopes[1].selector()).status; if (status != STATUS_OK) return status;
    constexpr auto ok = "memory independent sessions, content and owner retirement verified\n";
    return ::write(1, ok, ::strlen(ok)) == ssize_t(::strlen(ok)) ? STATUS_OK : STATUS_INTERNAL;
}
int main(int argc, char** argv) {
    if (const auto s = px_init(); s != STATUS_OK) return s;
    if (argc == 2 && std::string_view{argv[1]} == "contracts") return contracts();
    if (argc == 2 && std::string_view{argv[1]} == "sessions") return session_check();
    const auto mb = argc > 1 ? libk::parse<size_t>(argv[1]).value_or(0) : 8;
    const auto hold = argc > 2 ? libk::parse<size_t>(argv[2]).value_or(0) : 1000;
    if (!mb || mb > 1024) return STATUS_BAD_ARGS;
    const auto count = mb * 1024 * 1024 / sizeof(uint64_t);
    auto data = std::make_unique_for_overwrite<uint64_t[]>(count);
    for (size_t pass = 1; pass <= 3; ++pass) {
        for (size_t i = 0; i < count; ++i) { if (pass > 1 && data[i] != pattern(i, pass-1)) return STATUS_INTERNAL; data[i] = pattern(i, pass); }
        if (pass == 1) {
            char line[80];
            const auto filled = libk::fmt::format_to_n(line, "memory filled bytes={}\n", mb * 1024 * 1024);
            if (!filled || ::write(1, line, filled.produced) != static_cast<ssize_t>(filled.produced)) return STATUS_INTERNAL;
        }
        const auto due = _after(std::chrono::milliseconds{hold}); if (!due) return STATUS_BAD_ARGS;
        while (_clock_now().value < *due) { const auto s = _ev_wait(&_events(), *due).status; if (s != STATUS_OK && s != STATUS_TIMED_OUT) return s; }
        for (size_t i = 0; i < count; ++i) if (data[i] != pattern(i, pass)) return STATUS_INTERNAL;
        char line[80];
        const auto result = libk::fmt::format_to_n(line, "memory pass={} bytes={} verified\n", pass, mb * 1024 * 1024);
        if (!result || ::write(1, line, result.produced) != static_cast<ssize_t>(result.produced)) return STATUS_INTERNAL;
    }
    return STATUS_OK;
}
