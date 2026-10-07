#include <expected>
#include <array>
#include <optional>
#include <variant>
#include <test/test.hpp>

#include <cap/cap.hpp>
#include <cap/cspace.hpp>
#include <cap/graph.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <utility>
#include <ipc/transfer.hpp>
#include <mm/pmm.hpp>
#include <object/pool.hpp>
#include <object/group.hpp>
#include <boot/link.hpp>
#include <sched/sc.hpp>
#include <task/thread.hpp>
#include <uapi/mem.h>

namespace {

using cap::View;
using cap::CSpace;
using cap::Attenuation;
using cap::AttenuationError;
using cap::GrantError;
using cap::GrantGraph;
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
constinit libk::ManualLifetime<mm::RegionList> cap_test_map{};
constinit libk::ManualLifetime<mm::Pmm> cap_test_pmm{};
constinit libk::delegate<void() noexcept> cap_test_notify{};
constinit libk::ManualLifetime<object::pool<sched::Sc>> cap_test_contexts{};
constinit libk::ManualLifetime<object::pool<object::group>> cap_test_groups{};
constinit libk::ManualLifetime<object::pool<cap::CSpace>> cap_test_cspaces{};

constinit libk::ManualLifetime<GrantGraph> cap_test_graph{};
constinit libk::ManualLifetime<CSpace> cap_test_space_a{};
constinit libk::ManualLifetime<CSpace> cap_test_space_b{};
constinit libk::ManualLifetime<CSpace> cap_test_space_one{};
constinit libk::ManualLifetime<ipc::Transfer> cap_test_transfer{};

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
        const auto physical = kernel_phys(mm::Virt{
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
                .va = mm::Virt{
                    reinterpret_cast<usize>(cap_test_ram)},
                .size = sizeof(cap_test_ram),
            })) {
            reset();
            return false;
        }
        cap_test_map.reset();
        (void)cap_test_contexts.emplace(*cap_test_pmm, cap_test_notify);
        (void)cap_test_groups.emplace(*cap_test_pmm, cap_test_notify);
        (void)cap_test_cspaces.emplace(*cap_test_pmm, cap_test_notify);
        [[maybe_unused]] auto& graph =
            cap_test_graph.emplace(*cap_test_pmm);
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
                    .budget = time::Duration::from_ticks(index + 1),
                    .period = time::Duration::from_ticks(10),
                },
                time::Instant::from_ticks(0));
            if (!pending) {
                reset();
                return false;
            }
            targets_[index] = std::move(pending).value().publish();
        }
        return true;
    }

    [[nodiscard]] auto root(
        usize target,
        Rights rights = all_context_rights) noexcept
        -> std::expected<GrantRef, GrantError> {
        if (target >= 2 || !targets_[target]) {
            return std::unexpected(GrantError::InvalidKey);
        }
        auto reference = targets_[target].erase();
        if (!reference) {
            return std::unexpected(GrantError::InvalidState);
        }
        return graph().create_root(
            std::move(reference).value(),
            View{rights});
    }

    [[nodiscard]] auto graph() noexcept -> GrantGraph& {
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
    [[nodiscard]] auto target_ref(usize index) noexcept
        -> std::expected<object::ref<>,
            object::error> {
        if (index >= 2 || !targets_[index]) {
            return std::unexpected(
                object::error::invalid_id);
        }
        return targets_[index].erase();
    }
    void drain() noexcept {
        cap_test_cspaces->drain_reclaim();
        cap_test_contexts->drain_reclaim();
        cap_test_groups->drain_reclaim();
    }
    void drop_retired_target(usize index) noexcept {
        libk_assert(index < 2 && targets_[index]);
        targets_[index].reset();
        drain();
    }
    [[nodiscard]] auto retire_target(usize index) noexcept -> bool {
        libk_assert(index < 2 && targets_[index]);
        const auto id = targets_[index].id();
        if (!targets_[index].retire()) {
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
        cap_test_graph.reset();
        for (auto& target : targets_) {
            if (target) {
                libk_assert(target.retire());
                target.reset();
            }
        }
        if (cap_test_contexts) {
            drain();
        }
        cap_test_cspaces.reset();
        cap_test_contexts.reset();
        cap_test_groups.reset();
        cap_test_pmm.reset();
        cap_test_map.reset();
    }

    object::ref<sched::Sc> targets_[2]{};
};

[[nodiscard]] auto decode_and_check(
    object::ObjectKind kind,
    const View& source,
    byte (&bytes)[CAP_ATTENUATION_SIZE]) noexcept -> bool {
    auto decoded = cap::decode_attenuation(
        libk::Span<const byte>{bytes, sizeof(bytes)});
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
    using cap::EpLimit;
    using cap::IrqRoute;
    using cap::MemLimit;
    using cap::Badge;
    using cap::Quota;
    using cap::VmLimit;
    using object::ObjectKind;

    byte bytes[CAP_ATTENUATION_SIZE]{};
    const View simple{attenuation_rights, std::monostate{}};
    const ObjectKind simple_kinds[] = {
        ObjectKind::Thread,
        ObjectKind::Sc,
        ObjectKind::Domain,
        ObjectKind::CSpace};
    for (const ObjectKind kind : simple_kinds) {
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
    attenuation_bytes(bytes, OBJECT_KIND_MEMORY);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET, 1);
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 2);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET,
        static_cast<u64>(VM_READ));
    if (!decode_and_check(ObjectKind::Mem, memory, bytes)) {
        return false;
    }

    const View vspace{
        attenuation_rights,
        VmLimit{
            mm::VRange{mm::Virt{0x1000}, 0x10000},
            mm::Perms::of(mm::Perm::Read)}};
    attenuation_bytes(bytes, OBJECT_KIND_VSPACE);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET, 0x2000);
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 0x2000);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET, VM_READ);
    if (!decode_and_check(ObjectKind::VSpace, vspace, bytes)) {
        return false;
    }

    const View pool{
        attenuation_rights,
        Quota{
            resource::budget{1024 * 1024, 16},
            OBJECT_KINDS}};
    attenuation_bytes(bytes, OBJECT_KIND_RESOURCE_POOL);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET, 4096);
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 4);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET,
        (u64{1} << OBJECT_KIND_MEMORY));
    if (!decode_and_check(ObjectKind::group, pool, bytes)) {
        return false;
    }

    const View notification{
        attenuation_rights, Badge{7}};
    attenuation_bytes(bytes, OBJECT_KIND_NOTIFICATION);
    if (!decode_and_check(ObjectKind::Notification, notification, bytes)) {
        return false;
    }

    const View endpoint{
        attenuation_rights, EpLimit{3, 3, 8}};
    attenuation_bytes(bytes, OBJECT_KIND_ENDPOINT);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET, 3);
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 3);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET, 4);
    if (!decode_and_check(ObjectKind::Endpoint, endpoint, bytes)) {
        return false;
    }

    const View channel{
        attenuation_rights, ChanLimit{ChannelSide::Any, 0, 0}};
    attenuation_bytes(bytes, OBJECT_KIND_CHANNEL);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET,
        CAP_CHANNEL_SIDE_A);
    if (!decode_and_check(ObjectKind::Channel, channel, bytes)) {
        return false;
    }

    const View pager{
        attenuation_rights};
    attenuation_bytes(bytes, OBJECT_KIND_PAGER);
    if (!decode_and_check(ObjectKind::Pager, pager, bytes)) {
        return false;
    }

    const View irq{
        attenuation_rights, IrqRoute{4, true}};
    attenuation_bytes(bytes, OBJECT_KIND_IRQ);
    return decode_and_check(ObjectKind::Irq, irq, bytes);
}

