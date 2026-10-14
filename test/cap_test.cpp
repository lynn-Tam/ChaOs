#include <expected>
#include <libk/scope_guard.hpp>
#include <array>
#include <optional>
#include <variant>
#include <test/test.hpp>

#include <cap/cap.hpp>
#include <cap/cspace.hpp>
#include <cap/grant.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <utility>
#include <mm/pmm.hpp>
#include <object/pool.hpp>
#include <boot/info.hpp>
#include <sched/sc.hpp>
#include <task/thread.hpp>
#include <uapi/mem.h>

namespace {

using cap::View;
using cap::CSpace;
using cap::Attenuation;
using cap::AttenuationError;
using cap::GrantError;
using cap::Graph;
using cap::GrantRef;
using cap::Right;
using cap::Rights;

// These are wire contracts, including the ordinal-to-mask MM projection.
static_assert(Rights::of(Right::Inspect).raw() == RIGHT_INSPECT);
static_assert(Rights::parse(RIGHT_MASK, RIGHT_MASK));
static_assert(!Rights::parse(u64{1} << 63, RIGHT_MASK));
static_assert(mm::Perms::of(mm::Perm::Read, mm::Perm::Write).raw()
    == (VM_READ | VM_WRITE));

constexpr usize cap_test_page_count = 96;
alignas(mm::page_size) byte
    cap_test_ram[cap_test_page_count * mm::page_size]{};
constinit libk::manual_lifetime<mm::RegionList> cap_test_map{};
constinit libk::manual_lifetime<mm::Pmm> cap_test_pmm{};
constinit libk::manual_lifetime<WorkQueue> cap_work{};
constinit libk::manual_lifetime<object::pool<sched::Sc>> cap_test_contexts{};
constinit libk::manual_lifetime<object::pool<object::group>> cap_test_groups{};
constinit libk::manual_lifetime<object::pool<cap::CSpace>> cap_test_cspaces{};

constinit libk::manual_lifetime<Graph> cap_test_graph{};
constinit libk::manual_lifetime<CSpace> cap_test_space_a{};
constinit libk::manual_lifetime<CSpace> cap_test_space_b{};
constinit libk::manual_lifetime<CSpace> cap_test_space_one{};
constinit libk::manual_lifetime<cap::Batch> cap_test_transfer{};

constexpr Rights inspect_rights = Rights::of(Right::Inspect);
constexpr Rights basic_rights = Rights::of(
    Right::Duplicate, Right::Delegate, Right::Inspect);
constexpr Rights all_context_rights = Rights::of(
    Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control);

constexpr Rights attenuation_rights = Rights::of(
    Right::Duplicate, Right::Delegate, Right::Inspect);

void put16(byte* bytes, usize offset, u16 value) noexcept {
    bytes[offset] = static_cast<byte>(value);
    bytes[offset + 1] = static_cast<byte>(value >> 8);
}

void put32(byte* bytes, usize offset, u32 value) noexcept {
    for (usize index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<byte>(value >> (index * 8));
    }
}

void put64(byte* bytes, usize offset, u64 value) noexcept {
    for (usize index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<byte>(value >> (index * 8));
    }
}

void attenuation_bytes(
    byte (&bytes)[CAP_ATTENUATION_SIZE],
    u16 kind,
    u64 rights = attenuation_rights.raw()) noexcept {
    for (byte& value : bytes) {
        value = 0;
    }
    put16(bytes, CAP_ATTENUATION_VERSION_OFFSET,
        CAP_ATTENUATION_VERSION_CURRENT);
    put16(bytes, CAP_ATTENUATION_KIND_OFFSET, kind);
    put32(bytes, CAP_ATTENUATION_SIZE_OFFSET,
        CAP_ATTENUATION_SIZE);
    put64(bytes, CAP_ATTENUATION_RIGHTS_OFFSET, rights);
}

struct RevokeProbe final {
    void ready() noexcept { ++signals; }
    usize signals{};
};

class CapFixture final : private libk::noncopyable_nonmovable {
public:
    CapFixture() noexcept = default;
    ~CapFixture() noexcept { reset(); }

    [[nodiscard]] auto initialize() noexcept -> bool {
        reset();
        (void)cap_work.emplace();
        const auto physical = boot_layout.phys(mm::va_t{
            reinterpret_cast<usize>(cap_test_ram)});
        if (!physical) {
            return false;
        }
        const auto first = mm::Page::from_base(*physical);
        if (!first) {
            return false;
        }
        auto& map = cap_test_map.emplace();
        if (!map.try_emplace_back(mm::Region{
                mm::Pages{*first, cap_test_page_count},
                mm::Region::Kind::Ram})) {
            reset();
            return false;
        }
        if (!mm::Pmm::initialize_in(
                cap_test_pmm, std::move(map), mm::Pmm::Window{
                .pa = *physical,
                .va = mm::va_t{
                    reinterpret_cast<usize>(cap_test_ram)},
                .size = sizeof(cap_test_ram),
            })) {
            reset();
            return false;
        }
        cap_test_map.reset();
        (void)cap_test_contexts.emplace(*cap_test_pmm, *cap_work);
        (void)cap_test_groups.emplace(*cap_test_pmm, *cap_work);
        (void)cap_test_cspaces.emplace(*cap_test_pmm, *cap_work);
        [[maybe_unused]] auto& graph =
            cap_test_graph.emplace(*cap_test_pmm, *cap_work);
        [[maybe_unused]] auto& space_a =
            cap_test_space_a.emplace(*cap_test_pmm);
        [[maybe_unused]] auto& space_b =
            cap_test_space_b.emplace(*cap_test_pmm);
        [[maybe_unused]] auto& space_one = cap_test_space_one.emplace(
            *cap_test_pmm,
            CSpace::Quota{.slots = 1, .pages = 3});

        for (usize index = 0; index < 2; ++index) {
            auto pending = cap_test_contexts->create(
                sched::Sc::Config{
                    .budget = index + 1,
                    .period = 10,
                },
                0);
            if (!pending) {
                reset();
                return false;
            }
            targets_[index] = std::move(pending).value().publish();
        }
        return true;
    }

    auto pending_target(usize index) noexcept {
        libk_assert(targets_[index].destroy());
        targets_[index].reset();
        drain();
        auto pending = cap_test_contexts->create(sched::Sc::Config{
            .budget = 1, .period = 10},
            0);
        if (pending) targets_[index] = pending->reference();
        return pending;
    }

    [[nodiscard]] auto root(
        usize target,
        Rights rights = all_context_rights) noexcept
        -> std::expected<GrantRef, GrantError> {
        if (target >= 2 || !targets_[target]) {
            return std::unexpected(GrantError::InvalidKey);
        }
        auto reference = object::ref<>{targets_[target]};
        if (!reference) {
            return std::unexpected(GrantError::InvalidState);
        }
        return graph().create_root(
            std::move(reference),
            View{rights});
    }

