#include <ubus/ubus.hpp>
#include <vm.hpp>
#include <map>
#include <bit>
#include <tuple>

static constexpr size_t page_size = 4096, io_jobs = 8, read_jobs = 2;
struct mem_key {
    uint64_t session{}, object{}, page{};
    auto operator<=>(const mem_key&) const = default;
};
struct mem_backing { size_t volume{}, slot{}; uint64_t epoch{}; };
using mem_index = std::map<mem_key, mem_backing>;
struct mem_session {
    _cap_owner pager, scope;
    _ev_watch watch, owner;
    uint64_t object{}, resident{}, target{}, swap_limit{UINT64_MAX}, key{}, cursor{}, aliases{}, maps{}, sampled{};
    uint64_t generation{};
    uint64_t backed{}; // Index page count; updated only at insertion/erase.
    std::array<PagerPage, PAGER_RECORD_COUNT> pages{};
    uint32_t count{}, position{};
    uint64_t next_key{}, next_cursor{}, quota{};
    bool batch{}, closing{};
    uint64_t last_object{}, last_page{UINT64_MAX};
};
enum class mem_mode { Off, On, Paused, Draining, Closing, Failed };
struct mem_volume {
    std::string name;
    ubus_conn peer;
    ubus_batch<size_t, io_jobs> calls;
    std::string_view method;
    size_t buffer{};
    status_t reply{STATUS_PENDING};
    std::vector<uint64_t> free;
    uint64_t bytes{}, next{}, used{}, limit{}, offset{}, desired_limit{};
    int64_t priority{};
    size_t cursor{};
    mem_mode mode{mem_mode::Off};
    status_t error{};
    std::array<uint64_t, io_jobs> registrations{};
    auto allocate() -> size_t {
        for (size_t n = 0; n < free.size(); ++n) {
            const auto i = (cursor + n) % free.size();
            if (!free[i]) continue;
            const auto bit = std::countr_zero(free[i]); free[i] &= ~(uint64_t{1} << bit); cursor = i; ++used;
            return i * 64 + bit;
        }
        return 0;
    }
    void release(size_t slot) { free[slot / 64] |= uint64_t{1} << (slot % 64); --used; }
};
enum class mem_phase { Free, Snapshot, Submit, Io, Detach, Supply, Evict, Header, Flush };
struct mem_job {
    _cap_owner buffer;
    uint64_t reserve{}, registration{}, request{}, epoch{}, started{};
    mem_key key;
    size_t volume{}, slot{}, destination{}, destination_slot{};
    bool read{}, format{}, flushed{}, migrating{}, inplace{}, background{}, restore{};
    status_t result{};
    mem_phase phase{mem_phase::Free};
};
struct mem_header { uint64_t magic{0x31504157534f594dULL}; uint32_t version{1}, page{page_size}; uint64_t bytes{}, identity{}; };
enum class mem_step { None, Lookup, Bind, Device, Open, Adopt, Stat, Prepare, Header, HeaderIo, Register, Drain };
struct mem_control {
    uint64_t id{}, due{}; status_t status{STATUS_OK}; bool format{}, enable{}; size_t volume{};
    mem_step step{};
    ubus_data options{};
    size_t index{};
    bool inventory{};
};
static ubus_conn parent, directory;
static ubus_object memory{"memory"}, admin{"memory.admin"};
static std::map<size_t, mem_session> sessions;
static std::vector<std::unique_ptr<mem_volume>> volumes;
static std::array<mem_job, io_jobs> jobs;
static mem_index backing;
static std::vector<mem_index::node_type> nodes;
static std::array<std::optional<PagerReq>, 64> waiting;
static std::array<size_t, 64> waiting_session;
static std::array<uint64_t, 64> waiting_due, waiting_quota;
static std::array<bool, 64> waiting_background;
static _cap_owner physical, ownership;
static _ev_watch pressure;
static mem_control control;
static uint64_t low = 8 * 1024 * 1024, high = 16 * 1024 * 1024, floor_bytes = 256 * 1024;
static uint64_t reads{}, writes{}, released{}, scanned{}, refaults{}, copied{}, failures{}, latency_total{}, latency_max{};
static uint64_t next_operation{};
static bool sweep_needed{}, slots_needed{}, serving{true};
static size_t write_depth = io_jobs - read_jobs, scan_session{}, volume_cursor{};
static mem_key sweep_cursor{};
static std::optional<mem_key> prefetch, restoring;
static std::array<std::pair<mem_key, uint64_t>, 64> hot;
static size_t hot_cursor{}, read_ahead{};
static uint64_t prefetch_due{};

