#pragma once

#include <util.hpp>
#include <string.h>

#include <array>
#include <algorithm>
#include <optional>
#include <deque>
#include <span>
#include <tuple>
#include <vector>
#include <string_view>
#include <variant>
#include <libk/assert.hpp>
#include <cap.hpp>
#include <libk/pkg.hpp>
#include <start.hpp>


inline constexpr word_t pr_page_size = 4096;
inline constexpr auto pr_retryable(status_t s) noexcept -> bool {
    return s == STATUS_BUSY || s == STATUS_RETRY;
}
struct pr_window {
    word_t address{}, size{};
    static constexpr auto round_size(word_t n) noexcept -> word_t {
        return n <= ~word_t{} - (pr_page_size - 1) ? (n + pr_page_size - 1) & ~(pr_page_size - 1) : 0;
    }
    constexpr auto valid() const noexcept -> bool {
        return address && size && !(address % pr_page_size) && !(size % pr_page_size)
            && size <= ~word_t{} - address;
    }
    constexpr auto overlaps(pr_window b) const noexcept -> bool {
        return valid() && b.valid() && address < b.address + b.size && b.address < address + size;
    }
};

class pr_pkg {
    vm_area bytes_;
    libk::pkg_view bundle_{};
    cap_t memory_{};
    word_t flags_{VM_READ};
public:
    auto open(cap_t vm, cap_t mem, pr_window w, size_t size, word_t flags = VM_READ) noexcept -> status_t {
        if (!size || size > w.size || !(flags & VM_READ)
            || (flags & ~(VM_READ | VM_EXECUTE | VM_MAP_PRIVATE))) return STATUS_BAD_ARGS;
        auto s = close();
        if (s == STATUS_OK) s = bytes_.open(vm, w.address, w.size, VM_READ);
        if (s == STATUS_OK) s = bytes_.attach(mem, 0, w.size, VM_READ);
        if (s != STATUS_OK) return s;
        bundle_ = libk::pkg_view::parse(reinterpret_cast<const void*>(bytes_.address), size);
        memory_ = mem;
        flags_ = flags;
        return bundle_ ? STATUS_OK : STATUS_BAD_ARGS;
    }
    auto close() noexcept -> status_t { bundle_ = {}; memory_ = 0; return bytes_.close(); }
    auto size() const noexcept -> size_t { return bundle_.size(); }
    auto memory() const noexcept -> cap_t { return memory_; }
    auto flags() const noexcept -> word_t { return flags_; }
    auto address() const noexcept -> word_t { return bytes_.address; }
    auto entry() const noexcept -> std::optional<libk::pkg_mod> { return bundle_.root(); }
    auto find(std::string_view name) const noexcept -> std::optional<libk::pkg_mod> {
        return bundle_.find(name);
    }
};