    [[nodiscard]] auto graph() noexcept -> Graph& {
        return *cap_test_graph;
    }
    [[nodiscard]] auto a() noexcept -> CSpace& { return *cap_test_space_a; }
    [[nodiscard]] auto b() noexcept -> CSpace& { return *cap_test_space_b; }
    [[nodiscard]] auto one() noexcept -> CSpace& {
        return *cap_test_space_one;
    }
    [[nodiscard]] auto target(usize index) noexcept
        -> sched::Sc& {
        return targets_[index].get();
    }
    [[nodiscard]] auto target_ref(usize index) noexcept -> object::ref<> {
        libk_assert(index < 2);
        return targets_[index];
    }
    void drain() noexcept { while (cap_work->run()) {} }
    auto settled_grants() noexcept -> usize { drain(); return graph().live_count(); }
    void drop_retired_target(usize index) noexcept {
        libk_assert(index < 2 && targets_[index]);
        targets_[index].reset();
        drain();
    }
    [[nodiscard]] auto retire_target(usize index) noexcept -> bool {
        libk_assert(index < 2 && targets_[index]);
        const auto id = targets_[index].id();
        if (!targets_[index].destroy()) {
            return false;
        }
        targets_[index].reset();
        drain();
        return !cap_test_contexts->lookup(id);
    }

private:
    void reset() noexcept {
        cap_test_transfer.reset();
        if (cap_test_space_one) {
            cap_test_space_one->retire();
            cap_test_space_one.reset();
        }
        if (cap_test_space_b) {
            cap_test_space_b->retire();
            cap_test_space_b.reset();
        }
        if (cap_test_space_a) {
            cap_test_space_a->retire();
            cap_test_space_a.reset();
        }
        if (cap_test_graph) drain();
        cap_test_graph.reset();
        for (auto& target : targets_) {
            if (target) {
                libk_assert(target.destroy());
                target.reset();
            }
        }
        if (cap_test_contexts) {
            drain();
        }
        cap_test_cspaces.reset();
        cap_test_contexts.reset();
        cap_test_groups.reset();
        cap_work.reset();
        cap_test_pmm.reset();
        cap_test_map.reset();
    }

    object::ref<sched::Sc> targets_[2]{};
};

[[nodiscard]] auto decode_and_check(
    ObjKind kind,
    const View& source,
    byte (&bytes)[CAP_ATTENUATION_SIZE]) noexcept -> bool {
    auto decoded = cap::decode_attenuation(
        std::span<const byte>{bytes, sizeof(bytes)});
    if (!decoded) {
        return false;
    }
    auto ceiling = cap::make_attenuation_ceiling(
        kind, source, decoded.value());
    return ceiling
        && cap::validate_ceiling(kind, ceiling.value())
        && cap::attenuates(kind, source, ceiling.value());
}

bool test_typed_attenuation_covers_all_families(
    const TestContext&) noexcept {
    using cap::ChanLimit;
    using cap::ChannelSide;
    using cap::IrqRoute;
    using cap::MemLimit;
    using cap::Badge;
    using cap::Quota;
    using cap::VmLimit;

    byte bytes[CAP_ATTENUATION_SIZE]{};
    const View simple{attenuation_rights, std::monostate{}};
    const ObjKind simple_kinds[] = {
        ObjThread,
        ObjSc,
        ObjDomain,
        ObjCSpace};
    for (const ObjKind kind : simple_kinds) {
        attenuation_bytes(bytes, static_cast<u16>(kind));
        if (!decode_and_check(kind, simple, bytes)) {
            return false;
        }
    }

    const View memory{
        attenuation_rights,
        MemLimit{
            mm::ObjectRange{0, 16},
            mm::Perms::of(mm::Perm::Read)}};
    attenuation_bytes(bytes, ObjMem);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET, 1);
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 2);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET,
        static_cast<u64>(VM_READ));
    if (!decode_and_check(ObjMem, memory, bytes)) {
        return false;
    }

    const View vspace{
        attenuation_rights,
        VmLimit{
            mm::VRange{mm::va_t{0x1000}, 0x10000},
            mm::Perms::of(mm::Perm::Read)}};
    attenuation_bytes(bytes, ObjVSpace);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET, 0x2000);
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 0x2000);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET, VM_READ);
    if (!decode_and_check(ObjVSpace, vspace, bytes)) {
        return false;
    }

    const View pool{
        attenuation_rights,
        Quota{
            resource::budget{1024 * 1024, 16},
            OBJ_KINDS}};
    attenuation_bytes(bytes, ObjGroup);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET, 4096);
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 4);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET,
        (u64{1} << ObjMem));
    if (!decode_and_check(ObjGroup, pool, bytes)) {
        return false;
    }

    const View notification{
        attenuation_rights, Badge{7}};
    attenuation_bytes(bytes, ObjNotification);
    if (!decode_and_check(ObjNotification, notification, bytes)) {
        return false;
    }

    const View channel{
        attenuation_rights, ChanLimit{ChannelSide::Any, 0, 0}};
    attenuation_bytes(bytes, ObjChannel);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET,
        CAP_CHANNEL_SIDE_A);
    if (!decode_and_check(ObjChannel, channel, bytes)) {
        return false;
    }

    const View pager{
        attenuation_rights};
    attenuation_bytes(bytes, ObjPager);
    if (!decode_and_check(ObjPager, pager, bytes)) {
        return false;
    }

    const View irq{
        attenuation_rights, IrqRoute{4, true}};
    attenuation_bytes(bytes, ObjIrq);
    return decode_and_check(ObjIrq, irq, bytes);
}

bool test_typed_attenuation_rejects_malformed_and_amplifying(
    const TestContext&) noexcept {
    using cap::ChanLimit;
    using cap::ChannelSide;
    using cap::MemLimit;

    byte bytes[CAP_ATTENUATION_SIZE]{};
    attenuation_bytes(bytes, ObjThread);
    bytes[CAP_ATTENUATION_VERSION_OFFSET] = 2;
    if (cap::decode_attenuation(
            std::span<const byte>{bytes, sizeof(bytes)})) {
        return false;
    }
    attenuation_bytes(bytes, ObjCount);
    if (cap::decode_attenuation(
            std::span<const byte>{bytes, sizeof(bytes)})) {
        return false;
    }
    attenuation_bytes(bytes, ObjThread);
    put64(bytes, CAP_ATTENUATION_WORD5_OFFSET, 1);
    auto decoded = cap::decode_attenuation(
        std::span<const byte>{bytes, sizeof(bytes)});
    if (!decoded) {
        return false;
    }
    const View simple{attenuation_rights, std::monostate{}};
    if (cap::make_attenuation_ceiling(
            ObjThread, simple, decoded.value())) {
        return false;
    }
    attenuation_bytes(bytes, ObjMem);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET, ~u64{0});
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 2);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET, VM_READ);
    decoded = cap::decode_attenuation(
        std::span<const byte>{bytes, sizeof(bytes)});
    const View memory{
        attenuation_rights,
        MemLimit{
            mm::ObjectRange{0, 16},
            mm::Perms::of(mm::Perm::Read)}};
    if (!decoded || cap::make_attenuation_ceiling(
                         ObjMem, memory, decoded.value())) {
        return false;
    }
    attenuation_bytes(bytes, ObjChannel);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET,
        CAP_CHANNEL_SIDE_A);
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 1);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET, ~u64{0});
    decoded = cap::decode_attenuation(
        std::span<const byte>{bytes, sizeof(bytes)});
    const View channel{
        attenuation_rights, ChanLimit{ChannelSide::Any, 0, 0}};
    if (!decoded) {
        return false;
    }
    auto channel_ceiling = cap::make_attenuation_ceiling(
        ObjChannel, channel, decoded.value());
    return channel_ceiling
        && !cap::attenuates(
            ObjChannel, channel, channel_ceiling.value());
}