static auto now() -> uint64_t { return _clock_now().value; }
static auto deadline(uint64_t ms) -> uint64_t { return _after(std::chrono::milliseconds{ms}).value_or(0); }
static auto session(size_t id) -> mem_session& { return sessions.find(id)->second; }
static auto cap(size_t id) -> cap_t { return session(id).pager.selector(); }
static auto mode(mem_mode state) -> std::string_view {
    switch (state) {
    case mem_mode::Off: return "Off"; case mem_mode::On: return "On"; case mem_mode::Paused: return "Paused";
    case mem_mode::Draining: return "Draining"; case mem_mode::Closing: return "Closing"; case mem_mode::Failed: return "Failed";
    }
    return "Failed";
}
static auto busy(mem_key key) -> bool {
    return std::ranges::any_of(jobs, [&](const auto& j) { return j.phase != mem_phase::Free && j.key == key; });
}
static auto swap_used(size_t si) -> uint64_t {
    uint64_t pages = session(si).backed;
    // Accepted snapshots already commit backing capacity, before I/O publishes the index.
    pages += std::ranges::count_if(jobs, [=](const auto& j) {
        return j.phase != mem_phase::Free && j.key.session == si && j.slot && !j.read && !backing.contains(j.key);
    });
    return pages * page_size;
}
static void erase(mem_index::iterator it) {
    if (control.status == STATUS_PENDING && volumes[it->second.volume]->mode == mem_mode::Draining) control.due = deadline(10000);
    --session(it->first.session).backed;
    volumes[it->second.volume]->release(it->second.slot); nodes.push_back(backing.extract(it));
}
static auto place(mem_session& s, cap_t from, cap_t to) -> status_t {
    const auto info = _cap_copy<ObjectInfo>(_object_info(s.pager.selector())); if (!info) return info.error();
    const auto member = _cap_copy<ScopeObject>(_scope_query(from, info->object_id, SCOPE_INFO));
    if (!member) return _scope_query(to, info->object_id, SCOPE_INFO).status;
    return _scope_adopt(s.pager.selector(), from, to, member->owner_epoch).status;
}
static auto add_session(_cap_owner pager, _cap_owner scope, uint64_t logical) -> std::expected<size_t, status_t> {
    const auto view = _cap_copy<CapInfo>(_cap_info(pager.selector()));
    constexpr auto rights = RIGHT_SERVE | RIGHT_SUPPLY | RIGHT_MANAGE | RIGHT_INSPECT | RIGHT_ADOPT | RIGHT_DESTROY;
    if (!view || view->view.kind != ObjPager || (view->view.rights & rights) != rights) return std::unexpected(STATUS_BAD_RIGHTS);
    if (_object_info(pager.selector()).status != STATUS_OK) return std::unexpected(STATUS_INVALID_CAP);
    const auto object = reinterpret_cast<const ObjectInfo*>(_ipc())->object_id;
    if (std::ranges::any_of(sessions, [=](const auto& row) { return row.second.object == object; })) return std::unexpected(STATUS_ALREADY_CONNECTED);
    mem_session s; s.object = object; s.pager = std::move(pager); s.scope = std::move(scope);
    auto status = _pager_configure(s.pager.selector(), true, logical).status;
    if (status == STATUS_OK) status = s.watch.open(&_events(), s.pager.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, WATCH_CHANGED, SOURCE_READ);
    if (status == STATUS_OK) status = s.owner.open(&_events(), s.scope.selector(), STATE_CLOSING | STATE_DRAINED);
    if (status == STATUS_OK) status = place(s, s.scope.selector(), ownership.selector());
    if (status != STATUS_OK) return std::unexpected(status);
    // The default session owns the staging pool; other IDs are native Pager IDs.
    const size_t id = sessions.empty() ? 0 : object;
    sessions.emplace(id, std::move(s)); return id;
}
static auto select_volume(size_t exclude = SIZE_MAX) -> size_t {
    size_t best = SIZE_MAX;
    for (size_t n = 0; n < volumes.size(); ++n) {
        const auto i = (volume_cursor + n) % volumes.size(); const auto& v = *volumes[i];
        if (i != exclude && v.mode == mem_mode::On && v.used < v.limit && (best == SIZE_MAX || v.priority > volumes[best]->priority)) best = i;
    }
    if (best != SIZE_MAX) volume_cursor = best + 1;
    return best;
}
static auto prepare_index(size_t required) -> status_t {
    if (parent.active()) {
        ubus_msg r; auto status = parent.poll(r);
        if (status == STATUS_WOULD_BLOCK) return STATUS_PENDING;
        if (status == STATUS_OK) status = r.status;
        if (status != STATUS_OK) return status;
    }
    const auto count = required - std::min(required, nodes.size() + backing.size());
    if (!count) return STATUS_OK;
    if (_quota_info(_startup().quota).status != STATUS_OK) return STATUS_NO_MEMORY;
    const auto quota = *reinterpret_cast<const QuotaInfo*>(_ipc());
    if (_memory_info(physical.selector()).status != STATUS_OK) return STATUS_NO_MEMORY;
    const auto ram = *reinterpret_cast<const MemoryInfo*>(_ipc());
    // Include allocator/node metadata and the peak handle-vector replacement.
    // These bounds only guard preparation; actual committed pages stay charged.
    constexpr auto per_node = sizeof(mem_index::value_type) * 2 + 128;
    const auto vector_bytes = nodes.capacity() < required ? required * sizeof(mem_index::node_type) : 0;
    if (count > (UINT64_MAX - vector_bytes - required / 8) / per_node) return STATUS_BAD_ARGS;
    const auto needed = count * per_node + vector_bytes + required / 8;
    if (ram.free < floor_bytes || needed > ram.free - floor_bytes) return STATUS_NO_MEMORY;
    if (needed > quota.memory_available) {
        if (needed > UINT64_MAX - (quota.memory_limit - quota.memory_available)) return STATUS_BAD_ARGS;
        ubus_msg q; q.data.put("action", "resize").put("bytes", quota.memory_limit - quota.memory_available + needed).put("caps", quota.cap_limit);
        q.deadline = deadline(5000);
        const auto status = parent.begin("Resource", std::move(q), true);
        return status == STATUS_OK ? STATUS_PENDING : status;
    }
    nodes.reserve(nodes.size() + backing.size() + count);
    mem_index prepaid;
    for (size_t i = 0; i < count; ++i) prepaid.emplace(mem_key{UINT64_MAX, 0, i}, mem_backing{});
    while (!prepaid.empty()) nodes.push_back(prepaid.extract(prepaid.begin()));
    return STATUS_OK;
}
// One serialized device control call per volume, independent of data I/O.
// Every caller can retire a preceding completion, so abandoned jobs cannot
// strand a late Register or prevent its eventual Unregister during drain.
static auto command(mem_volume& v, std::string_view method = {}, size_t index = 0) -> status_t {
    // A slow control call on another buffer must not gate registered data I/O.
    if (method == "Register" && v.registrations[index] && (v.method.empty() || (v.method != "Close" && v.buffer != index))) return STATUS_OK;
    if (!v.method.empty()) {
        if (v.reply == STATUS_PENDING) {
            if (!v.peer.pending()) return STATUS_PENDING;
            ubus_msg r; auto status = v.peer.poll(r);
            if (status == STATUS_WOULD_BLOCK) return STATUS_PENDING;
            if (status == STATUS_OK) status = r.status;
            if (status == STATUS_PENDING) status = STATUS_PEER_FAULT;
            if (status == STATUS_OK && v.method == "Register") {
                const auto id = r.data["registration"].as<uint64_t>().value_or(0);
                if (!id) status = STATUS_PEER_FAULT;
                else v.registrations[v.buffer] = id;
            } else if (status == STATUS_OK && v.method == "Unregister") {
                v.registrations[v.buffer] = 0;
            }
            v.reply = status;
        }
        if (method.empty()) return v.reply;
        const auto previous = std::exchange(v.method, {});
        if (v.reply != STATUS_OK || (previous == method && v.buffer == index)) return v.reply;
        return STATUS_RETRY; // Re-evaluate leases after another command changed them.
    }
    if (method.empty()) return STATUS_OK;
    if (method == "Register" && v.registrations[index]) return STATUS_OK;
    if (method == "Unregister" && !v.registrations[index]) return STATUS_OK;
    ubus_msg q; q.deadline = deadline(1000);
    if (method == "Register") {
        const auto c = q.attach(jobs[index].buffer.selector(), RIGHT_MAP | RIGHT_DUPLICATE);
        if (!c) return c.error();
        q.data.put("buffer", ubus_buffer{*c, 0, page_size}).put("perms", uint64_t{index < read_jobs ? VM_READ | VM_WRITE : VM_READ});
    } else if (method == "Unregister") q.data.put("registration", v.registrations[index]);
    // Once accepted, control effects must retire with their real reply or
    // provider closure; a local timeout cannot prove that DMA has exited.
    const auto status = v.peer.begin(method, std::move(q), true);
    if (status != STATUS_OK) return status;
    if (method == "Close") v.mode = mem_mode::Closing;
    v.method = method; v.buffer = index; v.reply = STATUS_PENDING; return STATUS_PENDING;
}
static auto detach(mem_volume& v, size_t index) -> status_t {
    auto& j = jobs[index];
    auto status = command(v, "Unregister", index);
    if (status == STATUS_OK) {
        if (j.phase != mem_phase::Free && volumes[j.volume].get() == &v) j.registration = 0;
        return STATUS_OK;
    }
    if (status == STATUS_PENDING || status == STATUS_BUSY || status == STATUS_RETRY) return status;
    if ((j.phase == mem_phase::Io || j.phase == mem_phase::Detach) && volumes[j.volume].get() != &v) return STATUS_BUSY;
    // An unreachable provider cannot acknowledge Unregister. Remove the other
    // idle mappings, then require native CPU/DMA quiescence for this buffer.
    for (auto& other : volumes) if (other.get() != &v && other->registrations[index]) {
        status = command(*other, "Unregister", index);
        if (status == STATUS_PENDING || status == STATUS_BUSY || status == STATUS_RETRY) return status;
    }
    status = _pager_buffer(cap(0), 1, j.reserve).status;
    if (status != STATUS_OK) return status;
    for (auto& other : volumes) other->registrations[index] = 0;
    j.registration = 0; return STATUS_OK;
}
static auto submit(size_t index) -> status_t {
    auto& j = jobs[index]; auto& v = *volumes[j.volume];
    if (j.phase != mem_phase::Flush) {
        const auto status = command(v, "Register", index); if (status != STATUS_OK) return status;
        j.registration = v.registrations[index];
    }
    if (v.next == UINT64_MAX) return STATUS_EXHAUSTED;
    const auto id = ++v.next; const auto flush = j.phase == mem_phase::Flush;
    const auto status = v.calls.submit(id, index, v.peer, flush ? "Flush" : j.read ? "Read" : "Write", [&](size_t, auto&, ubus_msg& q) {
        q.deadline = deadline(5000); q.data.put("id", id);
        if (!flush) q.data.put("offset", j.slot * page_size).put("registration", j.registration)
            .put("buffer_offset", uint64_t{0}).put("bytes", uint64_t{page_size});
        return STATUS_OK;
    });
    if (status == STATUS_OK) { j.phase = mem_phase::Io; j.flushed |= flush; }
    return status;
}
static void finish_job(mem_job& j) {
    const auto elapsed = now() - j.started; latency_total += elapsed; latency_max = std::max(latency_max, elapsed);
    j.phase = mem_phase::Free; j.registration = j.request = 0; j.key = {}; j.slot = 0; j.format = j.flushed = j.migrating = j.inplace = j.background = j.restore = false;
}
static void fail_job(mem_job& j, status_t error, bool media = true) {
    ++failures; auto& v = *volumes[j.volume]; v.error = error; if (media || !j.slot) v.mode = mem_mode::Failed;
    if (j.request) (void)_pager_fail(cap(j.key.session), j.request, j.background && !media ? PAGER_FAIL_RETRY : 0);
    if (j.slot && !j.read && !j.inplace) v.release(j.slot);
    if (j.inplace && media) { const auto old = backing.find(j.key); if (old != backing.end()) erase(old); }
    if (j.migrating) volumes[j.destination]->release(j.destination_slot);
    if (control.status == STATUS_PENDING) {
        const auto name = control.options["volume"].as<std::string_view>();
        if (!name || v.name == *name) control.status = error;
    }
    finish_job(j);
}
static void complete_io(size_t index, status_t status) {
    auto& j = jobs[index]; auto& v = *volumes[j.volume];
    if (status == STATUS_PENDING) status = STATUS_PEER_FAULT;
    if (status != STATUS_OK) {
        v.error = status; v.mode = mem_mode::Failed;
    }
    if (j.registration && status != STATUS_OK) {
        const auto detached = detach(v, index);
        if (detached != STATUS_OK) {
            v.error = detached;
            j.result = status; j.phase = mem_phase::Detach; return;
        }
    }
    j.registration = 0;
    if (status != STATUS_OK) { fail_job(j, status); return; }
    if (!j.slot) {
        if (j.format) {
            if (!j.flushed) { j.phase = mem_phase::Flush; return; }
            v.mode = mem_mode::Draining; control.step = mem_step::Drain;
            finish_job(j); return;
        }
        mem_header header{};
        status = _mem_read(j.buffer.selector(), 0, std::as_writable_bytes(std::span{&header, 1})).status;
        if (status != STATUS_OK || header.magic != mem_header{}.magic || header.version != 1 || header.page != page_size || header.bytes != v.bytes) {
            fail_job(j, STATUS_BAD_ARGS); return;
        }
        control.step = mem_step::Prepare;
        finish_job(j); return;
    }
    if (j.read && j.migrating) {
        ++reads; j.volume = j.destination; j.slot = j.destination_slot;
        j.read = j.migrating = false; j.phase = mem_phase::Submit;
    } else if (j.read) { ++reads; ++refaults; j.phase = mem_phase::Supply; }
    else {
        ++writes;
        if (j.inplace) {
            const auto old = backing.find(j.key); if (old == backing.end()) _thread_exit(STATUS_INTERNAL);
            old->second.epoch = j.epoch;
        } else {
            if (const auto old = backing.find(j.key); old != backing.end()) erase(old);
            if (nodes.empty()) _thread_exit(STATUS_INTERNAL);
            auto node = std::move(nodes.back()); nodes.pop_back(); node.key() = j.key; node.mapped() = {j.volume, j.slot, j.epoch};
            backing.insert(std::move(node));
            ++session(j.key.session).backed;
        }
        const auto completed = j.request ? _pager_complete(cap(j.key.session), j.request) : _sys_result{.status=STATUS_OK};
        if (completed.status == STATUS_RETRY || completed.status == STATUS_CLOSED) erase(backing.find(j.key));
        j.registration = 0;
        if (j.request && completed.status == STATUS_OK && !(completed.value & PAGER_DIRTY) && v.mode == mem_mode::On) {
            j.request = 0; j.slot = 0; j.phase = mem_phase::Evict;
        } else finish_job(j);
    }
}
static void pump_io() {
    if (control.status == STATUS_PENDING && control.step == mem_step::Header && jobs[0].phase == mem_phase::Free) {
        control.step = mem_step::HeaderIo;
        auto& j = jobs[0]; j.volume = control.volume; j.read = !control.format; j.format = control.format;
        j.slot = 0; j.started = now(); j.phase = mem_phase::Header;
        if (j.format) {
            const mem_header header{.bytes=volumes[j.volume]->bytes, .identity=now()};
            std::array<std::byte, page_size> data{}; ::memcpy(data.data(), &header, sizeof(header));
            const auto written = _mem_write(j.buffer.selector(), 0, data);
            if (written.status != STATUS_OK) fail_job(j, written.status);
        }
    }
    for (size_t vi = 0; vi < volumes.size(); ++vi) {
        auto& v = *volumes[vi]; size_t index; ubus_msg response;
        for (size_t n = 0; n < io_jobs && v.calls.completion(index, response) == STATUS_OK; ++n) {
            auto& j = jobs[index];
            auto status = response.status;
            if (status == STATUS_OK && j.phase == mem_phase::Io && (j.slot || control.status == STATUS_PENDING)) {
                const auto count = response.data["bytes"].as<uint64_t>();
                if (!count || (*count != page_size && !(j.format && j.flushed && *count == 0))) status = STATUS_PEER_FAULT;
            }
            complete_io(index, status);
        }
    }
    for (size_t index = 0; index < jobs.size(); ++index) {
        auto& j = jobs[index];
        if (j.phase == mem_phase::Evict) {
            if (volumes[j.volume]->mode != mem_mode::On || _pager_reclaim(cap(j.key.session), j.key.object, j.key.page).status != STATUS_BUSY) finish_job(j);
        } else if (j.phase == mem_phase::Detach) { complete_io(index, j.result); }
        else if (j.phase == mem_phase::Supply) {
            const auto status = _pager_supply(cap(j.key.session), j.request, j.buffer.selector(), 0, PAGER_SUPPLY_COPY | (j.background ? PAGER_SUPPLY_NOWAIT : 0)).status;
            if (status == STATUS_NO_MEMORY && j.background) {
                (void)_pager_fail(cap(j.key.session), j.request, PAGER_FAIL_RETRY);
                const bool restore = j.restore; finish_job(j);
                if (!restore) prefetch_due = deadline(100);
                if (restore && control.status == STATUS_PENDING && control.step == mem_step::Drain) {
                    const auto name = control.options["volume"].as<std::string_view>();
                    if (!name || volumes[j.volume]->name == *name) control.status = STATUS_NO_MEMORY;
                }
            } else if (status == STATUS_OK || status == STATUS_RETRY || status == STATUS_CLOSED) {
                if (status != STATUS_OK && _pager_records(cap(j.key.session), j.key.object).status == STATUS_NOT_FOUND) {
                    const auto old = backing.find(j.key); if (old != backing.end()) erase(old);
                }
                if (status == STATUS_OK) {
                    hot[hot_cursor++ % hot.size()] = {j.key, deadline(100)};
                    auto& s = session(j.key.session); const mem_key next{j.key.session, j.key.object, j.key.page + 1};
                    if (read_ahead && !prefetch && !restoring && now() >= prefetch_due && jobs[1].phase == mem_phase::Free && s.last_object == j.key.object && j.key.page == s.last_page + 1
                        && backing.contains(next) && volumes[j.volume]->mode == mem_mode::On
                        && _pager_restore(cap(next.session), next.object, next.page).status == STATUS_PENDING) prefetch = next;
                    s.last_object = j.key.object; s.last_page = j.key.page;
                }
                finish_job(j);
            } else if ((status != STATUS_BUSY && status != STATUS_NO_MEMORY) || now() - j.started > _startup().frequency * 5) {
                (void)_pager_fail(cap(j.key.session), j.request, j.background ? PAGER_FAIL_RETRY : 0); ++failures; finish_job(j);
            }
        } else if (j.phase == mem_phase::Submit || j.phase == mem_phase::Header || j.phase == mem_phase::Flush) {
            const auto status = submit(index);
            // An accepted header registration still owns its real completion.
            // Demand jobs may time out; their volume retains any late lease.
            if (status == STATUS_PENDING && !j.slot) continue;
            if (status != STATUS_OK && status != STATUS_PENDING && status != STATUS_BUSY && status != STATUS_NO_MEMORY && status != STATUS_RETRY) fail_job(j, status);
            else if (status != STATUS_OK && now() - j.started > _startup().frequency * 5) fail_job(j, STATUS_TIMED_OUT, false);
        }
    }
}
static auto queue_read(size_t si, const PagerReq& req) -> bool {
    const mem_key key{si, req.key, req.page_index};
    if (busy(key)) return false;
    const bool speculative = prefetch && *prefetch == key;
    const bool background = speculative || (restoring && *restoring == key);
    const auto saved = backing.find(key);
    if (saved != backing.end() && volumes[saved->second.volume]->mode == mem_mode::Failed) { (void)_pager_fail(cap(si), req.id); return true; }
    if (saved == backing.end()) {
        const auto status = _pager_complete(cap(si), req.id).status;
        if (status == STATUS_OK || status == STATUS_RETRY || status == STATUS_CLOSED) {
            if (speculative) prefetch.reset();
            if (restoring && *restoring == key) restoring.reset();
            return true;
        }
        return false;
    }
    for (size_t i = background ? 1 : 0; i < read_jobs; ++i) if (jobs[i].phase == mem_phase::Free
        && (i || control.status != STATUS_PENDING || (control.step != mem_step::Header && control.step != mem_step::HeaderIo))) {
        auto& j = jobs[i]; j.key = key; j.request = req.id; j.read = true; j.background = background; j.restore = restoring && *restoring == key;
        j.volume = saved->second.volume; j.slot = saved->second.slot; j.started = now(); j.phase = mem_phase::Submit;
        if (speculative) prefetch.reset();
        if (restoring && *restoring == key) restoring.reset();
        return true;
    }
    return false;
}
static auto pump_pagers() -> status_t {
    for (auto& [si, entry] : sessions) {
        auto& s = entry;
        if (s.closing) { sweep_needed = true; continue; }
        bool closing{};
        if (s.owner.pending()) {
            const auto facts = _source_info(s.scope.selector());
            closing = facts.status != STATUS_OK || !(facts.value & STATE_OPEN);
            if (!closing) { s.owner.ack(); (void)s.owner.arm(facts.value2); }
        }
        if (s.watch.pending()) {
            const auto result = _pager_configure(cap(si));
            if (result.status != STATUS_OK) closing = true;
            else {
                const auto info = *reinterpret_cast<const PagerInfo*>(_ipc());
                if (info.generation != s.generation) { s.generation = info.generation; sweep_needed = true; }
            }
        }
        if (closing) {
            if (!si) return STATUS_CLOSED;
            const auto closed = _object_destroy(cap(si)).status;
            if (closed != STATUS_OK && closed != STATUS_CLOSED) return closed;
            s.closing = sweep_needed = true;
            for (auto* watch : {&s.owner, &s.watch}) { const auto status = watch->close(); if (status != STATUS_OK) return status; }
            continue;
        }
        bool drained{};
        for (size_t n = 0; n < 32; ++n) {
            const auto empty = std::ranges::find_if(waiting, [](const auto& r) { return !r; });
            if (empty == waiting.end()) { s.watch.ack(); break; }
            const auto result = _pager_claim(cap(si));
            if (result.status == STATUS_WOULD_BLOCK) { drained = true; break; }
            if (result.status != STATUS_OK) break;
            const auto r = *reinterpret_cast<const PagerReq*>(_ipc());
            if (r.kind == PAGER_REQUEST_RECLAIMED) {
                if (status_t(r.payload.reclaimed.status) == STATUS_OK) {
                    released += r.payload.reclaimed.pages;
                    const auto quota = _pager_records(cap(si), r.key).status == STATUS_OK ? reinterpret_cast<const PagerRecords*>(_ipc())->quota : 0;
                    for (size_t i = 0; i < waiting.size(); ++i) if (waiting[i] && waiting[i]->kind == PAGER_REQUEST_CAPACITY
                        && (!waiting_quota[i] || waiting_quota[i] == quota)) {
                        (void)_pager_complete(cap(waiting_session[i]), waiting[i]->id); waiting[i].reset();
                    }
                }
                (void)_pager_complete(cap(si), r.id);
                for (auto& j : jobs) if (j.phase == mem_phase::Snapshot && j.key == mem_key{si, r.key, r.page_index}) {
                    if (!j.inplace) volumes[j.volume]->release(j.slot);
                    finish_job(j);
                }
            } else if (r.kind == PAGER_REQUEST_WRITEBACK) {
                auto found = std::ranges::find_if(jobs, [&](const auto& j) { return j.phase == mem_phase::Snapshot && j.key == mem_key{si, r.key, r.page_index}; });
                if (found == jobs.end()) { (void)_pager_fail(cap(si), r.id); continue; }
                found->request = r.id; found->epoch = r.payload.writeback.dirty_epoch; found->phase = mem_phase::Submit; copied += page_size;
            } else if (r.kind == PAGER_REQUEST_CAPACITY || !queue_read(si, r)) {
                const auto i = size_t(empty - waiting.begin()); waiting[i] = r; waiting_session[i] = si;
                const mem_key key{si, r.key, r.page_index};
                waiting_background[i] = r.kind == PAGER_REQUEST_PAGE_IN && ((prefetch && *prefetch == key) || (restoring && *restoring == key));
                waiting_quota[i] = r.kind == PAGER_REQUEST_CAPACITY ? r.payload.capacity.quota : 0;
                waiting_due[i] = deadline(r.kind == PAGER_REQUEST_CAPACITY ? 500 : 5000);
            }
        }
        if (drained) {
            s.watch.ack(); const auto seen = _source_info(cap(si), SOURCE_READ);
            if (seen.status == STATUS_OK) (void)s.watch.arm(seen.value2);
        }
    }
    for (size_t i = 0; i < waiting.size(); ++i) if (waiting[i]) {
        if (session(waiting_session[i]).closing) waiting[i].reset();
        else if (waiting[i]->kind != PAGER_REQUEST_CAPACITY && queue_read(waiting_session[i], *waiting[i])) waiting[i].reset();
        else if (now() >= waiting_due[i]) {
            (void)_pager_fail(cap(waiting_session[i]), waiting[i]->id, waiting_background[i] ? PAGER_FAIL_RETRY : 0);
            const mem_key key{waiting_session[i], waiting[i]->key, waiting[i]->page_index};
            for (auto* pending : {&prefetch, &restoring}) if (*pending && **pending == key) pending->reset();
            waiting[i].reset(); ++failures;
        }
    }
    for (auto& j : jobs) if (j.phase == mem_phase::Snapshot && (session(j.key.session).closing
        || _pager_records(cap(j.key.session), j.key.object).status == STATUS_NOT_FOUND)) {
        if (!j.inplace) volumes[j.volume]->release(j.slot);
        else { const auto old = backing.find(j.key); if (old != backing.end()) erase(old); }
        finish_job(j);
    }
    for (auto* pending : {&prefetch, &restoring}) if (*pending && (session((*pending)->session).closing
        || _pager_records(cap((*pending)->session), (*pending)->object).status == STATUS_NOT_FOUND)) pending->reset();
    return STATUS_OK;
}
static auto victim(mem_key key, uint32_t flags, uint64_t stamp, bool recycle) -> status_t {
    if (!(flags & PAGER_RESIDENT) || (flags & (PAGER_BUSY | PAGER_PINNED | PAGER_ACCESSED)) || busy(key)) return STATUS_OK;
    if (std::ranges::any_of(hot, [&](const auto& h) { return h.first == key && h.second > stamp; })) return STATUS_OK;
    if (!(flags & PAGER_DIRTY)) return _pager_reclaim(cap(key.session), key.object, key.page).status == STATUS_BUSY ? STATUS_BUSY : STATUS_OK;
    const auto saved = backing.find(key);
    const bool inplace = saved != backing.end() && volumes[saved->second.volume]->mode == mem_mode::On;
    if (!inplace && nodes.empty()) { slots_needed |= recycle; return STATUS_OK; }
    const auto vi = inplace ? saved->second.volume : select_volume(); if (vi == SIZE_MAX) { slots_needed |= recycle; return STATUS_OK; }
    auto& volume = *volumes[vi]; auto& s = session(key.session);
    if (!inplace && s.swap_limit != UINT64_MAX && !backing.contains(key) && swap_used(key.session) >= s.swap_limit) {
        slots_needed |= recycle; return STATUS_OK;
    }
    for (size_t i = read_jobs; i < read_jobs + write_depth; ++i) if (jobs[i].phase == mem_phase::Free) {
        auto& j = jobs[i]; const auto slot = inplace ? saved->second.slot : volume.allocate(); if (!slot) return STATUS_OK;
        j.inplace = inplace;
        j.key = key; j.volume = vi; j.slot = slot; j.read = false; j.started = now(); j.phase = mem_phase::Snapshot;
        const auto status = _pager_reclaim(cap(key.session), key.object, key.page, j.buffer.selector(), 0).status;
        if (status == STATUS_PENDING) return STATUS_OK;
        if (!inplace) volume.release(slot);
        finish_job(j); return status == STATUS_BUSY ? STATUS_BUSY : STATUS_OK;
    }
    return STATUS_BUSY;
}
static auto scan(bool global) -> bool {
    if (sessions.empty()) return false;
    auto next = sessions.upper_bound(scan_session); if (next == sessions.end()) next = sessions.begin();
    const auto si = scan_session = next->first; auto& s = next->second;
    if (s.closing) return true;
    bool capacity{};
    for (size_t i = 0; i < waiting.size(); ++i) capacity |= waiting[i] && waiting_session[i] == si && waiting[i]->kind == PAGER_REQUEST_CAPACITY;
    const bool reclaim = global || capacity || (s.target && s.resident > s.target);
    if (!s.batch) {
        const auto status = _pager_records(cap(si), s.key, s.cursor, s.aliases, reclaim, s.maps).status;
        if (status != STATUS_OK) { s.key = s.cursor = s.aliases = s.maps = 0; s.resident = s.sampled; s.sampled = 0; return false; }
        const auto rows = *reinterpret_cast<const PagerRecords*>(_ipc());
        if (!s.aliases) { s.pages = {}; s.count = rows.count; s.position = 0; }
        for (size_t i = 0; i < rows.count; ++i) { s.pages[i].page = rows.pages[i].page; s.pages[i].flags |= rows.pages[i].flags; }
        s.key = rows.key; s.aliases = rows.alias_cursor; s.maps = rows.map_cursor;
        if (rows.state & PAGER_PARTIAL) return true;
        s.next_key = rows.next_key; s.next_cursor = rows.cursor; s.quota = rows.quota; s.batch = true;
    }
    const auto stamp = now();
    for (; s.position < s.count; ++s.position) {
        const auto& page = s.pages[s.position];
        bool eligible = !capacity;
        for (size_t i = 0; i < waiting.size() && !eligible; ++i)
            eligible = waiting[i] && waiting_session[i] == si && waiting[i]->kind == PAGER_REQUEST_CAPACITY
                && (!waiting_quota[i] || waiting_quota[i] == s.quota);
        if (reclaim && eligible && victim({si, s.key, page.page}, page.flags, stamp, global || capacity) == STATUS_BUSY) return false;
        ++scanned; if (page.flags & PAGER_RESIDENT) s.sampled += page_size;
    }
    s.batch = false; s.cursor = s.next_cursor;
    if (!s.cursor) {
        s.key = s.next_key;
        if (!s.key) { s.resident = s.sampled; s.sampled = 0; return false; }
    }
    return true;
}
static void sweep() {
    // The binding registry is authoritative. No user index keeps a client Mem alive.
    for (size_t n = 0; n < 16; ++n) {
        auto it = backing.lower_bound(sweep_cursor);
        if (it == backing.end()) { sweep_cursor = {}; sweep_needed = slots_needed = false; return; }
        const auto key = it->first; sweep_cursor = {key.session, key.object, key.page + 1};
        if (busy(key)) continue;
        const auto status = _pager_records(cap(key.session), key.object, key.page).status;
        if (session(key.session).closing || status == STATUS_NOT_FOUND) { erase(it); continue; }
        auto& volume = *volumes[it->second.volume];
        const auto rows = *reinterpret_cast<const PagerRecords*>(_ipc());
        // Retained backing is a cache while the frame is resident. Release it
        // under slot pressure only after the kernel makes the frame dirty.
        if (status == STATUS_OK && slots_needed && volume.mode == mem_mode::On
            && rows.count && rows.pages[0].page == key.page && (rows.pages[0].flags & PAGER_RESIDENT)
            && _pager_dirty(cap(key.session), key.object, key.page).status == STATUS_OK) {
            erase(it); slots_needed = false; continue;
        }
        if (status != STATUS_OK || volume.mode != mem_mode::Draining
            || (volume.desired_limit && it->second.slot <= volume.desired_limit)) continue;
        if (rows.count && rows.pages[0].page == key.page && (rows.pages[0].flags & PAGER_RESIDENT)) {
            if (_pager_dirty(cap(key.session), key.object, key.page).status == STATUS_OK) erase(it);
        } else {
            if (jobs[1].phase != mem_phase::Free || restoring || prefetch) continue;
            const auto destination = select_volume(it->second.volume);
            if (destination != SIZE_MAX) {
                auto& j = jobs[1]; j.key = key; j.request = 0; j.epoch = it->second.epoch;
                j.volume = it->second.volume; j.slot = it->second.slot; j.read = j.migrating = true;
                j.destination = destination; j.destination_slot = volumes[destination]->allocate();
                j.started = now(); j.phase = mem_phase::Submit;
            } else if (_pager_restore(cap(key.session), key.object, key.page).status == STATUS_PENDING) restoring = key;
        }
    }
}
static auto active() -> bool {
    return std::ranges::any_of(jobs, [](const auto& j) { return j.phase != mem_phase::Free; });
}
static void drain() {
    const bool running = control.status == STATUS_PENDING && control.step == mem_step::Drain;
    const auto name = running ? control.options["volume"].as<std::string_view>() : std::nullopt;
    bool remaining{}, stalled{};
    for (size_t vi = 0; vi < volumes.size(); ++vi) {
        auto& v = *volumes[vi];
        const bool selected = running && (!name || v.name == *name);
        // Closing is irreversible and survives the control operation's exit.
        if (v.mode != mem_mode::Closing) {
            if (v.mode != mem_mode::Draining) continue;
            if (!selected) { v.mode = mem_mode::Paused; continue; }
            const bool external = v.desired_limit ? std::ranges::any_of(backing, [&](const auto& row) { return row.second.volume == vi && row.second.slot > v.desired_limit; }) : v.used != 0;
            if (external || std::ranges::any_of(jobs, [=](const auto& j) { return j.phase != mem_phase::Free && (j.volume == vi || (j.migrating && j.destination == vi)); })) { remaining = stalled = true; continue; }
            if (v.desired_limit) {
                v.limit = std::exchange(v.desired_limit, 0); v.free.resize((v.limit + 64) / 64);
                const auto end = (v.limit + 1) % 64; if (end) v.free.back() &= (uint64_t{1} << end) - 1;
                v.cursor = 0; v.mode = mem_mode::On; continue;
            }
            for (size_t i = 0; i < io_jobs; ++i) if (v.registrations[i]) {
                const auto status = detach(v, i);
                if (status == STATUS_PENDING || status == STATUS_BUSY || status == STATUS_RETRY) { remaining = true; break; }
                if (status != STATUS_OK) { v.error = status; control.status = status; v.mode = mem_mode::Paused; return; }
            }
            if (std::ranges::any_of(v.registrations, [](auto r) { return r != 0; })) continue;
        }
        const auto status = command(v, "Close");
        if (status == STATUS_PENDING || status == STATUS_BUSY || status == STATUS_RETRY) { remaining |= selected; continue; }
        if (status != STATUS_OK && status != STATUS_CLOSED && status != STATUS_PEER_CLOSED) {
            v.error = status; v.mode = mem_mode::Failed;
            if (selected) { control.status = status; return; }
            continue;
        }
        (void)v.peer.close(); v.mode = mem_mode::Off;
    }
    if (!running || control.status != STATUS_PENDING) return;
    if (!remaining) {
        control.status = STATUS_OK;
        if (backing.empty() && std::ranges::all_of(volumes, [](const auto& v) { return v->mode == mem_mode::Off; })) {
            nodes.clear(); nodes.shrink_to_fit();
            for (auto& v : volumes) { v->free.clear(); v->free.shrink_to_fit(); v->calls = {}; }
        }
    } else if (stalled && now() >= control.due) {
        control.status = STATUS_NO_MEMORY;
        for (auto& v : volumes) if (v->mode == mem_mode::Draining) v->mode = mem_mode::Paused;
    }
}
static auto status(ubus_request&, ubus_msg& r) -> status_t {
    MemoryInfo ram{}; if (_memory_info(physical.selector()).status == STATUS_OK) ram = *reinterpret_cast<const MemoryInfo*>(_ipc());
    if (_quota_info(_startup().quota).status != STATUS_OK) return STATUS_CLOSED;
    const auto quota = *reinterpret_cast<const QuotaInfo*>(_ipc());
    uint64_t pages{}, capacity{}, reading{}, writing{};
    for (const auto& request : waiting) if (request) { if (request->kind == PAGER_REQUEST_CAPACITY) ++capacity; else ++pages; }
    for (const auto& job : jobs) if (job.phase != mem_phase::Free) { if (job.read) ++reading; else ++writing; }
    ubus_data list{ubus_type::Array}, clients{ubus_type::Array};
    for (const auto& v : volumes) { ubus_data row; row.put("name", v->name).put("state", mode(v->mode)).put("bytes", v->limit * page_size)
        .put("desired-bytes", v->desired_limit ? v->desired_limit * page_size : v->mode == mem_mode::Draining || v->mode == mem_mode::Closing || v->mode == mem_mode::Off ? 0 : v->limit * page_size)
        .put("used", v->used * page_size).put("priority", v->priority).put("error", int64_t{v->error}); list.push(row); }
    for (const auto& [si, entry] : sessions) {
        PagerInfo info{}; if (_pager_configure(cap(si)).status == STATUS_OK) info = *reinterpret_cast<const PagerInfo*>(_ipc());
        ubus_data row; row.put("id", uint64_t{si}).put("logical", info.logical).put("limit", info.limit).put("resident", info.logical ? session(si).resident : 0)
            .put("target", session(si).target).put("swap", session(si).swap_limit); clients.push(row);
    }
    r.data.put("volumes", list).put("sessions", clients).put("free", ram.free).put("reserve", ram.reserve)
        .put("free-low", low).put("free-high", high).put("read-ahead", uint64_t{read_ahead}).put("writeback-depth", uint64_t{write_depth})
        .put("reads", reads).put("writes", writes).put("released", released).put("refaults", refaults).put("scanned", scanned)
        .put("copied", copied).put("failures", failures).put("latency-ticks", latency_total).put("latency-max", latency_max)
        .put("serving", serving).put("operation", control.id).put("result", int64_t{control.status}).put("pool-bytes", uint64_t{io_jobs * page_size})
        .put("index-bytes-estimate", uint64_t{nodes.capacity() * sizeof(mem_index::node_type) + (nodes.size() + backing.size()) * (sizeof(mem_index::value_type) + 32)})
        .put("charged-bytes", quota.memory_limit - quota.memory_available).put("waiting-pages", pages).put("waiting-capacity", capacity)
        .put("inflight-reads", reading).put("inflight-writes", writing).put("frequency", _startup().frequency);
    return STATUS_OK;
}
static auto volume(ubus_request& q, ubus_msg& r, bool format) -> status_t {
    if (!serving) return STATUS_CLOSED;
    if (control.status == STATUS_PENDING) return STATUS_BUSY;
    const auto name = *q.data["volume"].as<std::string_view>();
    size_t index = volumes.size();
    for (size_t i = 0; i < volumes.size(); ++i) if (volumes[i]->name == name) { index = i; break; }
    if (index < volumes.size() && volumes[index]->mode != mem_mode::Off) {
        auto& v = *volumes[index];
        if (format || (v.mode != mem_mode::On && v.mode != mem_mode::Paused)) return STATUS_BUSY;
        if (q.data["offset"].as<uint64_t>().value_or(v.offset) != v.offset || q.data["bytes"].as<uint64_t>().value_or(v.bytes) != v.bytes) return STATUS_BAD_ARGS;
        const auto bytes = q.data["limit"].as<uint64_t>().value_or(v.limit * page_size);
        if (!bytes || bytes % page_size || bytes > v.bytes - page_size) return STATUS_BAD_ARGS;
        const auto count = bytes / page_size; v.desired_limit = 0;
        control = {++next_operation, deadline(10000), STATUS_OK, false, false, index};
        if (count != v.limit) {
            v.desired_limit = count; control.status = STATUS_PENDING;
            control.step = count < v.limit ? mem_step::Drain : mem_step::Prepare;
            if (count < v.limit) v.mode = mem_mode::Draining;
        } else v.mode = mem_mode::On;
        if (count <= v.limit) v.priority = q.data["priority"].as<int64_t>().value_or(v.priority);
        control.options = std::move(q.data);
    } else {
        auto v = std::make_unique<mem_volume>(); v->name = name;
        v->offset = q.data["offset"].as<uint64_t>().value_or(0);
        v->priority = q.data["priority"].as<int64_t>().value_or(0);
        if (index == volumes.size()) volumes.push_back(std::move(v)); else volumes[index] = std::move(v);
        (void)directory.close();
        control = {++next_operation, deadline(10000), STATUS_PENDING, format, !format, index, mem_step::Lookup};
        control.options = std::move(q.data);
    }
    r.data.put("operation", control.id); return STATUS_OK;
}
static void pump_control() {
    if (control.status != STATUS_PENDING || control.step == mem_step::Drain) return;
    auto& v = *volumes[control.volume];
    for (size_t step = 0; step < io_jobs; ++step) {
        auto& peer = control.step == mem_step::Lookup || control.step == mem_step::Prepare ? parent
            : control.step == mem_step::Bind || control.step == mem_step::Device || control.step == mem_step::Open ? directory : v.peer;
        if (peer.active() && !peer.pending()) return;
        status_t status{STATUS_OK}; ubus_msg q, r; q.deadline = deadline(5000);
        switch (control.step) {
        case mem_step::Lookup:
            if (!parent.active()) {
                q.data.put("name", control.inventory ? std::string_view{"hardware"} : std::string_view{v.name});
                status = parent.begin("Get", std::move(q));
                if (status != STATUS_OK) break;
            }
            status = parent.poll(r);
            if (status == STATUS_OK) status = r.status;
            if (status == STATUS_NOT_FOUND && !control.inventory) { control.inventory = true; continue; }
            if (status == STATUS_OK) {
                status = directory.begin_bind(r.capability(r.data["connector"]), {}, 0, UINT64_MAX, 0, deadline(5000));
                control.step = mem_step::Bind;
            }
            break;
        case mem_step::Bind:
            status = directory.poll_bind();
            if (status == STATUS_OK) {
                if (control.inventory) q.data.put("id", v.name);
                else {
                    q.data.put("exclusive", true).put("offset", v.offset);
                    if (const auto bytes = control.options["bytes"].as<uint64_t>()) q.data.put("bytes", *bytes);
                }
                status = directory.begin("Open", std::move(q), true);
                control.step = control.inventory ? mem_step::Device : mem_step::Open;
            }
            break;
        case mem_step::Device:
            status = directory.poll(r); if (status == STATUS_OK) status = r.status;
            if (status == STATUS_OK) {
                status = directory.begin_bind(r.capability(r.data["connector"]), {}, 0, UINT64_MAX, 0, deadline(5000));
                control.inventory = false; control.step = mem_step::Bind;
            }
            break;
        case mem_step::Open:
            status = directory.poll(r); if (status == STATUS_OK) status = r.status;
            if (status == STATUS_OK) {
                status = v.peer.begin_adopt(r.take(r.data["handle"]), {}, 0, UINT64_MAX, 0, deadline(5000));
                (void)directory.close();
                control.step = mem_step::Adopt;
            }
            break;
        case mem_step::Adopt:
            status = v.peer.poll_bind();
            if (status == STATUS_OK) { status = v.peer.begin("Stat", std::move(q), true); control.step = mem_step::Stat; }
            break;
        case mem_step::Stat:
            status = v.peer.poll(r); if (status == STATUS_OK) status = r.status;
            if (status == STATUS_OK) {
                v.bytes = r.data["size"].as<uint64_t>().value_or(0); v.next = r.data["sequence"].as<uint64_t>().value_or(0);
                const auto bytes = control.options["limit"].as<uint64_t>().value_or(v.bytes > page_size ? v.bytes - page_size : 0);
                if (v.bytes < page_size * 2 || v.bytes % page_size || !bytes || bytes % page_size || bytes > v.bytes - page_size) { status = STATUS_BAD_ARGS; break; }
                v.limit = bytes / page_size;
                status = v.calls.prepare(v.peer, 1024); control.step = mem_step::Header;
            }
            break;
        case mem_step::Prepare: {
            size_t required = v.desired_limit ? v.desired_limit : v.limit;
            for (const auto& other : volumes) if (other.get() != &v && other->mode != mem_mode::Off) required += other->limit;
            status = prepare_index(required); if (status != STATUS_OK) break;
            if (v.desired_limit) {
                const auto count = std::exchange(v.desired_limit, 0);
                v.free.resize((count + 64) / 64);
                for (auto slot = v.limit + 1; slot <= count; ++slot) v.free[slot / 64] |= uint64_t{1} << (slot % 64);
                v.limit = count; v.priority = control.options["priority"].as<int64_t>().value_or(v.priority);
                v.mode = mem_mode::On; control.status = STATUS_OK; control.step = mem_step::None;
            } else {
                v.free.assign((v.limit + 64) / 64, UINT64_MAX); v.free[0] &= ~uint64_t{1};
                const auto end = (v.limit + 1) % 64; if (end) v.free.back() &= (uint64_t{1} << end) - 1;
                control.step = mem_step::Register; control.index = 0;
            }
            break;
        }
        case mem_step::Register:
            status = command(v, "Register", control.index);
            if (status == STATUS_OK && ++control.index == io_jobs) {
                v.mode = mem_mode::On; control.status = STATUS_OK; control.step = mem_step::None;
            }
            break;
        default: return;
        }
        if (status == STATUS_WOULD_BLOCK || status == STATUS_PENDING) return;
        if ((status == STATUS_BUSY || status == STATUS_RETRY) && control.step == mem_step::Register) return;
        if (status != STATUS_OK) {
            v.error = status; v.desired_limit = 0;
            if (control.format || control.enable) v.mode = mem_mode::Failed;
            (void)directory.close();
            control.status = status; control.step = mem_step::None; return;
        }
        if (control.status != STATUS_PENDING) return;
    }
}