bool test_typed_attenuation_rejects_malformed_and_amplifying(
    const TestContext&) noexcept {
    using cap::ChanLimit;
    using cap::ChannelSide;
    using cap::MemLimit;
    using object::ObjectKind;

    byte bytes[CAP_ATTENUATION_SIZE]{};
    attenuation_bytes(bytes, OBJECT_KIND_THREAD);
    bytes[CAP_ATTENUATION_VERSION_OFFSET] = 2;
    if (cap::decode_attenuation(
            libk::Span<const byte>{bytes, sizeof(bytes)})) {
        return false;
    }
    attenuation_bytes(bytes, 10);
    if (cap::decode_attenuation(
            libk::Span<const byte>{bytes, sizeof(bytes)})) {
        return false;
    }
    attenuation_bytes(bytes, OBJECT_KIND_THREAD);
    put64(bytes, CAP_ATTENUATION_WORD5_OFFSET, 1);
    auto decoded = cap::decode_attenuation(
        libk::Span<const byte>{bytes, sizeof(bytes)});
    if (!decoded) {
        return false;
    }
    const View simple{attenuation_rights, std::monostate{}};
    if (cap::make_attenuation_ceiling(
            ObjectKind::Thread, simple, decoded.value())) {
        return false;
    }
    attenuation_bytes(bytes, OBJECT_KIND_MEMORY);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET, ~u64{});
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 2);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET, VM_READ);
    decoded = cap::decode_attenuation(
        libk::Span<const byte>{bytes, sizeof(bytes)});
    const View memory{
        attenuation_rights,
        MemLimit{
            mm::ObjectRange{0, 16},
            mm::Perms::of(mm::Perm::Read)}};
    if (!decoded || cap::make_attenuation_ceiling(
                         ObjectKind::Mem, memory, decoded.value())) {
        return false;
    }
    attenuation_bytes(bytes, OBJECT_KIND_CHANNEL);
    put64(bytes, CAP_ATTENUATION_WORD0_OFFSET,
        CAP_CHANNEL_SIDE_A);
    put64(bytes, CAP_ATTENUATION_WORD1_OFFSET, 1);
    put64(bytes, CAP_ATTENUATION_WORD2_OFFSET, ~u64{});
    decoded = cap::decode_attenuation(
        libk::Span<const byte>{bytes, sizeof(bytes)});
    const View channel{
        attenuation_rights, ChanLimit{ChannelSide::Any, 0, 0}};
    if (!decoded) {
        return false;
    }
    auto channel_ceiling = cap::make_attenuation_ceiling(
        ObjectKind::Channel, channel, decoded.value());
    return channel_ceiling
        && !cap::attenuates(
            ObjectKind::Channel, channel, channel_ceiling.value());
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
    attenuation_bytes(bytes, OBJECT_KIND_SCHED_CONTEXT);
    auto decoded = cap::decode_attenuation(
        libk::Span<const byte>{bytes, sizeof(bytes)});
    if (!decoded) {
        return false;
    }
    const usize before_slots = fixture.b().live_slots();
    const usize before_grants = fixture.graph().live_count();
    auto child = fixture.a().typed_delegate(
        source.value(), fixture.b(), decoded.value());
    if (!child || fixture.b().live_slots() != before_slots + 1
        || fixture.graph().live_count() != before_grants + 1) {
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
    const usize stable_grants = fixture.graph().live_count();

    // Fill the bounded destination so the production transaction derives its
    // child Grant before destination reservation fails.  GrantGraph and CSpace
    // must return to the exact pre-call counts, not merely reject early.
    auto occupied = fixture.a().duplicate(
        source.value(), fixture.one(), basic_rights);
    if (!occupied || fixture.one().live_slots() != 1) {
        return false;
    }
    const usize full_slots = fixture.one().live_slots();
    const usize full_grants = fixture.graph().live_count();
    attenuation_bytes(bytes, OBJECT_KIND_SCHED_CONTEXT);
    decoded = cap::decode_attenuation(
        libk::Span<const byte>{bytes, sizeof(bytes)});
    if (!decoded
        || fixture.a().typed_delegate(
               source.value(), fixture.one(), decoded.value())
        || fixture.one().live_slots() != full_slots
        || fixture.graph().live_count() != full_grants
        || !fixture.one().close(occupied.value())) {
        return false;
    }

    attenuation_bytes(bytes, OBJECT_KIND_THREAD);
    decoded = cap::decode_attenuation(
        libk::Span<const byte>{bytes, sizeof(bytes)});
    if (!decoded
        || fixture.a().typed_delegate(
               source.value(), fixture.b(), decoded.value())
        || fixture.b().live_slots() != stable_slots
        || fixture.graph().live_count() != stable_grants) {
        return false;
    }
    attenuation_bytes(bytes, OBJECT_KIND_SCHED_CONTEXT,
        static_cast<u64>(RIGHT_CONTROL));
    decoded = cap::decode_attenuation(
        libk::Span<const byte>{bytes, sizeof(bytes)});
    if (!decoded
        || fixture.a().typed_delegate(
               source.value(), fixture.b(), decoded.value())
        || fixture.b().live_slots() != stable_slots
        || fixture.graph().live_count() != stable_grants) {
        return false;
    }
    return fixture.a().close(source.value())
        && !fixture.a().typed_delegate(
            source.value(), fixture.b(), decoded.value());
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
            resolved.value().lease(), std::move(other).value(), View{inspect_rights});
        if (replaced || replaced.error() != cap::GrantError::InvalidKey
            || fixture.graph().live_count() != 1) return false;
    }
    return fixture.a().close(handle)
        && fixture.graph().live_count() == 0;
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
    auto amplified = fixture.a().duplicate(
        source.value(), fixture.b(), View{Rights::of(Right::Control)});
    if (amplified
        || amplified.error() != cap::CSpaceError::Amplification) {
        return false;
    }
    auto copy = fixture.a().duplicate(
        source.value(), fixture.b(), View{inspect_rights});
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
        && fixture.graph().live_count() == 0;
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
    auto child = fixture.a().delegate(
        source.value(),
        fixture.b(),
        View{inspect_rights},
        View{inspect_rights});
    if (!child) {
        return false;
    }

    RevokeProbe probe{};
    cap::GrantRevoke completion{
        sync::Latch::Notifier::bind<
            &RevokeProbe::ready>(probe)};
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
        if (rejected
            || rejected.error()
                != cap::CSpaceError::GrantUnavailable
            || !source_live) {
            return false;
        }
    }
    const auto child_state = fixture.graph().state(child_key);
    if (!completion.complete() || probe.signals != 1
        || child_state
        || child_state.error() != GrantError::InvalidKey) {
        return false;
    }
    return fixture.b().close(child.value())
        && fixture.a().close(source.value())
        && fixture.graph().live_count() == 0;
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
        .invalidate = [](void* context, cap::GrantWork&& work, cap::GrantInvalidation) noexcept {
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
        Probe probe{ops};
        auto lease = root.value().acquire();
        if (!lease || !lease.value().attach(probe.attachment)) return false;
        lease.value().reset();
        if (order != 0) {
            if (!fixture.graph().invalidate(root.value().key(), revoke)
                || !probe.work || !probe.attachment.busy()) return false;
            if (order == 2) probe.work.reset();
        }
        const bool synchronous = probe.attachment.detach();
        if (synchronous != (order != 1) || probe.releases != 0) return false;
        probe.work.reset();
        if (probe.releases != (order == 1 ? 1U : 0U)
            || probe.attachment.attached() || probe.attachment.busy()
            || (order != 0 && !revoke.complete())) return false;
    }
    return fixture.graph().live_count() == 0;
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
    return f.graph().live_count() == 0;
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
        && fixture.graph().live_count() == 0;
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
        && fixture.graph().live_count() == 0;
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
    auto rejected = fixture.a().move(source.value(), fixture.one());
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
    auto moved = fixture.a().move(source.value(), fixture.one());
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
        && fixture.graph().live_count() == 0;
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

    ipc::Transfer::Specs specs{};
    if (!specs.try_emplace_back(ipc::TransferSpec{
            .source = copy_source.value(),
            .rights = inspect_rights,
            .kind = ipc::TransferKind::Copy,
        })
        || !specs.try_emplace_back(ipc::TransferSpec{
            .source = move_source.value(),
            .kind = ipc::TransferKind::Move,
        })) {
        return false;
    }
    auto& transfer = cap_test_transfer.emplace();
    if (!ipc::Transfer::prepare(
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
        && fixture.graph().live_count() == 0;
}

bool test_ipc_transfer_rolls_back_when_move_source_changes(
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
        std::move(root).value(), View{inspect_rights});
    if (!source) {
        return false;
    }
    ipc::Transfer::Specs specs{};
    if (!specs.try_emplace_back(ipc::TransferSpec{
            .source = source.value(),
            .kind = ipc::TransferKind::Move,
        })) {
        return false;
    }

    auto& transfer = cap_test_transfer.emplace();
    if (!ipc::Transfer::prepare(
            transfer, fixture.a(), fixture.b(), specs)
        || !fixture.a().close(source.value())) {
        return false;
    }
    auto rejected = transfer.commit();
    if (rejected
        || rejected.error() != ipc::TransferError::SourceChanged
        || fixture.b().live_slots() != 1) {
        return false;
    }
    cap_test_transfer.reset();
    return fixture.b().live_slots() == 0
        && fixture.graph().live_count() == 0;
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
        if (!held.retire()) {
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
    auto made_pool = cap_test_groups->create(*cap_test_pmm, limit);
    if (!made_pool) {
        return false;
    }
    auto pool = std::move(made_pool).value().publish();
    auto pool_ref = pool.erase();
    if (!pool_ref) {
        return false;
    }
    auto charged = pool->reserve(
        std::move(pool_ref).value(), object_charge);
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
        || !space->delegate(
            installed.value(), fixture.b(), inspect_rights)) {
        return false;
    }
    const resource::budget after_delegate{
        .memory = after_install.memory - GrantGraph::node_charge().memory,
        .caps = after_install.caps,
    };
    if (pool->available() != after_delegate) {
        return false;
    }
    auto child = space->delegate(
        installed.value(), fixture.b(), inspect_rights);
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
            || !space.retire() || space->table_pages() != 3
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
        || !pool.retire()) {
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
    auto duplicate = fixture.a().duplicate(
        source.value(), fixture.b(), inspect_rights);
    auto child = fixture.a().delegate(
        source.value(), fixture.b(), inspect_rights);
    if (!duplicate || !child) {
        return false;
    }
    cap::GrantRevoke completion{};
    if (!fixture.a().revoke(source.value(), completion, false)
        || !completion.complete()) {
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
    auto child = fixture.a().delegate(
        source.value(), fixture.b(), inspect_rights);
    if (!child) {
        return false;
    }

    cap::GrantRevoke completion{};
    if (!fixture.a().revoke(source.value(), completion, true)
        || !completion.complete()
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
        && fixture.graph().live_count() == 0;
}

bool allocation_transaction_aborts_complete_lineage(bool deferred) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    constexpr resource::budget limit{
        .memory = 4 * mm::page_size,
        .caps = 4,
    };
    auto pending_pool = cap_test_groups->create(*cap_test_pmm, limit);
    if (!pending_pool) {
        return false;
    }
    auto pool = std::move(pending_pool).value().publish();
    if (!deferred) {
        auto self = pool.erase();
        auto target = fixture.target_ref(1);
        if (!self || !target) return false;
        auto rejected = pool->begin(std::move(*self));
        if (!rejected || rejected->adopt(fixture.graph(), std::move(*target),
                                        View{Rights::of(Right::Signal)})) return false;
        rejected->reset(); // Private target rollback before any grant exists.
        fixture.drop_retired_target(1);
        if (pool->available() != limit || fixture.graph().live_count()) return false;
    }

    auto txn_ref = pool.erase();
    if (!txn_ref) {
        return false;
    }
    auto txn = pool->begin(std::move(txn_ref).value());
    auto child_pool_ref = pool.erase();
    if (!txn || !child_pool_ref) {
        return false;
    }
    auto child_reservation = pool->reserve(
        std::move(child_pool_ref).value(), GrantGraph::node_charge());
    auto target_ref = fixture.target_ref(0);
    if (!child_reservation || !target_ref) {
        return false;
    }
    auto allocation = txn.value().adopt(fixture.graph(), std::move(target_ref).value(), View{all_context_rights});
    if (!allocation) {
        return false;
    }
    auto root_lease = txn.value().acquire();
    auto child_target = fixture.target_ref(0);
    if (!root_lease || !child_target) {
        return false;
    }
    auto child = fixture.graph().derive(
        std::move(child_reservation).value(),
        root_lease.value(),
        std::move(child_target).value(),
        View{inspect_rights});
    root_lease.value().reset();
    if (!child) {
        return false;
    }

    struct Work final {
        usize signals{};
        void wake() noexcept { ++signals; }
    } work;
    if (deferred) fixture.graph().bind_work_notifier(GrantGraph::WorkNotifier::bind<&Work::wake>(work));
    txn.value().reset();
    if (deferred) {
        const bool retained = work.signals != 0 && fixture.graph().work_pending()
            && pool->available() != limit;
        while (fixture.graph().service(1).more) {}
        fixture.graph().unbind_work_notifier();
        if (!retained) return false;
    }
    child.value().reset();
    txn.value().reset();
    fixture.drop_retired_target(0);
    if (fixture.graph().live_count() != 0
        || pool->available() != limit
        || pool->sponsorship_count() != 0
        || pool->close() != object::group::phase::closed
        || !pool.retire()) {
        return false;
    }
    pool.reset();
    fixture.drain();
    return cap_test_pmm->verify_invariants();
}

bool test_allocation_transaction_aborts_complete_lineage(const TestContext&) noexcept {
    return allocation_transaction_aborts_complete_lineage(false);
}
bool test_allocation_transaction_defers_rollback(const TestContext&) noexcept {
    return allocation_transaction_aborts_complete_lineage(true);
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
    auto pending_pool = cap_test_groups->create(*cap_test_pmm, limit);
    if (!pending_pool) {
        return false;
    }
    auto pool = std::move(pending_pool).value().publish();
    auto txn_ref = pool.erase();
    auto child_pool_ref = pool.erase();
    if (!txn_ref || !child_pool_ref) {
        return false;
    }
    auto txn = pool->begin(std::move(txn_ref).value());
    auto child_reservation = pool->reserve(
        std::move(child_pool_ref).value(), GrantGraph::node_charge());
    auto target_ref = fixture.target_ref(0);
    if (!txn || !child_reservation || !target_ref) {
        return false;
    }
    auto allocation = txn.value().adopt(fixture.graph(), std::move(target_ref).value(), View{all_context_rights});
    if (!allocation) {
        return false;
    }
    auto root_lease = txn.value().acquire();
    auto child_target = fixture.target_ref(0);
    if (!root_lease || !child_target) {
        return false;
    }
    auto child = fixture.graph().derive(
        std::move(child_reservation).value(),
        root_lease.value(),
        std::move(child_target).value(),
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
    txn.value().commit();
    txn.value().reset();

    if (pool->close() != object::group::phase::closed
        || pool->available() != limit
        || pool->sponsorship_count() != 0
        || fixture.graph().live_count() != 0) {
        return false;
    }
    auto dead = fixture.a().resolve<sched::Sc>(
        installed.value(), inspect_rights);
    if (dead || !fixture.a().close(installed.value())) {
        return false;
    }
    fixture.drop_retired_target(0);
    if (!pool.retire()) {
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

    auto pending_parent = cap_test_groups->create(*cap_test_pmm, parent_limit);
    if (!pending_parent) {
        return false;
    }
    auto parent = std::move(pending_parent).value().publish();
    auto txn_ref = parent.erase();
    auto child_charge_ref = parent.erase();
    auto user_charge_ref = parent.erase();
    if (!txn_ref || !child_charge_ref
        || !user_charge_ref) {
        return false;
    }
    auto txn = parent->begin(std::move(txn_ref).value());
    auto object_reservation = parent->reserve(
        std::move(child_charge_ref).value(), child_charge);
    auto user_reservation = parent->reserve(
        std::move(user_charge_ref).value(), GrantGraph::node_charge());
    if (!txn || !object_reservation
        || !user_reservation) {
        return false;
    }
    auto pending_child = cap_test_groups->create(std::move(object_reservation).value(), *cap_test_pmm, child_limit);
    if (!pending_child) {
        return false;
    }
    auto child = std::move(pending_child).value().publish();
    auto child_ref = child.erase();
    if (!child_ref) {
        return false;
    }
    const auto rights = Rights::of(
        Right::Inspect, Right::Create, Right::Close, Right::Revoke);
    const cap::Quota authority{
        .budget = child_limit,
        .object_kinds = u64{1} << static_cast<u16>(
            object::ObjectKind::group),
    };
    auto allocation = txn.value().adopt(fixture.graph(), std::move(child_ref).value(), View{rights, authority});
    if (!allocation) {
        return false;
    }
    auto root_lease = txn.value().acquire();
    auto user_target = child.erase();
    if (!root_lease || !user_target) {
        return false;
    }
    auto user_grant = fixture.graph().derive(
        std::move(user_reservation).value(),
        root_lease.value(),
        std::move(user_target).value(),
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
    txn.value().commit();
    txn.value().reset();
    child.reset();

    if (parent->close() != object::group::phase::reclaiming) {
        return false;
    }
    fixture.drain();
    if (parent->state() != object::group::phase::closed
        || parent->available() != parent_limit
        || fixture.graph().live_count() != 0
        || !fixture.a().close(installed.value())
        || !parent.retire()) {
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
    auto pending = cap_test_groups->create(*cap_test_pmm, limit);
    if (!pending) return false;
    auto pool = std::move(pending).value().publish();
    cap::Handle handles[2]{};
    const auto rights = Rights::of(Right::Inspect, Right::Destroy);
    for (usize index = 0; index < 2; ++index) {
        auto txn_ref = pool.erase();
        auto child_ref = pool.erase();
        if (!txn_ref || !child_ref) return false;
        auto txn = pool->begin(std::move(txn_ref).value());
        auto child_charge = pool->reserve(std::move(child_ref).value(), GrantGraph::node_charge());
        auto target = fixture.target_ref(index);
        if (!txn || !child_charge || !target) return false;
        auto allocation = txn.value().adopt(fixture.graph(), std::move(target).value(), View{rights});
        if (!allocation) return false;
        auto lease = txn.value().acquire();
        auto child_target = fixture.target_ref(index);
        if (!lease || !child_target) return false;
        auto child = fixture.graph().derive(std::move(child_charge).value(), lease.value(),
            std::move(child_target).value(), View{rights});
        if (!child) return false;
        auto installed = fixture.a().insert(std::move(child).value(), View{rights});
        if (!installed) return false;
        handles[index] = installed.value();
        txn.value().commit();
    }
    auto held = fixture.a().resolve<sched::Sc>(handles[1], inspect_rights);
    if (!held) return false;
    std::optional<cap::Resolved<sched::Sc>> retained{std::move(held).value()};
    if (!fixture.a().destroy(handles[1]) || !fixture.a().destroy(handles[0])) return false;
    // A retained operation delays its own revoke, not the other local close.
    if (fixture.graph().live_count() != 2 || pool->state() != object::group::phase::open)
        return false;
    if (pool->close() != object::group::phase::revoking) return false;
    retained.reset();
    fixture.drop_retired_target(0);
    fixture.drop_retired_target(1);
    if (pool->state() != object::group::phase::closed || pool->available() != limit
        || fixture.graph().live_count() != 0 || !fixture.a().close(handles[0])
        || !fixture.a().close(handles[1]) || !pool.retire()) return false;
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

} // namespace

void register_cap_tests(TestRegistry& registry) noexcept {
    (void)registry.add("cap", "batch publishes all reserved selectors or none", test_cap_batch_has_one_publication);
    (void)registry.add("cap", "attachment detach transfers callback quiescence exactly once",
        test_attachment_detach_transfers_quiescence_once);
    (void)registry.add("cap", "unpublished allocation rollback survives deferred revoke work",
        test_allocation_transaction_defers_rollback);
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
        test_ipc_transfer_rolls_back_when_move_source_changes);
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
        "allocation transaction abort revokes its complete hidden lineage",
        test_allocation_transaction_aborts_complete_lineage);
    (void)registry.add(
        "cap",
        "ResourcePool close revokes hidden roots without scanning CSpaces",
        test_pool_close_revokes_hidden_allocation_root);
    (void)registry.add(
        "cap",
        "parent ResourcePool close recursively drains its child pool",
        test_parent_close_recursively_closes_child_pool);
    (void)registry.add(
        "cap",
        "destroy authority enters the target anchor retirement path",
        test_destroy_authority_uses_object_anchor_retirement);
    (void)registry.add("cap", "local object destruction drains independently and joins pool close",
        test_destroy_allocation_progress_and_pool_close);
}