bool test_typed_delegate_transaction_rolls_back(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root = fixture.root(0);
    if (!root) {
        return false;
    }
    auto source = fixture.a().insert(
        std::move(root).value(), View{basic_rights});
    if (!source) {
        return false;
    }
    byte bytes[CAP_ATTENUATION_SIZE]{};
    attenuation_bytes(bytes, ObjSc);
    auto decoded = cap::decode_attenuation(
        std::span<const byte>{bytes, sizeof(bytes)});
    if (!decoded) {
        return false;
    }
    const usize before_slots = fixture.b().live_slots();
    const usize before_grants = fixture.settled_grants();
    auto child = fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Derive, decoded.value());
    if (!child || fixture.b().live_slots() != before_slots + 1
        || fixture.settled_grants() != before_grants + 1) {
        return false;
    }
    {
        auto resolved = fixture.b().resolve<
            sched::Sc>(child.value(), inspect_rights);
        if (!resolved) {
            return false;
        }
    }
    if (!fixture.b().close(child.value())) {
        return false;
    }
    const usize stable_slots = fixture.b().live_slots();
    const usize stable_grants = fixture.settled_grants();

    // Fill the bounded destination so the production transaction derives its
    // child Grant before destination reservation fails.  Graph and CSpace
    // must return to the exact pre-call counts, not merely reject early.
    auto occupied = fixture.a().transfer(source.value(), fixture.one(), cap::XferOp::Copy, basic_rights);
    if (!occupied || fixture.one().live_slots() != 1) {
        return false;
    }
    const usize full_slots = fixture.one().live_slots();
    const usize full_grants = fixture.settled_grants();
    attenuation_bytes(bytes, ObjSc);
    decoded = cap::decode_attenuation(
        std::span<const byte>{bytes, sizeof(bytes)});
    if (!decoded
        || fixture.a().transfer(source.value(), fixture.one(), cap::XferOp::Derive, decoded.value())
        || fixture.one().live_slots() != full_slots
        || fixture.settled_grants() != full_grants
        || !fixture.one().close(occupied.value())) {
        return false;
    }

    attenuation_bytes(bytes, ObjThread);
    decoded = cap::decode_attenuation(
        std::span<const byte>{bytes, sizeof(bytes)});
    if (!decoded
        || fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Derive, decoded.value())
        || fixture.b().live_slots() != stable_slots
        || fixture.settled_grants() != stable_grants) {
        return false;
    }
    attenuation_bytes(bytes, ObjSc,
        static_cast<u64>(RIGHT_CONTROL));
    decoded = cap::decode_attenuation(
        std::span<const byte>{bytes, sizeof(bytes)});
    if (!decoded
        || fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Derive, decoded.value())
        || fixture.b().live_slots() != stable_slots
        || fixture.settled_grants() != stable_grants) {
        return false;
    }
    return fixture.a().close(source.value())
        && !fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Derive, decoded.value());
}

bool test_resolve_composes_authority_and_pins_kind(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root = fixture.root(0);
    if (!root) {
        return false;
    }
    auto inserted = fixture.a().insert(
        std::move(root).value(), View{basic_rights});
    if (!inserted) {
        return false;
    }
    const auto handle = inserted.value();
    {
        auto resolved = fixture.a().resolve<
            sched::Sc>(handle, inspect_rights);
        auto wrong_kind = fixture.a().resolve<Thread>(
            handle, inspect_rights);
        auto denied = fixture.a().resolve<
            sched::Sc>(
                handle, Rights::of(Right::Control));
        if (!resolved
            || &resolved.value().object() != &fixture.target(0)
            || !resolved.value().rights().contains(basic_rights)
            || wrong_kind
            || wrong_kind.error() != cap::CSpaceError::WrongKind
            || denied
            || denied.error() != cap::CSpaceError::Denied) {
            return false;
        }
        auto other = fixture.target_ref(1);
        if (!other) return false;
        auto replaced = fixture.graph().derive(
            resolved.value().lease(), std::move(other), View{inspect_rights});
        if (replaced || replaced.error() != cap::GrantError::InvalidKey
            || fixture.settled_grants() != 1) return false;
    }
    return fixture.a().close(handle)
        && fixture.settled_grants() == 0;
}

bool test_duplicate_attenuates_without_splitting_grant(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root = fixture.root(0);
    if (!root) {
        return false;
    }
    auto source = fixture.a().insert(
        std::move(root).value(), View{basic_rights});
    if (!source) {
        return false;
    }
    auto amplified = fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Copy, View{Rights::of(Right::Control)});
    if (amplified
        || amplified.error() != cap::CSpaceError::Amplification) {
        return false;
    }
    auto copy = fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Copy, View{inspect_rights});
    if (!copy) {
        return false;
    }
    {
        auto original = fixture.a().resolve<
            sched::Sc>(source.value(), inspect_rights);
        auto duplicate = fixture.b().resolve<
            sched::Sc>(copy.value(), inspect_rights);
        auto denied = fixture.b().resolve<
            sched::Sc>(
                copy.value(), Rights::of(Right::Delegate));
        if (!original || !duplicate
            || original.value().grant() != duplicate.value().grant()
            || denied
            || denied.error() != cap::CSpaceError::Denied) {
            return false;
        }
    }
    return fixture.b().close(copy.value())
        && fixture.a().close(source.value())
        && fixture.settled_grants() == 0;
}

bool test_delegation_revoke_waits_for_existing_lease(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root = fixture.root(0);
    if (!root) {
        return false;
    }
    const auto root_key = root.value().key();
    auto source = fixture.a().insert(
        std::move(root).value(), View{all_context_rights});
    if (!source) {
        return false;
    }
    auto child = fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Derive, View{inspect_rights});
    if (!child) {
        return false;
    }

    RevokeProbe probe{};
    cap::GrantRevoke completion{
        sync::Latch::Notifier::bind<
            &RevokeProbe::ready>(probe)};
    RevokeProbe closed_probe{};
    cap::GrantRevoke closed{sync::Latch::Notifier::bind<&RevokeProbe::ready>(closed_probe)};
    libk::scope_exit drained{[&]() noexcept { fixture.drain(); }};
    cap::GrantKey child_key{};
    {
        auto active_result = fixture.b().resolve<
            sched::Sc>(child.value(), inspect_rights);
        if (!active_result) {
            return false;
        }
        auto active = std::move(active_result).value();
        child_key = active.grant();
        if (!fixture.graph().revoke_descendants(root_key, completion)
            || completion.complete() || !completion.arm()) {
            return false;
        }
        auto rejected = fixture.b().resolve<
            sched::Sc>(child.value(), inspect_rights);
        auto source_live = fixture.a().resolve<
            sched::Sc>(source.value(), inspect_rights);
        fixture.graph().close(root_key, closed);
        if (closed.complete() || closed_probe.signals != 0) return false;
        if (rejected
            || rejected.error()
                != cap::CSpaceError::GrantUnavailable
            || !source_live) {
            return false;
        }
    }
    fixture.drain();
    const auto child_state = fixture.graph().state(child_key);
    if (!closed.complete() || closed_probe.signals != 1
        || !completion.complete() || probe.signals != 1
        || child_state
        || child_state.error() != GrantError::InvalidKey) {
        return false;
    }
    return fixture.b().close(child.value())
        && fixture.a().close(source.value())
        && fixture.settled_grants() == 0;
}

