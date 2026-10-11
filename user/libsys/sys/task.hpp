#pragma once

#include <array>
#include <span>
#include <tuple>
#include <variant>
#include <libk/assert.hpp>
#include <sys/handle.hpp>
#include <libk/pkg.hpp>
#include <sys/start.hpp>

namespace proc {
inline constexpr word_t page_size = 4096;
inline constexpr auto retryable(status_t s) noexcept -> bool {
    return s == STATUS_BUSY || s == STATUS_RETRY;
}
struct window {
    word_t address{}, size{};
    static constexpr auto round_size(word_t n) noexcept -> word_t {
        return n <= ~word_t{} - (page_size - 1) ? (n + page_size - 1) & ~(page_size - 1) : 0;
    }
    constexpr auto valid() const noexcept -> bool {
        return address && size && !(address % page_size) && !(size % page_size)
            && size <= ~word_t{} - address;
    }
    constexpr auto overlaps(window b) const noexcept -> bool {
        return valid() && b.valid() && address < b.address + b.size && b.address < address + size;
    }
};

class package {
    sys::mapping bytes_;
    libk::pkg_view bundle_{};
    cap_t memory_{};
    word_t flags_{VM_READ};
public:
    auto open(cap_t vm, cap_t mem, window w, size_t size, word_t flags = VM_READ) noexcept -> status_t {
        if (!size || size > w.size || !(flags & VM_READ)
            || (flags & ~(VM_READ | VM_EXECUTE | VM_MAP_PRIVATE))) return STATUS_BAD_ARGS;
        auto s = close();
        if (s == STATUS_OK) s = bytes_.open(vm, w.address, w.size, VM_READ);
        if (s == STATUS_OK) s = bytes_.attach(mem, 0, w.size, VM_READ);
        if (s != STATUS_OK) return s;
        bundle_ = libk::pkg_view::parse(reinterpret_cast<const void*>(w.address), size);
        memory_ = mem;
        flags_ = flags;
        return bundle_ ? STATUS_OK : STATUS_BAD_ARGS;
    }
    auto close() noexcept -> status_t { bundle_ = {}; memory_ = 0; return bytes_.close(); }
    auto size() const noexcept -> size_t { return bundle_.size(); }
    auto memory() const noexcept -> cap_t { return memory_; }
    auto flags() const noexcept -> word_t { return flags_; }
    auto address() const noexcept -> word_t { return bytes_.address; }
    auto find(std::string_view name) const noexcept -> std::optional<libk::pkg_mod> {
        return bundle_.find(name);
    }
};

// Temporary selectors are adopted before any operation that may fail. The
// task owns their cleanup; VSpace owns committed backing after selectors close.
class loader {
    using owner = sys::cap_owner;
    cap_t pool_, vm_;
    sys::mapping& scratch_;
    package& package_;
public:
    loader(cap_t pool, cap_t vm, sys::mapping& s, package& p)
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
            const auto page = offset / page_size;
            const auto start = offset % page_size;
            const auto count = std::min(n, size_t{page_size - start});
            auto s = sys::memory_populate(mem, page).status;
            if (s != STATUS_OK) return s;
            s = scratch_.attach(mem, page, page_size, VM_READ | VM_WRITE);
            if (s != STATUS_OK) return s;
            std::copy_n(bytes, count, reinterpret_cast<uint8_t*>(scratch_.address) + start);
            const auto detached = scratch_.detach();
            if (detached != STATUS_OK) return detached;
            offset += count; bytes += count; n -= count;
        }
        return STATUS_OK;
    }
    auto make_desc(const void* src, size_t n) noexcept -> std::expected<owner, status_t> {
        const auto size = window::round_size(n);
        if (!size || !src) return std::unexpected(STATUS_BAD_ARGS);
        auto mem = memory(size);
        if (!mem) return mem;
        const auto s = write(mem->selector(), size, 0, src, n);
        if (s != STATUS_OK) return std::unexpected(s);
        return mem;
    }
    auto install(cap_t mem, word_t address, word_t size, word_t perms,
                 word_t first = 0, word_t flags = 0) noexcept -> status_t {
        if (!window{address, size}.valid()) return STATUS_BAD_ARGS;
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
            || (file - base) % page_size || seg.file.size() % page_size) return std::unexpected(STATUS_BAD_ARGS);
        first = (file - base) / page_size;
        const CapView view{.version = CAP_ATTENUATION_VERSION_CURRENT, .kind = ObjMem,
            .size = CAP_ATTENUATION_SIZE, .rights = RIGHT_MAP,
            .words = {first, seg.file.size() / page_size, (seg.perms & VM_WRITE) ? VM_READ : seg.perms}};
        std::array<uint8_t, CAP_ATTENUATION_SIZE> wire{};
        sys::encode_cap(view, wire);
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
            const auto size = window::round_size(seg.size);
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
            const auto file_size = seg.file.size() / page_size * page_size;
            const auto boundary = seg.file.size() % page_size ? page_size : 0;
            // At most three disjoint backings: whole file pages, one partial
            // page, and demand-zero BSS. The package segments remain the layout authority.
            word_t offset = 0;
            while (offset < size) {
                const bool file = offset < file_size;
                const bool partial = boundary && offset == file_size;
                const word_t length = file ? file_size : partial ? page_size : size - offset;
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

inline constexpr word_t base_kinds = OBJ_BIT(ObjThread) | OBJ_BIT(ObjSc)
    | OBJ_BIT(ObjCSpace) | OBJ_BIT(ObjMem) | OBJ_BIT(ObjVSpace)
    | OBJ_BIT(ObjGroup) | OBJ_BIT(ObjNotification);

struct task_id {
    uint32_t slot{}, gen{};
    auto valid() const noexcept -> bool { return gen != 0; }
    auto key() const noexcept -> uint64_t { return (uint64_t{gen} << 32) | slot; }
    auto operator==(const task_id&) const -> bool = default;
};
struct cap_src {
    uint32_t role{};
    boot::Import contract{};
    cap_t cap{};
    std::variant<word_t, CapView> limit{}; // Rights-only duplication or a derived typed view.
    word_t badge{}; // Nonzero requests a Channel endpoint minted by the parent.
};
struct quota {
    word_t bytes{1024 * 1024}, caps{256}, kinds{base_kinds};
    word_t slots{128}, pages{20}, pool_perms{RIGHT_CREATE};
};
struct schedule {
    word_t budget{500'000}, period{10'000'000}, urgency{8};
    uint32_t cpu{UINT32_MAX};
};
struct layout {
    window stack{0x40010000, 0x10000};
    word_t boot{0x40020000};
    // IPC VA is the current runtime ABI, shared by applications and servers.
    word_t ipc{BOOT_ROOT_IPC_ADDRESS};
};
struct spawn_opts {
    const boot::args* args{};
    std::span<const cap_src> caps{};
    quota resources{};
    schedule sched{};
    layout va{};
    word_t scratch{0x18000000};
    cap_t events{}; // Borrowed parent receive capability; caller keeps it alive.
    word_t close_badge{};
};
struct spawn_result { task_id id{}; status_t status{STATUS_BUSY}; };
struct task_exit { status_t status{}; };

// One slot owns a task through construction and asynchronous group drain.
// Exit consumption, rather than process death, releases the slot generation.
template<size_t Capacity>
class tasks {
    using owner = sys::cap_owner;
    struct record {
        owner group_, vm_, cs_, thread_, sc_, ended_;
        cap_t close_events_{};
        word_t close_badge_{};
        enum class drain : uint8_t { live, waiting, done };
        drain drain_{};
        bool running_{}, closing_{};
        status_t result_{};
        ~record() { if (group_) sys::exit(STATUS_BUSY); }
        auto pool() const noexcept -> cap_t { return group_.selector(); }
        auto vspace() const noexcept -> cap_t { return vm_.selector(); }
        auto cspace() const noexcept -> cap_t { return cs_.selector(); }
        auto open(cap_t parent, const quota& q) noexcept -> status_t {
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
            status_t s{};
            if (drain_ == drain::live) {
                s = close_events_ ? sys::resource_close_async(pool(), close_events_, close_badge_).status
                                  : sys::resource_close(pool()).status;
                if (s != STATUS_OK) return s;
                drain_ = close_events_ ? drain::waiting : drain::done;
            }
            return drain_ == drain::waiting ? STATUS_BUSY : group_.close();
        }
    };
    struct slot {
        uint32_t gen{1};
        std::variant<std::monostate, record, task_exit> value;
    };
    std::array<slot, Capacity> slots_{};
    cap_t pool_{}, domain_{};
    uint32_t cpus_{};
    // Synchronous construction only; no task retains these borrowed bytes.
    alignas(BootCap) std::array<uint8_t, 4096> boot_{};

    auto find(task_id id) noexcept -> slot* {
        return id.valid() && id.slot < Capacity && slots_[id.slot].gen == id.gen ? &slots_[id.slot] : nullptr;
    }
    auto active(task_id id) noexcept -> record* {
        auto* s = find(id);
        return s ? std::get_if<record>(&s->value) : nullptr;
    }
    auto build(record& r, task_id id, package& pkg,
               const libk::pkg_mod& mod, const spawn_opts& o) noexcept -> status_t {
        if (!cpus_ || !domain_ || !o.va.stack.valid()
            || !window{o.va.boot, 4096}.valid() || !window{o.va.ipc, 4096}.valid()
            || o.va.stack.overlaps({o.va.boot, 4096}) || o.va.stack.overlaps({o.va.ipc, 4096})
            || window{o.va.boot, 4096}.overlaps({o.va.ipc, 4096})) return STATUS_BAD_ARGS;
        // Layout policy is a caller input, but it cannot overlap an ELF segment.
        for (size_t i = 0; i < mod.segment_count(); ++i) {
            const auto decoded = mod.segment(i);
            if (!decoded) return STATUS_BAD_ARGS;
            const auto& seg = *decoded;
            const window w{seg.va, window::round_size(seg.size)};
            if (!w.valid() || w.overlaps(o.va.stack) || w.overlaps({o.va.boot, 4096})
                || w.overlaps({o.va.ipc, 4096})) return STATUS_BAD_ARGS;
        }
        auto s = r.open(pool_, o.resources);
        if (s != STATUS_OK) return s;
        sys::mapping scratch;
        // This mapping belongs to the parent, never to the child pool.
        s = scratch.open(parent_vm_, o.scratch, page_size);
        if (s != STATUS_OK) return s;
        loader load{r.pool(), r.vspace(), scratch, pkg};
        s = load.map(mod);
        if (s != STATUS_OK) return s;
        auto stack = load.anonymous(o.va.stack.address, o.va.stack.size, VM_READ | VM_WRITE);
        if (!stack) return stack.error();
        auto ipc = load.anonymous(o.va.ipc, 4096, VM_READ | VM_WRITE);
        if (!ipc) return ipc.error();
        s = sys::memory_populate(ipc->selector(), 0).status;
        if (s != STATUS_OK) return s;
        auto envelope = load.anonymous(o.va.boot, 4096, VM_READ);
        if (!envelope) return envelope.error();
        auto events = load.adopt(sys::notification_create(r.pool(), boot::event_badge));
        if (!events) return events.error();
        auto ended = load.adopt(o.events ? sys::cap_duplicate(o.events, 0, RIGHT_SIGNAL)
                                        : sys::notification_create(r.pool(), 1));
        if (!ended) return ended.error();
        r.ended_ = std::move(*ended);
        r.close_events_ = o.close_badge ? o.events : 0;
        r.close_badge_ = o.close_badge;
        boot_.fill(0);
        auto& hdr = *reinterpret_cast<BootHdr*>(boot_.data());
        hdr = {};
        hdr.magic = BOOT_MAGIC;
        hdr.major = BOOT_MAJOR;
        hdr.minor = BOOT_MINOR;
        hdr.cpu_count = cpus_;
        hdr.stack_base = o.va.stack.address;
        hdr.stack_size = o.va.stack.size;
        hdr.boot_bundle_size = std::any_of(o.caps.begin(), o.caps.end(),
            [](const auto& src) { return src.role == BOOT_BUNDLE; }) ? pkg.size() : 0;
        auto* entries = reinterpret_cast<BootCap*>(boot_.data() + sizeof(hdr));
        const auto add = [&](BootCap entry, sys::SysResult copied) noexcept -> status_t {
            if (copied.status != STATUS_OK) return copied.status;
            if (!copied.value) return STATUS_INVALID_CAP;
            if (sizeof(hdr) + (hdr.count + 1) * sizeof(BootCap) > boot_.size()) return STATUS_BAD_ARGS;
            entry.handle = copied.value;
            entries[hdr.count++] = entry;
            return STATUS_OK;
        };
        const auto local = [&](uint32_t role, cap_t cap, word_t rights) noexcept {
            BootCap entry{};
            entry.role = role;
            entry.kind = boot_kind(role);
            return add(entry, sys::cap_duplicate(cap, r.cspace(), rights));
        };
        for (auto [role, cap, rights] : std::array{
            std::tuple{uint32_t{BOOT_POOL}, r.pool(), o.resources.pool_perms},
            std::tuple{uint32_t{BOOT_VSPACE}, r.vspace(), word_t{RIGHT_DELEGATE | RIGHT_MAP | RIGHT_PROTECT | RIGHT_UNMAP | RIGHT_DESTROY}},
            std::tuple{uint32_t{BOOT_CSPACE}, r.cspace(), word_t{RIGHT_MANAGE}},
            std::tuple{uint32_t{BOOT_EVENTS}, events->selector(), word_t{RIGHT_SIGNAL | RIGHT_RECEIVE | RIGHT_DUPLICATE}}}) {
            s = local(role, cap, rights);
            if (s != STATUS_OK) return s;
        }
        // One reusable desc carrier supplies typed derivation and ThreadInit.
        auto desc = load.memory(4096);
        if (!desc) return desc.error();
        for (const auto& src : o.caps) {
            if (!src.cap) return STATUS_BAD_ARGS;
            const auto* view = std::get_if<CapView>(&src.limit);
            const auto kind = src.role ? boot_kind(src.role) : src.contract.kind;
            if (view && (view->version != CAP_ATTENUATION_VERSION_CURRENT
                || view->size != CAP_ATTENUATION_SIZE || view->kind != kind)) return STATUS_BAD_ARGS;
            const auto rights = view ? view->rights : std::get<word_t>(src.limit);
            BootCap entry{};
            entry.role = src.role;
            entry.kind = kind;
            if (src.role) {
                if (!kind) return STATUS_BAD_ARGS;
            } else {
                if (!src.contract.name || !kind) return STATUS_BAD_ARGS;
                const std::string_view name{src.contract.name};
                if (name.empty() || name.size() >= BOOT_NAME_MAX) return STATUS_BAD_ARGS;
                entry.contract.protocol = src.contract.protocol;
                entry.contract.major = src.contract.major;
                entry.contract.minor = src.contract.minor;
                std::copy(name.begin(), name.end(), entry.contract.name);
            }
            sys::SysResult copied;
            if (src.badge) copied = sys::channel_mint(src.cap, r.cspace(), src.badge, rights);
            else if (!view) copied = sys::cap_duplicate(src.cap, r.cspace(), rights);
            else {
                std::array<uint8_t, CAP_ATTENUATION_SIZE> wire{};
                sys::encode_cap(*view, wire);
                s = load.write(desc->selector(), 4096, 0, wire.data(), wire.size());
                if (s != STATUS_OK) return s;
                copied = sys::cap_typed_delegate(src.cap, r.cspace(), desc->selector());
            }
            s = add(entry, copied);
            if (s != STATUS_OK) return s;
        }
        hdr.size = sizeof(hdr) + hdr.count * sizeof(BootCap);
        if (!boot::BootView::parse(boot_.data(), hdr.size)) return STATUS_BAD_ARGS;
        const auto arg_size = o.args ? o.args->size() : 0;
        if (arg_size > boot_.size() - hdr.size) return STATUS_BAD_ARGS;
        s = load.write(envelope->selector(), 4096, 0, boot_.data(), hdr.size);
        if (s == STATUS_OK && arg_size)
            s = load.write(envelope->selector(), 4096, hdr.size, o.args->data(), arg_size);
        if (s == STATUS_OK) s = sys::memory_seal(envelope->selector()).status;
        if (s != STATUS_OK) return s;
        ThreadInit init{};
        init.version = THREAD_START_VERSION;
        init.entry = mod.entry;
        init.stack = o.va.stack.address + o.va.stack.size;
        init.arguments[0] = o.va.boot;
        init.arguments[1] = 4096;
        init.arguments[2] = arg_size ? o.va.boot + hdr.size : 0;
        init.arguments[3] = arg_size;
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
    tasks() = default;
    tasks(const tasks&) = delete;
    auto operator=(const tasks&) -> tasks& = delete;
    ~tasks() {
        for (const auto& s : slots_) if (!std::holds_alternative<std::monostate>(s.value)) sys::exit(STATUS_BUSY);
    }
    void open(const boot::BootView& boot) noexcept {
        pool_ = boot.selector(BOOT_POOL);
        domain_ = boot.selector(BOOT_DOMAIN);
        parent_vm_ = boot.selector(BOOT_VSPACE);
        cpus_ = boot.cpu_count();
    }
    auto spawn(package& pkg, std::string_view name, const spawn_opts& opts = {}) noexcept -> spawn_result {
        const auto mod = pkg.find(name);
        if (!mod || !mod->executable || (opts.close_badge && !opts.events)) return {{}, STATUS_BAD_ARGS};
        for (uint32_t i = 0; i < Capacity; ++i) {
            auto& slot = slots_[i];
            if (!slot.gen || !std::holds_alternative<std::monostate>(slot.value)) continue;
            const task_id id{i, slot.gen};
            auto& r = slot.value.template emplace<record>();
            const auto s = build(r, id, pkg, *mod, opts);
            if (s != STATUS_OK) { r.closing_ = true; r.result_ = s; (void)poll(id); }
            return {id, s};
        }
        return {};
    }
    auto observe(task_id id) noexcept -> sys::SysResult {
        auto* r = active(id);
        if (r && r->running_) return sys::exit_query(r->thread_.selector());
        auto* s = find(id);
        if (s) if (const auto* e = std::get_if<task_exit>(&s->value))
            return {.status = STATUS_OK, .value = 1, .value2 = static_cast<word_t>(e->status)};
        return {.status = STATUS_INVALID_CAP};
    }
    auto poll(task_id id) noexcept -> status_t {
        auto* s = find(id);
        if (!s) return STATUS_INVALID_CAP;
        if (std::holds_alternative<task_exit>(s->value)) return STATUS_OK;
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
        s->value.template emplace<task_exit>(task_exit{r->result_});
        return STATUS_OK;
    }
    auto request_stop(task_id id) noexcept -> status_t {
        auto* s = find(id);
        if (!s) return STATUS_INVALID_CAP;
        if (std::holds_alternative<task_exit>(s->value)) return STATUS_OK;
        auto* r = active(id);
        if (!r) return STATUS_INVALID_CAP;
        if (r->closing_) return STATUS_OK;
        const auto result = sys::exit_query(r->thread_.selector());
        if (result.status != STATUS_OK) return result.status;
        r->closing_ = true;
        r->result_ = result.value ? static_cast<status_t>(result.value2) : STATUS_CANCELED;
        return STATUS_OK;
    }
    void notify(task_id id, word_t badges) noexcept {
        auto* r = active(id);
        if (r && r->drain_ == record::drain::waiting && (badges & r->close_badge_)) r->drain_ = record::drain::done;
    }
    auto closing_needs_poll(task_id id) noexcept -> bool {
        auto* r = active(id);
        return r && r->closing_ && r->drain_ != record::drain::waiting;
    }
    auto result(task_id id) noexcept -> std::optional<task_exit> {
        auto* s = find(id);
        if (s) if (const auto* e = std::get_if<task_exit>(&s->value)) return *e;
        return {};
    }
    auto collect(task_id id) noexcept -> sys::SysResult {
        const auto status = poll(id);
        if (status != STATUS_OK) return {.status = status};
        auto* s = find(id);
        const auto result = std::get<task_exit>(s->value).status;
        s->value.template emplace<std::monostate>();
        s->gen = s->gen == UINT32_MAX ? 0 : s->gen + 1;
        return {.status = STATUS_OK, .value = static_cast<word_t>(result)};
    }
};
} // namespace proc