int main() {
    auto result = parent.bind(_startup().bootstrap); if (result != STATUS_OK) return result;
    ubus_msg setup; result = parent.invoke("Receive", {}, setup); if (result != STATUS_OK) return result;
    auto config = setup.data["message"];
    low = config["free-low"].as<uint64_t>().value_or(low); high = config["free-high"].as<uint64_t>().value_or(high);
    write_depth = config["writeback-depth"].as<uint64_t>().value_or(write_depth);
    if (!write_depth || write_depth > io_jobs - read_jobs) return STATUS_BAD_ARGS;
    physical = setup.take(config["physical"]);
    const auto owned = _scope_create(_startup().alloc()); if (owned.status != STATUS_OK) return owned.status;
    ownership = _cap_owner{owned.value};
    const auto logical = config["logical"].as<uint64_t>().value_or(UINT64_MAX);
    const auto initial = add_session(setup.take(config["pager"]), setup.take(config["scope"]), logical); if (!initial) return initial.error();
    session(0).target = config["resident"].as<uint64_t>().value_or(0);
    session(0).swap_limit = config["swap-limit"].as<uint64_t>().value_or(UINT64_MAX);
    read_ahead = config["read-ahead"].as<uint64_t>().value_or(0);
    if (session(0).target % page_size || (session(0).swap_limit != UINT64_MAX && session(0).swap_limit % page_size) || read_ahead > 1) return STATUS_BAD_ARGS;
    const auto startup_volumes = config["volumes"];
    if (startup_volumes && startup_volumes.type() != ubus_type::Array) return STATUS_BAD_ARGS;
    result = _memory_watch(physical.selector(), low, high, floor_bytes).status; if (result != STATUS_OK) return result;
    result = pressure.open(&_events(), physical.selector(), SOURCE_READY, 0, WATCH_CHANGED); if (result != STATUS_OK) return result;
    for (auto& j : jobs) {
        const auto mem = _memory_create(_startup().alloc(), page_size, VM_READ | VM_WRITE); if (mem.status != STATUS_OK) return mem.status;
        j.buffer = _cap_owner{mem.value}; const auto populated = _memory_populate(mem.value, 0); if (populated.status != STATUS_OK) return populated.status;
        const auto reserved = _pager_buffer(cap(0), 0, mem.value); if (reserved.status != STATUS_OK) return reserved.status;
        j.reserve = reserved.value;
    }
    result = memory.add("Open", {{"quota", ubus_type::Cap}, {"scope", ubus_type::Cap}, {"logical", ubus_type::U64}}, {{"pager", ubus_type::Cap}, {"owner", ubus_type::Cap}, {"scope", ubus_type::Cap}, {"session", ubus_type::U64}}, [](auto& q, auto& r) {
        if (!serving) return STATUS_CLOSED;
        const CreateAuth auth{q.capability(q.data["quota"]), q.capability(q.data["scope"])};
        const auto parked = _scope_create(auth); if (parked.status != STATUS_OK) return parked.status;
        _cap_owner scope{parked.value};
        const auto made = _pager_create({auth.quota, scope.selector()}); if (made.status != STATUS_OK) return made.status;
        auto pager = _cap_owner{made.value}; const auto added = add_session(std::move(pager), std::move(scope), *q.data["logical"].template as<uint64_t>());
        if (!added) return added.error();
        const auto si = *added; const auto client = r.attach(cap(si), RIGHT_ATTACH | RIGHT_DUPLICATE);
        const auto owner = r.attach(cap(si), RIGHT_SERVE | RIGHT_SUPPLY | RIGHT_MANAGE | RIGHT_INSPECT | RIGHT_ADOPT | RIGHT_DESTROY | RIGHT_DUPLICATE);
        const auto parked_scope = r.attach(session(si).scope.selector(), RIGHT_MANAGE | RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (!client || !owner || !parked_scope) return STATUS_NO_MEMORY;
        r.data.cap("pager", *client).cap("owner", *owner).cap("scope", *parked_scope).put("session", uint64_t{si}); return STATUS_OK;
    });
    ubus_schema statistics{{"serving", ubus_type::Bool}, {"volumes", ubus_type::Array}, {"sessions", ubus_type::Array}, {"result", ubus_type::I64}};
    for (const auto name : {"free", "reserve", "free-low", "free-high", "read-ahead", "writeback-depth", "reads", "writes", "released", "refaults", "scanned", "copied", "failures", "latency-ticks", "latency-max", "operation", "pool-bytes", "index-bytes-estimate", "charged-bytes", "waiting-pages", "waiting-capacity", "inflight-reads", "inflight-writes", "frequency"}) statistics.push_back({name, ubus_type::U64});
    if (result == STATUS_OK) result = admin.add("Status", {}, std::move(statistics), status);
    for (const bool format : {true, false}) if (result == STATUS_OK) result = admin.add(format ? "Format" : "Enable",
        {{"volume", ubus_type::String}, {"offset", ubus_type::U64, true}, {"bytes", ubus_type::U64, true}, {"limit", ubus_type::U64, true}, {"priority", ubus_type::I64, true}}, {{"operation", ubus_type::U64}}, [format](auto& q, auto& r) {
            return volume(q, r, format);
        });
    if (result == STATUS_OK) result = admin.add("Pause", {{"volume", ubus_type::String, true}}, {}, [](auto& q, auto&) {
        if (control.status == STATUS_PENDING) return STATUS_BUSY;
        const auto name = q.data["volume"].template as<std::string_view>();
        for (auto& v : volumes) if ((!name || v->name == *name) && v->mode == mem_mode::On) v->mode = mem_mode::Paused;
        return STATUS_OK;
    });
    if (result == STATUS_OK) result = admin.add("Disable", {{"volume", ubus_type::String, true}}, {{"operation", ubus_type::U64}}, [](auto& q, auto& r) {
        if (control.status == STATUS_PENDING) return STATUS_BUSY;
        const auto name = q.data["volume"].template as<std::string_view>();
        for (auto& v : volumes) if ((!name || v->name == *name) && (v->mode == mem_mode::On || v->mode == mem_mode::Paused || v->mode == mem_mode::Failed)) {
            v->desired_limit = 0; v->mode = mem_mode::Draining;
        }
        control = {++next_operation, deadline(10000), STATUS_PENDING, false, false, 0, mem_step::Drain};
        control.options = std::move(q.data); r.data.put("operation", control.id); return STATUS_OK;
    });
    if (result == STATUS_OK) result = admin.add("Query", {{"operation", ubus_type::U64}}, {{"result", ubus_type::I64}}, [](auto& q, auto& r) {
        if (*q.data["operation"].template as<uint64_t>() != control.id) return STATUS_NOT_FOUND;
        r.data.put("result", int64_t{control.status}); return STATUS_OK;
    });
    if (result == STATUS_OK) result = admin.add("Cancel", {{"operation", ubus_type::U64}}, {}, [](auto& q, auto&) {
        if (*q.data["operation"].template as<uint64_t>() != control.id) return STATUS_NOT_FOUND;
        if (control.status != STATUS_PENDING) return STATUS_TOO_LATE;
        if (control.step != mem_step::Drain || control.format || control.enable) return STATUS_BUSY;
        const auto name = control.options["volume"].as<std::string_view>();
        if (std::ranges::any_of(volumes, [&](const auto& v) { return (!name || v->name == *name) && v->mode == mem_mode::Closing; })) return STATUS_TOO_LATE;
        for (auto& v : volumes) if (v->mode == mem_mode::Draining) v->mode = mem_mode::Paused;
        control.status = STATUS_CANCELED; return STATUS_OK;
    });
    if (result == STATUS_OK) result = admin.add("Configure", {{"name", ubus_type::String}, {"value", ubus_type::U64}, {"session", ubus_type::U64, true}}, {}, [](auto& q, auto&) {
        const auto name = *q.data["name"].template as<std::string_view>(); const auto value = *q.data["value"].template as<uint64_t>();
        if (name == "free-low" || name == "free-high") {
            const auto lo = name == "free-low" ? value : low, hi = name == "free-high" ? value : high;
            const auto status = _memory_watch(physical.selector(), lo, hi, floor_bytes).status;
            if (status == STATUS_OK) { low = lo; high = hi; } return status;
        }
        if (name == "read-ahead") { if (value > 1) return STATUS_BAD_ARGS; read_ahead = value; return STATUS_OK; }
        if (name == "writeback-depth") {
            if (!value || value > io_jobs - read_jobs) return STATUS_BAD_ARGS;
            if (active()) return STATUS_BUSY;
            write_depth = value; return STATUS_OK;
        }
        const auto si = q.data["session"].template as<uint64_t>().value_or(0); if (!sessions.contains(si) || value % page_size) return STATUS_BAD_ARGS;
        auto& s = session(si); if (s.closing) return STATUS_CLOSED;
        if (name == "resident") s.target = value;
        else if (name == "logical") return _pager_configure(cap(si), true, value).status;
        else if (name == "swap") { if (value < swap_used(si)) return STATUS_BUSY; s.swap_limit = value; }
        else return STATUS_INVALID_OP;
        return STATUS_OK;
    });
    if (result == STATUS_OK) result = admin.add("Adopt", {{"pager", ubus_type::Cap}, {"scope", ubus_type::Cap}, {"logical", ubus_type::U64},
        {"resident", ubus_type::U64, true}, {"swap", ubus_type::U64, true}}, {{"session", ubus_type::U64}}, [](auto& q, auto& r) {
        if (!serving) return STATUS_CLOSED;
        const auto logical = *q.data["logical"].template as<uint64_t>();
        const auto target = q.data["resident"].template as<uint64_t>().value_or(0), maximum = q.data["swap"].template as<uint64_t>().value_or(UINT64_MAX);
        if (target % page_size || (maximum != UINT64_MAX && maximum % page_size)) return STATUS_BAD_ARGS;
        const auto added = add_session(q.take(q.data["pager"]), q.take(q.data["scope"]), logical); if (!added) return added.error();
        auto& s = session(*added); s.target = target; s.swap_limit = maximum;
        r.data.put("session", uint64_t{*added}); return STATUS_OK;
    });
    if (result == STATUS_OK) result = admin.add("Release", {}, {}, [](auto&, auto&) {
        if (control.status == STATUS_PENDING || active() || !backing.empty() || std::ranges::any_of(waiting, [](const auto& r) { return r.has_value(); })
            || std::ranges::any_of(sessions, [](const auto& row) { return row.second.closing; })
            || std::ranges::any_of(volumes, [](const auto& v) { return v->mode != mem_mode::Off; })) return STATUS_BUSY;
        for (auto& j : jobs) if (j.reserve) {
            const auto status = _pager_buffer(cap(0), 2, j.reserve).status;
            if (status != STATUS_OK) return status;
            j.reserve = 0;
        }
        for (auto& [si, s] : sessions) {
            const auto status = place(s, ownership.selector(), s.scope.selector()); if (status != STATUS_OK) return status;
        }
        for (auto& [si, s] : sessions) {
            for (auto* watch : {&s.watch, &s.owner}) { const auto status = watch->close(); if (status != STATUS_OK) return status; }
        }
        serving = false; return STATUS_OK;
    });
    if (result == STATUS_OK) result = memory.open();
    if (result == STATUS_OK) result = admin.open();
    if (result != STATUS_OK) return result;
    for (const auto& [name, object] : std::array{std::pair{"memory", &memory}, std::pair{"admin", &admin}}) {
        auto connector = object->connector(); if (!connector) return connector.error();
        ubus_msg q, r; const auto index = q.give(std::move(*connector)); if (!index) return index.error();
        q.data.put("name", name).put("interface", name == std::string_view{"memory"} ? "memory" : "memory.admin").put("version", uint64_t{1}).cap("connector", *index);
        result = parent.invoke("Publish", std::move(q), r); if (result != STATUS_OK) return result;
    }
    auto startup_volume = startup_volumes.values().begin();
    for (;;) {
        // A data error may end a control operation while its lookup/resize is
        // outstanding. Retire that reply transport before accepting a new one;
        // accepted Resource effects remain observable through native Quota.
        if (control.status != STATUS_PENDING) {
            if (parent.active()) { result = parent.cancel(); if (result != STATUS_OK) return result; }
            if (directory.active()) { result = directory.close(); if (result != STATUS_OK) return result; }
        }
        if (serving && control.status != STATUS_PENDING && startup_volume != startup_volumes.values().end()) {
            const auto row = *startup_volume; ++startup_volume;
            ubus_request q; ubus_msg r;
            if (!row["volume"].as<std::string_view>() || !q.data.assign(row.bytes())) return STATUS_BAD_ARGS;
            const auto configured = volume(q, r, false);
            if (configured != STATUS_OK) control = {++next_operation, 0, configured};
        }
        for (size_t n = 0; n < 8; ++n) { const auto s = admin.poll(); if (s == STATUS_WOULD_BLOCK) break; if (s != STATUS_OK) return s; }
        for (size_t n = 0; n < 8; ++n) { const auto s = memory.poll(); if (s == STATUS_WOULD_BLOCK) break; if (s != STATUS_OK) return s; }
        if (serving) { result = pump_pagers(); if (result != STATUS_OK) return result; }
        pump_control();
        for (auto& v : volumes) if (!v->method.empty()) (void)command(*v);
        pump_io();
        MemoryInfo ram{}; if (_memory_info(physical.selector()).status == STATUS_OK) ram = *reinterpret_cast<const MemoryInfo*>(_ipc());
        const bool enabled = std::ranges::any_of(volumes, [](const auto& v) { return v->mode == mem_mode::On; });
        const bool draining = std::ranges::any_of(volumes, [](const auto& v) { return v->mode == mem_mode::Draining; });
        const bool shortage = std::ranges::any_of(waiting, [](const auto& r) { return r.has_value(); });
        const bool targeted = std::ranges::any_of(sessions, [](const auto& row) { return row.second.target != 0; });
        if (shortage || (enabled && (ram.pressure || targeted)))
            for (size_t n = 0; n < 8 && scan(ram.pressure && enabled); ++n) {}
        if (draining || sweep_needed || (enabled && slots_needed)) sweep();
        std::erase_if(sessions, [](const auto& row) {
            const auto si = row.first; const auto& s = row.second;
            return s.closing && !s.backed && std::ranges::none_of(jobs, [=](const auto& j) {
                return j.phase != mem_phase::Free && j.key.session == si;
            });
        });
        drain();
        pressure.ack(); const auto observation = _source_info(physical.selector()); if (observation.status == STATUS_OK) (void)pressure.arm(observation.value2);
        const auto wake = _ev_wait(&_events(), active() || control.status == STATUS_PENDING || std::ranges::any_of(volumes, [](const auto& v) { return v->mode == mem_mode::Closing || (!v->method.empty() && v->reply == STATUS_PENDING); }) || draining || shortage || sweep_needed || (enabled && slots_needed) || (serving && startup_volume != startup_volumes.values().end()) ? deadline(1) : enabled && (ram.pressure || targeted) ? deadline(5) : 0).status;
        if (wake != STATUS_OK && wake != STATUS_TIMED_OUT) return wake;
    }
}