bool test_close_drains_orphan_operation(const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) return false;
    auto root = fixture.root(0);
    if (!root) return false;
    const auto key = root->key();
    auto held = root->acquire();
    if (!held) return false;
    root->reset();
    RevokeProbe probe{};
    cap::GrantRevoke close{sync::Latch::Notifier::bind<&RevokeProbe::ready>(probe)};
    fixture.graph().close(key, close);
    if (close.complete() || probe.signals) return false;
    held->reset();
    fixture.drain();
    if (!close.complete() || probe.signals != 1 || fixture.settled_grants()) return false;
    RevokeProbe stale_probe{};
    cap::GrantRevoke stale{sync::Latch::Notifier::bind<&RevokeProbe::ready>(stale_probe)};
    fixture.graph().close(key, stale);
    return stale.complete() && stale_probe.signals == 1;
}

bool test_attachment_detach_transfers_quiescence_once(const TestContext&) noexcept {
    struct Probe {
        cap::GrantWork work;
        usize releases{};
        cap::GrantAttachment attachment;
        explicit Probe(const cap::GrantAttachmentOps& ops) noexcept : attachment(this, ops) {}
        ~Probe() noexcept {
            if (attachment.attached()) (void)attachment.detach();
            work.reset();
        }
    };
    static constexpr cap::GrantAttachmentOps ops{
        .invalidate = [](void* context, cap::GrantWork&& work) noexcept {
            static_cast<Probe*>(context)->work = std::move(work);
        },
        .released = [](void* context) noexcept { ++static_cast<Probe*>(context)->releases; },
    };
    CapFixture fixture;
    if (!fixture.initialize()) return false;
    for (unsigned order = 0; order < 3; ++order) {
        auto root = fixture.root(0);
        if (!root) return false;
        cap::GrantRevoke revoke;
        libk::scope_exit drained{[&]() noexcept { fixture.drain(); }};
        Probe probe{ops};
        auto lease = root.value().acquire();
        if (!lease || !lease.value().attach(probe.attachment)) return false;
        lease.value().reset();
        if (order != 0) {
            if (!fixture.graph().invalidate(root.value().key(), revoke)) return false;
            fixture.drain();
            if (!probe.work || !probe.attachment.busy()) return false;
            if (order == 2) probe.work.reset();
        }
        const bool synchronous = probe.attachment.detach();
        if (synchronous != (order != 1) || probe.releases != 0) return false;
        probe.work.reset();
        fixture.drain();
        if (probe.releases != (order == 1 ? 1U : 0U)
            || probe.attachment.attached() || probe.attachment.busy()
            || (order != 0 && !revoke.complete())) return false;
    }
    return fixture.settled_grants() == 0;
}

bool test_cap_batch_has_one_publication(const TestContext&) noexcept {
    CapFixture f;
    if (!f.initialize()) return false;
    for (bool foreign : {true, false}) {
        std::array<CSpace::NewCap, 2> caps;
        std::array<cap::Handle, 2> ids;
        for (usize i = 0; i < caps.size(); ++i) {
            auto grant = f.root(i);
            auto slot = foreign && i == 1 ? f.b().reserve() : f.a().reserve();
            if (!grant || !slot) return false;
            auto cap = f.a().prepare(std::move(*slot), std::move(*grant), View{inspect_rights});
            if (!cap) return false;
            ids[i] = cap->handle();
            caps[i] = std::move(*cap);
        }
        auto done = f.a().insert(std::span{caps});
        if (foreign) {
            if (done || done.error() != cap::CSpaceError::InvalidState
                || f.a().resolve<sched::Sc>(ids[0], inspect_rights)) return false;
        } else {
            if (!done) return false;
            for (usize i = 0; i < caps.size(); ++i) {
                auto target = f.a().resolve<sched::Sc>(ids[i], inspect_rights);
                if (!target || &target->object() != &f.target(i)) return false;
                if (!f.a().close(ids[i])) return false;
            }
        }
    }
    return f.settled_grants() == 0;
}

bool test_handles_are_local_and_stale_generation_stays_dead(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root_a = fixture.root(0);
    auto root_b = fixture.root(1);
    if (!root_a || !root_b) {
        return false;
    }
    auto handle_a = fixture.a().insert(
        std::move(root_a).value(), View{inspect_rights});
    auto handle_b = fixture.b().insert(
        std::move(root_b).value(), View{inspect_rights});
    if (!handle_a || !handle_b
        || handle_a.value().raw() != handle_b.value().raw()) {
        return false;
    }
    {
        auto resolved_a = fixture.a().resolve<
            sched::Sc>(handle_a.value(), inspect_rights);
        auto resolved_b = fixture.b().resolve<
            sched::Sc>(handle_b.value(), inspect_rights);
        if (!resolved_a || !resolved_b
            || &resolved_a.value().object() != &fixture.target(0)
            || &resolved_b.value().object() != &fixture.target(1)) {
            return false;
        }
    }
    const auto stale = handle_a.value();
    if (!fixture.a().close(stale)) {
        return false;
    }
    auto replacement_root = fixture.root(0);
    if (!replacement_root) {
        return false;
    }
    auto replacement = fixture.a().insert(
        std::move(replacement_root).value(), View{inspect_rights});
    auto rejected = fixture.a().resolve<
        sched::Sc>(stale, inspect_rights);
    if (!replacement
        || replacement.value().index() != stale.index()
        || replacement.value().generation() == stale.generation()
        || rejected
        || rejected.error() != cap::CSpaceError::InvalidHandle) {
        return false;
    }
    return fixture.a().close(replacement.value())
        && fixture.b().close(handle_b.value())
        && fixture.settled_grants() == 0;
}

bool test_remote_selector_close_is_cspace_exact(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root = fixture.root(0);
    if (!root) {
        return false;
    }
    auto installed = fixture.b().insert(
        std::move(root).value(), View{inspect_rights});
    if (!installed) {
        return false;
    }
    const auto selector = installed.value();
    auto wrong_space = fixture.a().close(selector);
    if (wrong_space
        || wrong_space.error() != cap::CSpaceError::InvalidHandle) {
        return false;
    }
    if (!fixture.b().close(selector)) {
        return false;
    }
    auto stale = fixture.b().close(selector);
    return !stale
        && stale.error() == cap::CSpaceError::InvalidState
        && fixture.settled_grants() == 0;
}