// Temporary selectors are adopted before any operation that may fail. The
// task owns their cleanup; VSpace owns committed backing after selectors close.
class pr_loader {
    using owner = cap_owner;
    cap_t pool_, vm_;
    vm_area& scratch_;
    pr_pkg& package_;
public:
    pr_loader(cap_t pool, cap_t vm, vm_area& s, pr_pkg& p)
        : pool_(pool), vm_(vm), scratch_(s), package_(p) {}
    auto adopt(sys::SysResult r) noexcept -> std::expected<owner, status_t> {
        owner cap{r.value};
        if (!cap) return std::unexpected(r.status == STATUS_OK ? STATUS_INVALID_CAP : r.status);
        if (r.status != STATUS_OK) return std::unexpected(r.status);
        return cap;
    }
    auto memory(word_t size, word_t perms = VM_READ | VM_WRITE) noexcept -> std::expected<owner, status_t> {
        return adopt(sys::memory_create(pool_, size, perms));
    }
    auto write(cap_t mem, word_t size, word_t offset, const void* src, size_t n) noexcept -> status_t {
        if (offset > size || n > size - offset || (n && !src)) return STATUS_BAD_ARGS;
        const auto* bytes = static_cast<const uint8_t*>(src);
        while (n) {
            const auto page = offset / pr_page_size;
            const auto start = offset % pr_page_size;
            const auto count = std::min(n, size_t{pr_page_size - start});
            auto s = sys::memory_populate(mem, page).status;
            if (s != STATUS_OK) return s;
            s = scratch_.attach(mem, page, pr_page_size, VM_READ | VM_WRITE);
            if (s != STATUS_OK) return s;
            std::copy_n(bytes, count, reinterpret_cast<uint8_t*>(scratch_.address) + start);
            const auto detached = scratch_.detach();
            if (detached != STATUS_OK) return detached;
            offset += count; bytes += count; n -= count;
        }
        return STATUS_OK;
    }
    auto make_desc(const void* src, size_t n) noexcept -> std::expected<owner, status_t> {
        const auto size = pr_window::round_size(n);
        if (!size || !src) return std::unexpected(STATUS_BAD_ARGS);
        auto mem = memory(size);
        if (!mem) return mem;
        const auto s = write(mem->selector(), size, 0, src, n);
        if (s != STATUS_OK) return std::unexpected(s);
        return mem;
    }
    auto install(cap_t mem, word_t address, word_t size, word_t perms,
                 word_t first = 0, word_t flags = 0) noexcept -> status_t {
        if (!pr_window{address, size}.valid()) return STATUS_BAD_ARGS;
        auto region = adopt(sys::vm_slice(vm_, address, size, perms,
            RIGHT_MAP | RIGHT_UNMAP));
        if (!region) return region.error();
        const auto status = sys::vm_map(region->selector(), mem, address, size, first, perms | flags).status;
        return status;
    }
    auto anonymous(word_t address, word_t size, word_t perms) noexcept -> std::expected<owner, status_t> {
        auto mem = memory(size);
        if (!mem) return mem;
        const auto s = install(mem->selector(), address, size, perms);
        if (s != STATUS_OK) return std::unexpected(s);
        return mem;
    }
    auto file_pages(const libk::pkg_seg& seg, word_t& first) noexcept -> std::expected<owner, status_t> {
        const auto base = package_.address();
        const auto file = reinterpret_cast<word_t>(seg.file.data());
        if (file < base || file - base > package_.size()
            || seg.file.size() > package_.size() - (file - base)
            || (file - base) % pr_page_size || seg.file.size() % pr_page_size) return std::unexpected(STATUS_BAD_ARGS);
        first = (file - base) / pr_page_size;
        const CapView view{.version = CAP_ATTENUATION_VERSION_CURRENT, .kind = ObjMem,
            .size = CAP_ATTENUATION_SIZE, .rights = RIGHT_MAP,
            .words = {first, seg.file.size() / pr_page_size, (seg.perms & VM_WRITE) ? VM_READ : seg.perms}};
        std::array<uint8_t, CAP_ATTENUATION_SIZE> wire{};
        cap_encode(view, wire);
        auto desc = make_desc(wire.data(), wire.size());
        if (!desc) return desc;
        return adopt(sys::cap_typed_delegate(package_.memory(), 0, desc->selector()));
    }
    auto map(const libk::pkg_mod& mod) noexcept -> status_t {
        if (!mod.executable) return STATUS_BAD_ARGS;
        for (size_t i = 0; i < mod.segment_count(); ++i) {
            const auto decoded = mod.segment(i);
            if (!decoded) return STATUS_BAD_ARGS;
            const auto& seg = *decoded;
            const auto size = pr_window::round_size(seg.size);
            if (!size) return STATUS_BAD_ARGS;
            if (((seg.perms & VM_EXECUTE) && !(package_.flags() & VM_EXECUTE))
                || ((seg.perms & VM_WRITE) && !(package_.flags() & VM_MAP_PRIVATE))) {
                // Copy when the source lacks executable or private-map support.
                // Boot physical memory and sealed file pages differ here.
                auto mem = memory(size, seg.perms | VM_READ | VM_WRITE);
                if (!mem) return mem.error();
                auto s = write(mem->selector(), size, 0, seg.file.data(), seg.file.size());
                if (s == STATUS_OK && (seg.perms & VM_EXECUTE)) s = sys::memory_seal(mem->selector()).status;
                if (s == STATUS_OK) s = install(mem->selector(), seg.va, size, seg.perms);
                if (s != STATUS_OK) return s;
                continue;
            }
            const auto file_size = seg.file.size() / pr_page_size * pr_page_size;
            const auto boundary = seg.file.size() % pr_page_size ? pr_page_size : 0;
            // At most three disjoint backings: whole file pages, one partial
            // page, and demand-zero BSS. The pr_pkg segments remain the pr_layout authority.
            word_t offset = 0;
            while (offset < size) {
                const bool file = offset < file_size;
                const bool partial = boundary && offset == file_size;
                const word_t length = file ? file_size : partial ? pr_page_size : size - offset;
                word_t first{};
                std::expected<owner, status_t> mem;
                if (file) {
                    auto part = seg;
                    part.file = part.file.first(length);
                    part.size = length;
                    mem = file_pages(part, first);
                } else {
                    mem = memory(length, seg.perms | VM_READ | VM_WRITE);
                }
                if (!mem) return mem.error();
                if (!file) {
                    const auto n = partial ? seg.file.size() - offset : 0;
                    if (n) {
                        const auto s = write(mem->selector(), length, 0, seg.file.data() + offset, n);
                        if (s != STATUS_OK) return s;
                    }
                    if (seg.perms & VM_EXECUTE) {
                        const auto s = sys::memory_seal(mem->selector()).status;
                        if (s != STATUS_OK) return s;
                    }
                }
                const auto s = install(mem->selector(), seg.va + offset, length, seg.perms,
                    first, file && (seg.perms & VM_WRITE) ? VM_MAP_PRIVATE : 0);
                if (s != STATUS_OK) return s;
                offset += length;
            }
        }
        return STATUS_OK;
    }
};