bool test_move_is_transactional_across_cspaces(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto source_root = fixture.root(0);
    auto blocker_root = fixture.root(1);
    if (!source_root || !blocker_root) {
        return false;
    }
    auto source = fixture.a().insert(
        std::move(source_root).value(), View{inspect_rights});
    auto blocker = fixture.one().insert(
        std::move(blocker_root).value(), View{inspect_rights});
    if (!source || !blocker) {
        return false;
    }
    auto rejected = fixture.a().transfer(source.value(), fixture.one(), cap::XferOp::Move);
    {
        auto source_live = fixture.a().resolve<
            sched::Sc>(source.value(), inspect_rights);
        if (rejected
            || rejected.error() != cap::CSpaceError::SlotQuota
            || !source_live) {
            return false;
        }
    }
    if (!fixture.one().close(blocker.value())) {
        return false;
    }
    auto moved = fixture.a().transfer(source.value(), fixture.one(), cap::XferOp::Move);
    if (!moved) {
        return false;
    }
    auto stale = fixture.a().resolve<sched::Sc>(
        source.value(), inspect_rights);
    {
        auto destination = fixture.one().resolve<
            sched::Sc>(moved.value(), inspect_rights);
        if (stale || !destination
            || &destination.value().object() != &fixture.target(0)) {
            return false;
        }
    }
    return fixture.one().close(moved.value())
        && fixture.settled_grants() == 0;
}

bool test_ipc_transfer_commits_copy_and_move_atomically(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto copy_root = fixture.root(0);
    auto move_root = fixture.root(1);
    if (!copy_root || !move_root) {
        return false;
    }
    auto copy_source = fixture.a().insert(
        std::move(copy_root).value(), View{basic_rights});
    auto move_source = fixture.a().insert(
        std::move(move_root).value(), View{inspect_rights});
    if (!copy_source || !move_source) {
        return false;
    }

    cap::Batch::Specs specs{};
    if (!specs.try_emplace_back(cap::XferSpec{
            .source = copy_source.value(),
            .limit = inspect_rights,
            .op = cap::XferOp::Copy,
        })
        || !specs.try_emplace_back(cap::XferSpec{
            .source = move_source.value(),
            .op = cap::XferOp::Move,
        })) {
        return false;
    }
    auto& transfer = cap_test_transfer.emplace();
    if (!cap::Batch::prepare(
            transfer, fixture.a(), fixture.b(), specs)) {
        return false;
    }
    auto committed = transfer.commit();
    cap_test_transfer.reset();
    if (!committed || committed.value().size() != 2) {
        return false;
    }

    {
        auto copy_original = fixture.a().resolve<
            sched::Sc>(
                copy_source.value(), inspect_rights);
        auto stale_move = fixture.a().resolve<
            sched::Sc>(
                move_source.value(), inspect_rights);
        auto copied = fixture.b().resolve<sched::Sc>(
            committed.value()[0], inspect_rights);
        auto moved = fixture.b().resolve<sched::Sc>(
            committed.value()[1], inspect_rights);
        if (!copy_original || stale_move || !copied || !moved
            || &copied.value().object() != &fixture.target(0)
            || &moved.value().object() != &fixture.target(1)) {
            return false;
        }
    }
    return fixture.b().close(committed.value()[1])
        && fixture.b().close(committed.value()[0])
        && fixture.a().close(copy_source.value())
        && fixture.settled_grants() == 0;
}

bool test_cap_batch_rolls_back(const TestContext&) noexcept {
    for (bool stop_dst : {false, true}) {
        CapFixture fixture;
        if (!fixture.initialize()) return false;
        auto root = fixture.root(0);
        if (!root) return false;
        auto source = fixture.a().insert(std::move(*root), View{inspect_rights});
        if (!source) return false;
        cap::Batch::Specs specs;
        if (!specs.try_push_back(cap::XferSpec{.source = *source, .op = cap::XferOp::Move})) return false;
        auto& batch = cap_test_transfer.emplace();
        if (!cap::Batch::prepare(batch, fixture.a(), fixture.b(), specs)) return false;
        if (stop_dst) fixture.b().retire();
        else if (!fixture.a().close(*source)) return false;
        auto rejected = batch.commit();
        if (rejected || rejected.error() != cap::CSpaceError::InvalidState
            || fixture.b().live_slots() != 1) return false;
        cap_test_transfer.reset();
        if (stop_dst && !fixture.a().close(*source)) return false;
        if (fixture.b().live_slots() != 0 || fixture.settled_grants() != 0) return false;
    }
    return true;
}

bool test_cspace_retire_waits_for_reserved_operation(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    {
        auto reserved = fixture.a().reserve();
        if (!reserved || fixture.a().table_pages() != 3) {
            return false;
        }
        auto reservation = std::move(reserved).value();
        fixture.a().retire();
        auto stopped = fixture.a().reserve();
        if (stopped
            || stopped.error() != cap::CSpaceError::InvalidState
            || fixture.a().table_pages() != 3
            || fixture.a().live_slots() != 1) {
            return false;
        }
    }
    if (fixture.a().table_pages() != 0
        || fixture.a().live_slots() != 0) {
        return false;
    }

    auto pending = cap_test_cspaces->create(*cap_test_pmm);
    if (!pending) {
        return false;
    }
    auto held = std::move(pending).value().publish();
    const auto id = held.id();
    auto pin_result = cap_test_cspaces->lookup(id);
    if (!pin_result) {
        return false;
    }
    auto pinned = std::move(pin_result).value();
    {
        auto reserved = pinned->reserve();
        if (!reserved) {
            return false;
        }
        auto reservation = std::move(reserved).value();
        if (!held.destroy()) {
            return false;
        }
        held.reset();
        if (pinned->table_pages() != 3) {
            return false;
        }
    }
    if (pinned->table_pages() != 0) {
        return false;
    }
    pinned.reset();
    fixture.drain();
    return !cap_test_cspaces->lookup(id)
        && cap_test_pmm->verify_invariants();
}

bool test_sponsored_cspace_refunds_reusable_capacity(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }

    constexpr resource::budget limit{
        .memory = 8 * mm::page_size,
        .caps = 8,
    };
    constexpr resource::budget object_charge{
        .memory = mm::page_size,
    };
    auto made_pool = cap_test_groups->create(limit);
    if (!made_pool) {
        return false;
    }
    auto pool = std::move(made_pool).value().publish();
    auto pool_ref = object::ref<>{pool};
    if (!pool_ref) {
        return false;
    }
    auto charged = pool->reserve(
        std::move(pool_ref), object_charge);
    if (!charged) {
        return false;
    }
    auto made_space = cap_test_cspaces->create(std::move(charged).value(), *cap_test_pmm, CSpace::Quota{.slots = 8, .pages = 3});
    if (!made_space) {
        return false;
    }
    auto space = std::move(made_space).value().publish();

    auto root = fixture.root(0);
    if (!root) {
        return false;
    }
    auto installed = space->insert(
        std::move(root).value(), View{basic_rights});
    const resource::budget after_install{
        .memory = limit.memory - object_charge.memory
            - 3 * mm::page_size,
        .caps = limit.caps - 1,
    };
    if (!installed || pool->available() != after_install
        || !space->transfer(installed.value(), fixture.b(), cap::XferOp::Derive, inspect_rights)) {
        return false;
    }
    const resource::budget after_delegate{
        .memory = after_install.memory - Graph::node_charge().memory,
        .caps = after_install.caps,
    };
    if (pool->available() != after_delegate) {
        return false;
    }
    auto child = space->transfer(installed.value(), fixture.b(), cap::XferOp::Derive, inspect_rights);
    if (!child || !fixture.b().close(child.value())
        || pool->available() != after_delegate) {
        return false;
    }
    // The first delegated capability is intentionally found through the only
    // live destination slot. Closing its CSpace releases the sponsored node.
    fixture.b().retire();
    if (pool->available() != after_install
        || !space->close(installed.value())
        || pool->available().caps != limit.caps) {
        return false;
    }

    {
        auto reserved = space->reserve();
        if (!reserved || pool->available().caps != limit.caps - 1
            || !space.destroy() || space->table_pages() != 3
            || pool->close() == object::group::phase::closed) {
            return false;
        }
        // Retirement cannot refund a selector or its table while its owner
        // still holds the reservation, even when the sponsor is closing.
    }
    if (space->table_pages() != 0 || pool->available() != resource::budget{
            .memory = limit.memory - object_charge.memory, .caps = limit.caps}) {
        return false;
    }
    space.reset();
    fixture.drain();
    if (pool->available() != limit || pool->sponsorship_count() != 0
        || pool->close() != object::group::phase::closed
        || !pool.destroy()) {
        return false;
    }
    pool.reset();
    fixture.drain();
    return cap_test_pmm->verify_invariants();
}

bool test_attenuated_operations_and_revoke_use_slot_authority(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const Rights source_rights = Rights::of(
        Right::Duplicate,
        Right::Delegate,
        Right::Inspect,
        Right::Revoke);
    auto root = fixture.root(0, source_rights);
    if (!root) {
        return false;
    }
    auto source = fixture.a().insert(
        std::move(root).value(), View{source_rights});
    if (!source) {
        return false;
    }
    auto duplicate = fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Copy, inspect_rights);
    auto child = fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Derive, inspect_rights);
    if (!duplicate || !child) {
        return false;
    }
    cap::GrantRevoke completion{};
    libk::scope_exit drained{[&]() noexcept { fixture.drain(); }};
    if (!fixture.a().revoke(source.value(), completion, false)) return false;
    fixture.drain();
    if (!completion.complete()) {
        return false;
    }
    auto shared = fixture.b().resolve<sched::Sc>(
        duplicate.value(), inspect_rights);
    auto revoked = fixture.b().resolve<sched::Sc>(
        child.value(), inspect_rights);
    return shared && !revoked
        && fixture.b().close(child.value())
        && fixture.b().close(duplicate.value())
        && fixture.a().close(source.value());
}

bool test_revoked_tombstones_do_not_retain_target(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const Rights rights = Rights::of(
        Right::Delegate, Right::Inspect, Right::Revoke);
    auto root = fixture.root(0, rights);
    if (!root) {
        return false;
    }
    auto source = fixture.a().insert(
        std::move(root).value(), View{rights});
    if (!source) {
        return false;
    }
    auto child = fixture.a().transfer(source.value(), fixture.b(), cap::XferOp::Derive, inspect_rights);
    if (!child) {
        return false;
    }

    cap::GrantRevoke completion{};
    libk::scope_exit drained{[&]() noexcept { fixture.drain(); }};
    if (!fixture.a().revoke(source.value(), completion, true)) return false;
    fixture.drain();
    if (!completion.complete()
        || !fixture.retire_target(0)) {
        return false;
    }

    // Both slots deliberately remain occupied while target reclamation is
    // observed.  They are identity tombstones, not hidden object owners.
    auto source_dead = fixture.a().resolve<sched::Sc>(
        source.value(), inspect_rights);
    auto child_dead = fixture.b().resolve<sched::Sc>(
        child.value(), inspect_rights);
    return !source_dead && !child_dead
        && fixture.b().close(child.value())
        && fixture.a().close(source.value())
        && fixture.settled_grants() == 0;
}

bool pending_rollback_closes_lineage(bool deferred) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    constexpr resource::budget limit{
        .memory = 4 * mm::page_size,
        .caps = 4,
    };
    auto pending_pool = cap_test_groups->create(limit);
    if (!pending_pool) {
        return false;
    }
    auto pool = std::move(pending_pool).value().publish();
    if (!deferred) {
        auto self = object::ref<>{pool};
        auto target = fixture.target_ref(1);
        if (!self || !target) return false;
        auto fee = pool->reserve(std::move(self), Graph::node_charge());
        auto rejected = fixture.pending_target(1);
        if (!fee || !rejected || rejected->root(fixture.graph(), std::move(*fee),
                                        View{Rights::of(Right::Signal)})) return false;
        rejected->reset(); // Private target rollback before any grant exists.
        fee->reset();
        fixture.drop_retired_target(1);
        if (pool->available() != limit || fixture.settled_grants()) return false;
    }

    auto payer_ref = object::ref<>{pool};
    if (!payer_ref) {
        return false;
    }
    auto root_fee = pool->reserve(std::move(payer_ref), Graph::node_charge());
    auto creation = fixture.pending_target(0);
    auto child_pool_ref = object::ref<>{pool};
    if (!creation || !child_pool_ref) {
        return false;
    }
    auto child_reservation = pool->reserve(
        std::move(child_pool_ref), Graph::node_charge());
    auto target_ref = fixture.target_ref(0);
    if (!child_reservation || !target_ref) {
        return false;
    }
    auto allocation = creation.value().root(fixture.graph(), std::move(*root_fee), View{all_context_rights});
    if (!allocation) {
        return false;
    }
    // A pending slot accepts one initial lineage, never a second owner.
    {
        auto payer = object::ref<>{pool};
        if (!payer) return false;
        auto fee = pool->reserve(std::move(payer), Graph::node_charge());
        if (!fee) return false;
        auto duplicate = creation->root(fixture.graph(), std::move(*fee), View{all_context_rights});
        if (duplicate || duplicate.error() != GrantError::InvalidState) return false;
    }
    auto root_lease = allocation.value().acquire();
    auto child_target = fixture.target_ref(0);
    if (!root_lease || !child_target) {
        return false;
    }
    auto child = fixture.graph().derive(
        std::move(child_reservation).value(),
        root_lease.value(),
        std::move(child_target),
        View{inspect_rights});
    root_lease.value().reset();
    if (!child) {
        return false;
    }

    creation.value().reset();
    allocation->reset();
    if (deferred && (!fixture.graph().work_pending() || pool->available() == limit)) return false;
    fixture.drain();
    child.value().reset();
    creation.value().reset();
    allocation->reset();
    fixture.drop_retired_target(0);
    if (fixture.settled_grants() != 0
        || pool->available() != limit
        || pool->sponsorship_count() != 0
        || pool->close() != object::group::phase::closed
        || !pool.destroy()) {
        return false;
    }
    pool.reset();
    fixture.drain();
    return cap_test_pmm->verify_invariants();
}