inline constexpr word_t pr_base_kinds = OBJ_BIT(ObjThread) | OBJ_BIT(ObjSc)
    | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjMem) | OBJ_BIT(ObjVSpace)
    | OBJ_BIT(ObjGroup) | OBJ_BIT(ObjNotification);

struct pr_id {
    uint32_t slot{}, gen{};
    auto valid() const noexcept -> bool { return gen != 0; }
    auto key() const noexcept -> uint64_t { return (uint64_t{gen} << 32) | slot; }
    auto operator==(const pr_id&) const -> bool = default;
};
struct pr_cap {
    rt_req contract{};
    uint64_t bytes{};
    cap_t cap{};
    std::variant<word_t, CapView> limit{}; // Rights-only duplication or a derived typed view.
    word_t badge{}; // Nonzero requests a Channel endpoint minted by the parent.
};
struct pr_quota {
    word_t bytes{1024 * 1024}, caps{256}, kinds{pr_base_kinds};
    word_t slots{128}, pages{20}, pool_perms{RIGHT_CREATE};
};
struct pr_sched {
    word_t budget{500'000}, period{10'000'000}, urgency{8};
    uint32_t cpu{UINT32_MAX};
};
struct pr_layout {
    pr_window stack{0x40010000, 0x10000};
    word_t bindings{0x40020000};
    // IPC VA is the current runtime ABI, shared by applications and servers.
    word_t ipc{0x40000000};
};
struct pr_opts {
    std::span<const std::string_view> argv{};
    std::span<const pr_cap> caps{};
    pr_quota resources{};
    pr_sched sched{};
    pr_layout va{};
    cap_t events{}; // Borrowed parent receive capability; caller keeps it alive.
};
struct pr_spawn { pr_id id{}; status_t status{STATUS_BUSY}; };
struct pr_exit { status_t status{}; };

// One slot owns a task through construction and asynchronous group drain.
// Exit consumption, rather than process death, releases the slot generation.
class pr_tasks {
    using owner = cap_owner;
    struct record {
        owner group_, vm_, cs_, thread_, sc_, ended_;
        bool running_{}, closing_{};
        status_t result_{};
        ~record() { if (group_) sys::exit(STATUS_BUSY); }
        auto pool() const noexcept -> cap_t { return group_.selector(); }
        auto vspace() const noexcept -> cap_t { return vm_.selector(); }
        auto cspace() const noexcept -> cap_t { return cs_.selector(); }
        auto open(cap_t parent, const pr_quota& q) noexcept -> status_t {
            const auto g = sys::resource_create_child(parent, q.bytes, q.caps, q.kinds);
            if (g.value) group_ = owner{g.value};
            if (g.status != STATUS_OK || !group_) return g.status == STATUS_OK ? STATUS_INVALID_CAP : g.status;
            const auto v = sys::vspace_create(pool());
            if (v.value) vm_ = owner{v.value};
            if (v.status != STATUS_OK || !vm_) return v.status == STATUS_OK ? STATUS_INVALID_CAP : v.status;
            const auto c = sys::cspace_create(pool(), q.slots, q.pages);
            if (c.value) cs_ = owner{c.value};
            return c.status == STATUS_OK && !cs_ ? STATUS_INVALID_CAP : c.status;
        }
        auto close() noexcept -> status_t {
            if (!group_) return STATUS_OK;
            // Child slots belong to its CSpace, retired by group drain.
            for (auto* cap : {&thread_, &sc_, &ended_, &cs_, &vm_}) {
                const auto status = cap->close();
                if (status != STATUS_OK) return status;
            }
            const auto status = sys::resource_close(pool()).status;
            return status == STATUS_OK ? group_.close() : status;
        }
    };
    struct slot {
        uint32_t gen{1};
        std::variant<std::monostate, record, pr_exit> value;
    };
    std::deque<slot> slots_;
    cap_t pool_{}, domain_{};
    uint32_t cpus_{};
    // Synchronous construction only; no task retains these borrowed bytes.