bool test_pending_rollback_closes_lineage(const TestContext&) noexcept {
    return pending_rollback_closes_lineage(false);
}
bool test_pending_rollback_defers_refund(const TestContext&) noexcept {
    return pending_rollback_closes_lineage(true);
}

bool test_pool_close_revokes_hidden_allocation_root(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    constexpr resource::budget limit{
        .memory = 4 * mm::page_size,
        .caps = 4,
    };
    auto pending_pool = cap_test_groups->create(limit);
    if (!pending_pool) {
        return false;
    }
    auto pool = std::move(pending_pool).value().publish();
    auto payer_ref = object::ref<>{pool};
    auto child_pool_ref = object::ref<>{pool};
    if (!payer_ref || !child_pool_ref) {
        return false;
    }
    auto root_fee = pool->reserve(std::move(payer_ref), Graph::node_charge());
    auto creation = fixture.pending_target(0);
    auto child_reservation = pool->reserve(
        std::move(child_pool_ref), Graph::node_charge());
    auto target_ref = fixture.target_ref(0);
    if (!creation || !child_reservation || !target_ref) {
        return false;
    }
    auto allocation = creation.value().root(fixture.graph(), std::move(*root_fee), View{all_context_rights});
    if (!allocation) {
        return false;
    }
    auto root_lease = allocation.value().acquire();
    auto child_target = fixture.target_ref(0);
    if (!root_lease || !child_target) {
        return false;
    }
    auto child = fixture.graph().derive(
        std::move(child_reservation).value(),
        root_lease.value(),
        std::move(child_target),
        View{inspect_rights});
    root_lease.value().reset();
    if (!child) {
        return false;
    }
    auto installed = fixture.a().insert(
        std::move(child).value(), View{inspect_rights});
    if (!installed) {
        return false;
    }
    // Close observes a real pending member; publication must drive it again.
    if (pool->close() != object::group::phase::closing) return false;
    creation.value().publish().reset();
    allocation->reset();

    (void)pool->close();
    fixture.drain();
    if (pool->state() != object::group::phase::closed
        || pool->available() != limit
        || pool->sponsorship_count() != 0
        || fixture.settled_grants() != 0) {
        return false;
    }
    auto dead = fixture.a().resolve<sched::Sc>(
        installed.value(), inspect_rights);
    if (dead || !fixture.a().close(installed.value())) {
        return false;
    }
    fixture.drop_retired_target(0);
    if (!pool.destroy()) {
        return false;
    }
    pool.reset();
    fixture.drain();
    return cap_test_pmm->verify_invariants();
}

bool test_parent_close_recursively_closes_child_pool(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    constexpr resource::budget parent_limit{
        .memory = 16 * mm::page_size,
        .caps = 16,
    };
    constexpr resource::budget child_limit{
        .memory = 4 * mm::page_size,
        .caps = 4,
    };
    constexpr auto child_slot = object::pool<
        object::group>::slot_charge();
    constexpr resource::budget child_charge{
        .memory = child_slot.memory + child_limit.memory,
        .caps = child_slot.caps + child_limit.caps,
    };

    auto pending_parent = cap_test_groups->create(parent_limit);
    if (!pending_parent) {
        return false;
    }
    auto parent = std::move(pending_parent).value().publish();
    auto payer_ref = object::ref<>{parent};
    auto child_charge_ref = object::ref<>{parent};
    auto user_charge_ref = object::ref<>{parent};
    if (!payer_ref || !child_charge_ref
        || !user_charge_ref) {
        return false;
    }
    auto root_fee = parent->reserve(std::move(payer_ref), Graph::node_charge());
    auto object_reservation = parent->reserve(
        std::move(child_charge_ref), child_charge);
    auto user_reservation = parent->reserve(
        std::move(user_charge_ref), Graph::node_charge());
    if (!root_fee || !object_reservation
        || !user_reservation) {
        return false;
    }
    auto pending_child = cap_test_groups->create(std::move(object_reservation).value(), child_limit);
    if (!pending_child) {
        return false;
    }
    auto child = pending_child->reference();
    const auto rights = Rights::of(
        Right::Inspect, Right::Create, Right::Close, Right::Revoke);
    const cap::Quota authority{
        .budget = child_limit,
        .object_kinds = u64{1} << static_cast<u16>(
            ObjGroup),
    };
    auto allocation = pending_child->root(fixture.graph(), std::move(*root_fee), View{rights, authority});
    if (!allocation) {
        return false;
    }
    auto root_lease = allocation.value().acquire();
    auto user_target = object::ref<>{child};
    if (!root_lease || !user_target) {
        return false;
    }
    auto user_grant = fixture.graph().derive(
        std::move(user_reservation).value(),
        root_lease.value(),
        std::move(user_target),
        View{rights, authority});
    root_lease.value().reset();
    if (!user_grant) {
        return false;
    }
    auto installed = fixture.a().insert(
        std::move(user_grant).value(), View{rights, authority});
    if (!installed) {
        return false;
    }
    pending_child->publish().reset();
    allocation->reset();
    child.reset();

    (void)parent->close();
    if (fixture.a().resolve<object::group>(installed.value(), rights)) return false;
    fixture.drain();
    if (parent->state() != object::group::phase::closed
        || parent->available() != parent_limit
        || fixture.settled_grants() != 0
        || !fixture.a().close(installed.value())
        || !parent.destroy()) {
        return false;
    }
    parent.reset();
    fixture.drain();
    return cap_test_pmm->verify_invariants();
}

bool test_destroy_allocation_progress_and_pool_close(const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) return false;
    constexpr resource::budget limit{.memory = 8 * mm::page_size, .caps = 8};
    auto pending = cap_test_groups->create(limit);
    if (!pending) return false;
    auto pool = std::move(pending).value().publish();
    cap::Handle handles[2]{};
    const auto rights = Rights::of(Right::Inspect, Right::Destroy);
    for (usize index = 0; index < 2; ++index) {
        auto payer_ref = object::ref<>{pool};
        auto child_ref = object::ref<>{pool};
        if (!payer_ref || !child_ref) return false;
        auto root_fee = pool->reserve(std::move(payer_ref), Graph::node_charge());
        auto creation = fixture.pending_target(index);
        auto child_charge = pool->reserve(std::move(child_ref), Graph::node_charge());
        auto target = fixture.target_ref(index);
        if (!creation || !child_charge || !target) return false;
        auto allocation = creation.value().root(fixture.graph(), std::move(*root_fee), View{rights});
        if (!allocation) return false;
        auto lease = allocation.value().acquire();
        auto child_target = fixture.target_ref(index);
        if (!lease || !child_target) return false;
        auto child = fixture.graph().derive(std::move(child_charge).value(), lease.value(),
            std::move(child_target), View{rights});
        if (!child) return false;
        auto installed = fixture.a().insert(std::move(child).value(), View{rights});
        if (!installed) return false;
        handles[index] = installed.value();
        // Published caps can be used before the creator commits admission.
        if (index == 0 && !fixture.a().destroy(handles[index])) return false;
        creation.value().publish().reset();
        allocation->reset();
    }
    auto held = fixture.a().resolve<sched::Sc>(handles[1], inspect_rights);
    if (!held) return false;
    std::optional<cap::Resolved<sched::Sc>> retained{std::move(held).value()};
    if (!fixture.a().destroy(handles[1])) return false;
    // A retained operation delays its own revoke, not the other local close.
    if (fixture.settled_grants() != 2 || pool->state() != object::group::phase::open)
        return false;
    if (pool->close() != object::group::phase::closing) return false;
    retained.reset();
    fixture.drop_retired_target(0);
    fixture.drop_retired_target(1);
    if (pool->state() != object::group::phase::closed || pool->available() != limit
        || fixture.settled_grants() != 0 || !fixture.a().close(handles[0])
        || !fixture.a().close(handles[1]) || !pool.destroy()) return false;
    pool.reset();
    fixture.drain();
    return cap_test_pmm->verify_invariants();
}

bool test_destroy_authority_uses_object_anchor_retirement(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const Rights rights = Rights::of(Right::Inspect, Right::Destroy);
    auto root = fixture.root(0, rights);
    if (!root) {
        return false;
    }
    auto handle = fixture.a().insert(
        std::move(root).value(), View{rights});
    if (!handle || !fixture.a().destroy(handle.value())) {
        return false;
    }
    auto rejected = fixture.a().resolve<sched::Sc>(
        handle.value(), inspect_rights);
    if (rejected || !fixture.a().close(handle.value())) {
        return false;
    }
    fixture.drop_retired_target(0);
    return true;
}

bool test_last_reference_refunds_managed_object(const TestContext&) noexcept {
    CapFixture fixture;
    if (!fixture.initialize()) return false;
    constexpr resource::budget limit{.memory=8 * mm::page_size, .caps=16};
    auto made = cap_test_groups->create(limit);
    if (!made) return false;
    auto pool = made->publish();
    auto fee = pool->reserve(pool, object::pool<CSpace>::slot_charge());
    auto root_fee = pool->reserve(pool, Graph::node_charge());
    if (!fee || !root_fee) return false;
    auto object = cap_test_cspaces->create(std::move(*fee), *cap_test_pmm);
    if (!object) return false;
    auto root = object->root(fixture.graph(), std::move(*root_fee), View{inspect_rights});
    if (!root) return false;
    auto hold = object->publish();
    root->reset();
    fixture.drain();
    // A real internal owner keeps the object live even without a capability.
    if (!hold.live() || pool->available() == limit) return false;
    hold.reset();
    if (pool->available() == limit) return false; // Worker has not reclaimed storage yet.
    fixture.drain();
    if (pool->available() != limit || pool->sponsorship_count() ||
        pool->state() != object::group::phase::open ||
        pool->close() != object::group::phase::closed || !pool.destroy()) return false;
    pool.reset();
    fixture.drain();
    return cap_test_pmm->verify_invariants();
}

} // namespace

void register_cap_tests(TestRegistry& registry) noexcept {
    (void)registry.add("cap", "last owner asynchronously retires and refunds managed storage",
        test_last_reference_refunds_managed_object);

    (void)registry.add("cap", "batch publishes all reserved selectors or none", test_cap_batch_has_one_publication);
    (void)registry.add("cap", "attachment detach transfers callback quiescence exactly once",
        test_attachment_detach_transfers_quiescence_once);
    (void)registry.add("cap", "unpublished allocation rollback survives deferred revoke work",
        test_pending_rollback_defers_refund);
    (void)registry.add(
        "cap",
        "typed attenuation decodes every supported capability family",
        test_typed_attenuation_covers_all_families);
    (void)registry.add(
        "cap",
        "typed attenuation rejects malformed and amplifying descriptors",
        test_typed_attenuation_rejects_malformed_and_amplifying);
    (void)registry.add(
        "cap",
        "typed delegation commits atomically and rolls back on rejection",
        test_typed_delegate_transaction_rolls_back);
    (void)registry.add(
        "cap",
        "resolve composes authority and pins the typed target",
        test_resolve_composes_authority_and_pins_kind);
    (void)registry.add(
        "cap",
        "duplicate attenuates a shared grant without amplification",
        test_duplicate_attenuates_without_splitting_grant);
    (void)registry.add(
        "cap",
        "delegation revoke blocks new use and drains old leases",
        test_delegation_revoke_waits_for_existing_lease);
    (void)registry.add("cap", "lineage close drains the last operation after its last cap",
        test_close_drains_orphan_operation);
    (void)registry.add(
        "cap",
        "handles are CSpace-local and stale generations stay dead",
        test_handles_are_local_and_stale_generation_stays_dead);
    (void)registry.add(
        "cap",
        "remote selector close targets the exact CSpace",
        test_remote_selector_close_is_cspace_exact);
    (void)registry.add(
        "cap",
        "move preserves source when destination transaction fails",
        test_move_is_transactional_across_cspaces);
    (void)registry.add(
        "cap",
        "IPC transfer publishes copy and move as one destination batch",
        test_ipc_transfer_commits_copy_and_move_atomically);
    (void)registry.add(
        "cap",
        "IPC transfer rolls back every reservation after source mutation",
        test_cap_batch_rolls_back);
    (void)registry.add(
        "cap",
        "CSpace retirement waits for reserved operations and pinned teardown",
        test_cspace_retire_waits_for_reserved_operation);
    (void)registry.add(
        "cap",
        "sponsored CSpace refunds selector and table capacity at reuse",
        test_sponsored_cspace_refunds_reusable_capacity);
    (void)registry.add(
        "cap",
        "attenuated ABI operations and revoke use effective slot authority",
        test_attenuated_operations_and_revoke_use_slot_authority);
    (void)registry.add(
        "cap",
        "revoked tombstones release their hidden target hold",
        test_revoked_tombstones_do_not_retain_target);
    (void)registry.add(
        "cap",
        "pending rollback revokes its complete hidden lineage",
        test_pending_rollback_closes_lineage);
    (void)registry.add(
        "cap",
        "group close revokes hidden roots without scanning CSpaces",
        test_pool_close_revokes_hidden_allocation_root);
    (void)registry.add(
        "cap",
        "parent group close recursively drains its child pool",
        test_parent_close_recursively_closes_child_pool);
    (void)registry.add(
        "cap",
        "destroy authority enters the target anchor retirement path",
        test_destroy_authority_uses_object_anchor_retirement);
    (void)registry.add("cap", "local object destruction drains independently and joins pool close",
        test_destroy_allocation_progress_and_pool_close);
}