    auto find(pr_id id) noexcept -> slot* {
        return id.valid() && id.slot < slots_.size() && slots_[id.slot].gen == id.gen ? &slots_[id.slot] : nullptr;
    }
    auto active(pr_id id) noexcept -> record* {
        auto* s = find(id);
        return s ? std::get_if<record>(&s->value) : nullptr;
    }
    auto build(record& r, pr_id id, pr_pkg& pkg,
               const libk::pkg_mod& mod, const pr_opts& o) noexcept -> status_t {
        size_t binding_size = sizeof(rt_bindings) + o.caps.size() * sizeof(rt_binding);
        for (const auto& src : o.caps) {
            if (!src.cap || !src.contract.name || !src.contract.kind) return STATUS_BAD_ARGS;
            const auto name = std::string_view{src.contract.name};
            if (name.empty() || name.size() > UINT32_MAX - binding_size) return STATUS_BAD_ARGS;
            binding_size += name.size();
        }
        if (binding_size > UINT32_MAX) return STATUS_BAD_ARGS;
        const pr_window bindings{o.va.bindings, pr_window::round_size(binding_size)};
        const pr_window ipc_range{o.va.ipc, pr_page_size};
        if (!cpus_ || !domain_ || !o.va.stack.valid() || !bindings.valid() || !ipc_range.valid() ||
            o.va.stack.overlaps(bindings) || o.va.stack.overlaps(ipc_range) || bindings.overlaps(ipc_range))
            return STATUS_BAD_ARGS;
        for (size_t i = 0; i < mod.segment_count(); ++i) {
            const auto decoded = mod.segment(i);
            if (!decoded) return STATUS_BAD_ARGS;
            const pr_window w{decoded->va, pr_window::round_size(decoded->size)};
            if (!w.valid() || w.overlaps(o.va.stack) || w.overlaps(bindings) || w.overlaps(ipc_range))
                return STATUS_BAD_ARGS;
        }
        std::vector<std::string_view> argv{o.argv.begin(), o.argv.end()};
        if (argv.empty()) argv.push_back(mod.name);
        size_t strings{};
        for (const auto arg : argv) {
            if (arg.find('\0') != std::string_view::npos || arg.size() >= o.va.stack.size - strings)
                return STATUS_BAD_ARGS;
            strings += arg.size() + 1;
        }
        auto s = r.open(pool_, o.resources);
        if (s != STATUS_OK) return s;
        vm_area scratch;
        s = scratch.open(parent_vm_, 0, pr_page_size);
        if (s != STATUS_OK) return s;
        pr_loader load{r.pool(), r.vspace(), scratch, pkg};
        s = load.map(mod);
        if (s != STATUS_OK) return s;
        auto stack = load.anonymous(o.va.stack.address, o.va.stack.size, VM_READ | VM_WRITE);
        if (!stack) return stack.error();
        auto ipc = load.anonymous(o.va.ipc, pr_page_size, VM_READ | VM_WRITE);
        if (!ipc) return ipc.error();
        s = sys::memory_populate(ipc->selector(), 0).status;
        if (s != STATUS_OK) return s;
        auto events = load.adopt(sys::notification_create(r.pool(), rt_event_badge));
        if (!events) return events.error();
        auto ended = load.adopt(o.events ? sys::cap_duplicate(o.events, 0, RIGHT_SIGNAL)
                                        : sys::notification_create(r.pool(), 1));
        if (!ended) return ended.error();
        r.ended_ = std::move(*ended);
        std::vector<AuxEntry> aux{{AT_PAGESZ, pr_page_size}, {AT_ENTRY, mod.entry},
                                  {AT_SYS_CPUS, cpus_}, {AT_SYS_IPC, o.va.ipc}};
        for (auto [key, cap, rights] : std::array{
            std::tuple{AT_SYS_POOL, r.pool(), o.resources.pool_perms},
            std::tuple{AT_SYS_VSPACE, r.vspace(), word_t{RIGHT_DELEGATE | RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP | RIGHT_DESTROY}},
            std::tuple{AT_SYS_CSPACE, r.cspace(), word_t{RIGHT_MANAGE}},
            std::tuple{AT_SYS_EVENTS, events->selector(), word_t{RIGHT_SIGNAL | RIGHT_RECEIVE | RIGHT_DUPLICATE}}}) {
            const auto copy = sys::cap_duplicate(cap, r.cspace(), rights);
            if (copy.status != STATUS_OK) return copy.status;
            aux.push_back({static_cast<word_t>(key), copy.value});
        }
        auto desc = load.memory(pr_page_size);
        if (!desc) return desc.error();
        if (!o.caps.empty()) {
            std::vector<uint8_t> data(binding_size);
            const rt_bindings header{rt_bindings_version, static_cast<uint32_t>(binding_size),
                                     static_cast<uint32_t>(o.caps.size()), 0};
            ::memcpy(data.data(), &header, sizeof(header));
            size_t name_offset = sizeof(header) + o.caps.size() * sizeof(rt_binding);
            for (size_t i = 0; i < o.caps.size(); ++i) {
                const auto& src = o.caps[i];
                const auto* view = std::get_if<CapView>(&src.limit);
                const auto kind = src.contract.kind;
                if (view && (view->version != CAP_ATTENUATION_VERSION_CURRENT ||
                    view->size != CAP_ATTENUATION_SIZE || view->kind != kind)) return STATUS_BAD_ARGS;
                const auto rights = view ? view->rights : std::get<word_t>(src.limit);
                sys::SysResult copied;
                if (src.badge) copied = sys::channel_mint(src.cap, r.cspace(), src.badge, rights);
                else if (!view) copied = sys::cap_duplicate(src.cap, r.cspace(), rights);
                else {
                    std::array<uint8_t, CAP_ATTENUATION_SIZE> wire{};
                    cap_encode(*view, wire);
                    s = load.write(desc->selector(), pr_page_size, 0, wire.data(), wire.size());
                    if (s != STATUS_OK) return s;
                    copied = sys::cap_typed_delegate(src.cap, r.cspace(), desc->selector());
                }
                if (copied.status != STATUS_OK) return copied.status;
                const std::string_view name{src.contract.name};
                const rt_binding entry{copied.value, src.bytes, src.contract.protocol, kind,
                    src.contract.major, src.contract.minor, 0, static_cast<uint32_t>(name_offset),
                    static_cast<uint32_t>(name.size())};
                ::memcpy(data.data() + sizeof(header) + i * sizeof(entry), &entry, sizeof(entry));
                ::memcpy(data.data() + name_offset, name.data(), name.size());
                name_offset += name.size();
            }
            auto memory = load.anonymous(bindings.address, bindings.size, VM_READ);
            if (!memory) return memory.error();
            s = load.write(memory->selector(), bindings.size, 0, data.data(), data.size());
            if (s == STATUS_OK) s = sys::memory_seal(memory->selector()).status;
            if (s != STATUS_OK) return s;
            aux.push_back({AT_SYS_BINDINGS, bindings.address});
            aux.push_back({AT_SYS_BINDINGS_SIZE, binding_size});
        }
        aux.push_back({AT_NULL, 0});
        const auto pointers = (argv.size() + 3) * sizeof(word_t);
        const auto prefix = pointers + aux.size() * sizeof(AuxEntry);
        if (prefix > o.va.stack.size || strings > o.va.stack.size - prefix) return STATUS_BAD_ARGS;
        const auto stack_bytes = (prefix + strings + 15) & ~size_t{15};
        if (stack_bytes > o.va.stack.size) return STATUS_BAD_ARGS;
        const auto sp = o.va.stack.address + o.va.stack.size - stack_bytes;
        std::vector<uint8_t> initial(stack_bytes);
        auto* words = reinterpret_cast<word_t*>(initial.data());
        words[0] = argv.size();
        size_t offset = prefix;
        for (size_t i = 0; i < argv.size(); ++i) {
            words[i + 1] = sp + offset;
            ::memcpy(initial.data() + offset, argv[i].data(), argv[i].size());
            offset += argv[i].size() + 1;
        }
        ::memcpy(initial.data() + pointers, aux.data(), aux.size() * sizeof(AuxEntry));
        s = load.write(stack->selector(), o.va.stack.size, o.va.stack.size - stack_bytes,
                       initial.data(), initial.size());
        if (s != STATUS_OK) return s;
        ThreadInit init{};
        init.version = THREAD_START_VERSION;
        init.entry = mod.entry;
        init.stack = sp;
        init.ipc = {.memory = ipc->selector(), .page = 0, .address = o.va.ipc, .pages = 1};
        s = load.write(desc->selector(), 4096, 0, &init, sizeof(init));
        if (s != STATUS_OK) return s;
        auto thread = load.adopt(sys::thread_create(r.pool(), r.vspace(), r.cspace(), desc->selector(), 0));
        if (!thread) return thread.error();
        r.thread_ = std::move(*thread);
        sys::SysResult created{.status = STATUS_BUSY};
        for (uint32_t i = 0; i < (o.sched.cpu == UINT32_MAX ? cpus_ : 1); ++i) {
            const auto cpu = o.sched.cpu == UINT32_MAX ? (id.slot + i) % cpus_ : o.sched.cpu;
            created = sys::sc_create(r.pool(), domain_, o.sched.budget, o.sched.period, o.sched.urgency, cpu);
            if (created.status != STATUS_BUSY) break;
        }
        auto sc = load.adopt(created);
        if (!sc) return sc.error();
        s = sys::sc_bind(sc->selector(), r.thread_.selector()).status;
        if (s == STATUS_OK) s = sys::exit_bind(r.thread_.selector(), r.ended_.selector(), 1).status;
        r.sc_ = std::move(*sc);
        if (s == STATUS_OK) s = scratch.close();
        if (s != STATUS_OK) return s;
        s = sys::execution_start(r.thread_.selector()).status;
        r.running_ = s == STATUS_OK;
        return s;
    }
    cap_t parent_vm_{};
public:
    pr_tasks() = default;
    pr_tasks(const pr_tasks&) = delete;
    auto operator=(const pr_tasks&) -> pr_tasks& = delete;
    ~pr_tasks() {
        for (const auto& s : slots_) if (!std::holds_alternative<std::monostate>(s.value)) sys::exit(STATUS_BUSY);
    }
    void open(cap_t pool, cap_t domain, cap_t vm, uint32_t cpus) noexcept {
        pool_ = pool; domain_ = domain; parent_vm_ = vm; cpus_ = cpus;
    }
    auto spawn(pr_pkg& pkg, std::string_view name, const pr_opts& opts = {}) noexcept -> pr_spawn {
        const auto mod = name.empty() ? pkg.entry() : pkg.find(name);
        if (!mod || !mod->executable) return {{}, STATUS_BAD_ARGS};
        uint32_t i = 0;
        for (; i < slots_.size(); ++i)
            if (slots_[i].gen && std::holds_alternative<std::monostate>(slots_[i].value)) break;
        if (i == slots_.size()) {
            if (slots_.size() == UINT32_MAX) return {{}, STATUS_NO_MEMORY};
            slots_.emplace_back();
        }
        auto& slot = slots_[i];
        const pr_id id{i, slot.gen};
        auto& r = slot.value.emplace<record>();
        const auto s = build(r, id, pkg, *mod, opts);
        if (s != STATUS_OK) { r.closing_ = true; r.result_ = s; (void)poll(id); }
        return {id, s};
    }
    auto observe(pr_id id) noexcept -> sys::SysResult {
        auto* r = active(id);
        if (r && r->running_) return sys::exit_query(r->thread_.selector());
        auto* s = find(id);
        if (s) if (const auto* e = std::get_if<pr_exit>(&s->value))
            return {.status = STATUS_OK, .value = 1, .value2 = static_cast<word_t>(e->status)};
        return {.status = STATUS_INVALID_CAP};
    }
    auto poll(pr_id id) noexcept -> status_t {
        auto* s = find(id);
        if (!s) return STATUS_INVALID_CAP;
        if (std::holds_alternative<pr_exit>(s->value)) return STATUS_OK;
        auto* r = std::get_if<record>(&s->value);
        if (!r) return STATUS_INVALID_CAP;
        if (!r->closing_) {
            const auto result = sys::exit_query(r->thread_.selector());
            if (result.status != STATUS_OK) return result.status;
            if (!result.value) return STATUS_WOULD_BLOCK;
            r->closing_ = true;
            r->result_ = static_cast<status_t>(result.value2);
        }
        const auto status = r->close();
        if (status != STATUS_OK) return status;
        s->value.template emplace<pr_exit>(pr_exit{r->result_});
        return STATUS_OK;
    }
    auto request_stop(pr_id id) noexcept -> status_t {
        auto* s = find(id);
        if (!s) return STATUS_INVALID_CAP;
        if (std::holds_alternative<pr_exit>(s->value)) return STATUS_OK;
        auto* r = active(id);
        if (!r) return STATUS_INVALID_CAP;
        if (r->closing_) return STATUS_OK;
        const auto result = sys::exit_query(r->thread_.selector());
        if (result.status != STATUS_OK) return result.status;
        r->closing_ = true;
        r->result_ = result.value ? static_cast<status_t>(result.value2) : STATUS_CANCELED;
        return STATUS_OK;
    }
    auto closing_needs_poll(pr_id id) noexcept -> bool {
        auto* r = active(id);
        return r && r->closing_;
    }
    auto result(pr_id id) noexcept -> std::optional<pr_exit> {
        auto* s = find(id);
        if (s) if (const auto* e = std::get_if<pr_exit>(&s->value)) return *e;
        return {};
    }
    auto collect(pr_id id) noexcept -> sys::SysResult {
        const auto status = poll(id);
        if (status != STATUS_OK) return {.status = status};
        auto* s = find(id);
        const auto result = std::get<pr_exit>(s->value).status;
        s->value.template emplace<std::monostate>();
        s->gen = s->gen == UINT32_MAX ? 0 : s->gen + 1;
        return {.status = STATUS_OK, .value = static_cast<word_t>(result)};
    }
};
