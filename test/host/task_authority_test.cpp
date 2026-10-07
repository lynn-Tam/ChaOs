#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <libk/assert.hpp>
#include <utility>
#include <uapi/status.h>
#include <servers/deploy/detail/authority.hpp>

#include "deploy/golden.hpp"

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    __builtin_trap();
}
} // namespace libk

namespace {

struct FakeBackend final {
    static inline size_t close_calls{};
    static inline size_t duplicate_calls{};
    static inline size_t typed_calls{};
    static inline size_t channel_calls{};
    static inline size_t fail_after{};
    static inline size_t fail_nonzero_after{};
    static inline size_t produced{90};
    static inline size_t resource_close_calls{};
    static inline cap_t closed_selectors[32]{};
    static inline size_t closed_count{};
    static inline cap_t close_failure_selector{};
    static inline bool ownership_faulted{};

    static void reset() noexcept {
        close_calls = 0;
        duplicate_calls = 0;
        typed_calls = 0;
        channel_calls = 0;
        fail_after = 0;
        fail_nonzero_after = 0;
        produced = 90;
        resource_close_calls = 0;
        closed_selectors[0] = 0;
        closed_count = 0;
        close_failure_selector = 0;
        ownership_faulted = false;
    }

    static void ownership_fault(status_t) noexcept {
        ownership_faulted = true;
    }

    [[nodiscard]] static auto close(
        sys::cap::CapRef reference) noexcept -> status_t {
        ++close_calls;
        if (closed_count < sizeof(closed_selectors) / sizeof(closed_selectors[0])) {
            closed_selectors[closed_count++] = reference.selector;
        }
        if (reference.selector == close_failure_selector) {
            return STATUS_BUSY;
        }
        return STATUS_OK;
    }

    [[nodiscard]] static auto resource_create_child(
        sys::cap::CapRef,
        word_t,
        word_t,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, 10, 0};
    }

    [[nodiscard]] static auto resource_close(
        sys::cap::CapRef) noexcept -> status_t {
        ++resource_close_calls;
        return STATUS_OK;
    }

    [[nodiscard]] static auto vspace_create(
        sys::cap::CapRef) noexcept -> sys::SysResult {
        return {STATUS_OK, 11, 0};
    }

    [[nodiscard]] static auto cspace_create(
        sys::cap::CapRef,
        word_t,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, 12, 0};
    }

    [[nodiscard]] static auto vm_slice(
        sys::cap::CapRef,
        word_t,
        word_t,
        word_t,
        word_t) noexcept -> sys::SysResult {
        return {STATUS_OK, 13, 0};
    }

    [[nodiscard]] static auto vm_map(
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t,
        word_t,
        word_t,
        word_t) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto vm_unmap(
        sys::cap::CapRef,
        word_t,
        word_t) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto vm_clear(
        sys::cap::CapRef) noexcept -> status_t {
        return STATUS_OK;
    }

    [[nodiscard]] static auto duplicate(
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t) noexcept -> sys::SysResult {
        ++duplicate_calls;
        if (fail_after != 0 && duplicate_calls >= fail_after) {
            return {STATUS_BUSY, 0, 0};
        }
        if (fail_nonzero_after != 0
            && duplicate_calls >= fail_nonzero_after) {
            return {STATUS_BUSY, ++produced, 0};
        }
        return {STATUS_OK, ++produced, 0};
    }

    [[nodiscard]] static auto typed_delegate(
        sys::cap::CapRef,
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t) noexcept -> sys::SysResult {
        ++typed_calls;
        return {STATUS_OK, ++produced, 0};
    }

    [[nodiscard]] static auto channel_mint(
        sys::cap::CapRef,
        sys::cap::CapRef,
        word_t,
        word_t) noexcept -> sys::SysResult {
        ++channel_calls;
        return {STATUS_OK, ++produced, 0};
    }
};

using Space = deploy::TaskSpace<8, 8, FakeBackend>;
using Authorities = deploy::AuthoritySet<4, 4>;

template<size_t RegistrationCapacity>
struct SourceFixture final {
    using aggregate_type = deploy::RegisteredSpace<
        Space, RegistrationCapacity>;

    Space raw{};
    aggregate_type aggregate{};

    [[nodiscard]] auto open() noexcept -> bool {
        return raw.open(
                   sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
            == STATUS_OK;
    }

    [[nodiscard]] auto add(
        cap_t selector,
        obj_kind_t kind) noexcept
        -> std::optional<deploy::LocalSlot> {
        return raw.adopt_local(
            typename Space::owner_type{
                sys::cap::CapRef{selector, 0}},
            kind);
    }

    [[nodiscard]] auto adopt() noexcept -> bool {
        return aggregate.adopt(std::move(raw));
    }

    template<typename Set>
    [[nodiscard]] auto register_source(
        Set& authorities,
        deploy::LocalSlot slot,
        uint64_t identity,
        const CapView& ceiling) noexcept
        -> std::optional<deploy::AuthorityId> {
        return aggregate.register_source(
            authorities, slot, identity, ceiling);
    }

    [[nodiscard]] auto close() noexcept -> status_t {
        return aggregate.close();
    }
};

struct Fixture final {
    uint8_t raw[deploy::host::kGoldenSize]{};
    deploy::ManifestWorkspace workspace{};
    deploy::PlanSet<1> plans{};
    deploy::DeploymentPlan plan{};
};

struct BatchFixture final {
    uint8_t raw[deploy::host::kGoldenSize
                + DEPLOY_IMPORT_STRIDE]{};
    deploy::ManifestWorkspace workspace{};
    deploy::PlanSet<1> plans{};
    deploy::DeploymentPlan plan{};
};

void put(
    uint8_t* bytes,
    size_t offset,
    uint64_t value,
    size_t width) noexcept {
    for (size_t byte = 0; byte < width; ++byte) {
        bytes[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
    }
}

[[nodiscard]] auto get(
    const uint8_t* bytes,
    size_t offset,
    size_t width) noexcept -> uint64_t {
    uint64_t result{};
    for (size_t byte = 0; byte < width; ++byte) {
        result |= static_cast<uint64_t>(bytes[offset + byte])
            << (byte * 8);
    }
    return result;
}

[[nodiscard]] auto make_plan(
    Fixture& fixture,
    uint16_t mode = DEPLOY_IMPORT_DUPLICATE,
    uint16_t attenuation_kind = OBJECT_KIND_THREAD,
    uint64_t channel_badge = 0) noexcept -> bool {
    for (size_t index = 0; index < sizeof(fixture.raw); ++index) {
        fixture.raw[index] = deploy::host::kGolden[index];
    }
    const size_t descriptor = DEPLOY_HEADER_TABLES
        + DEPLOY_TABLE_IMPORT * DEPLOY_TABLE_DESC_SIZE;
    const size_t import_table = static_cast<size_t>(get(
        fixture.raw, descriptor + DEPLOY_TABLE_OFFSET, 8));
    const size_t import = import_table;
    put(fixture.raw, import + DEPLOY_IMPORT_MODE, mode, 2);
    put(fixture.raw, import + DEPLOY_IMPORT_ATTENUATION
            + DEPLOY_ATTENUATION_KIND,
        attenuation_kind, 2);
    if (mode == DEPLOY_IMPORT_CHANNEL_MINT) {
        put(fixture.raw, import + DEPLOY_IMPORT_ATTENUATION
                + DEPLOY_ATTENUATION_WORD1,
            channel_badge, 8);
        put(fixture.raw, import + DEPLOY_IMPORT_ATTENUATION
                + DEPLOY_ATTENUATION_WORD2,
            UINT64_MAX, 8);
    }
    auto parsed = deploy::ManifestView::parse(
        fixture.raw,
        sizeof(fixture.raw),
        fixture.workspace);
    if (!parsed) {
        return false;
    }
    auto decoded = deploy::DeploymentPlan::decode(
        parsed.value(), fixture.plans);
    if (!decoded) {
        return false;
    }
    fixture.plan = std::move(decoded.value());
    return true;
}

[[nodiscard]] auto make_two_import_plan(BatchFixture& fixture) noexcept -> bool {
    for (size_t index = 0; index < deploy::host::kGoldenSize; ++index) {
        fixture.raw[index] = deploy::host::kGolden[index];
    }
    const size_t descriptor = DEPLOY_HEADER_TABLES
        + DEPLOY_TABLE_IMPORT * DEPLOY_TABLE_DESC_SIZE;
    const size_t import_descriptor = descriptor;
    const size_t import_table = static_cast<size_t>(get(
        fixture.raw,
        import_descriptor + DEPLOY_TABLE_OFFSET,
        8));
    size_t next_table = sizeof(fixture.raw);
    for (size_t table = DEPLOY_TABLE_DEPENDENCY;
         table <= DEPLOY_TABLE_STRING; ++table) {
        const size_t table_descriptor = DEPLOY_HEADER_TABLES
            + table * DEPLOY_TABLE_DESC_SIZE;
        const size_t offset = static_cast<size_t>(get(
            fixture.raw, table_descriptor + DEPLOY_TABLE_OFFSET, 8));
        if (offset > import_table && offset < next_table) {
            next_table = offset;
        }
    }
    if (next_table == sizeof(fixture.raw)) {
        return false;
    }
    for (size_t index = deploy::host::kGoldenSize;
         index > next_table; --index) {
        fixture.raw[index + DEPLOY_IMPORT_STRIDE - 1] =
            fixture.raw[index - 1];
    }
    for (size_t table = DEPLOY_TABLE_DEPENDENCY;
         table <= DEPLOY_TABLE_STRING; ++table) {
        const size_t table_descriptor = DEPLOY_HEADER_TABLES
            + table * DEPLOY_TABLE_DESC_SIZE;
        const size_t offset = static_cast<size_t>(get(
            fixture.raw, table_descriptor + DEPLOY_TABLE_OFFSET, 8));
        if (offset >= next_table && offset != 0) {
            put(fixture.raw, table_descriptor + DEPLOY_TABLE_OFFSET,
                offset + DEPLOY_IMPORT_STRIDE, 8);
        }
    }
    put(fixture.raw, DEPLOY_HEADER_TOTAL_SIZE,
        deploy::host::kGoldenSize + DEPLOY_IMPORT_STRIDE, 8);
    const size_t task_descriptor = DEPLOY_HEADER_TABLES
        + DEPLOY_TABLE_TASK * DEPLOY_TABLE_DESC_SIZE;
    const size_t task_table = static_cast<size_t>(get(
        fixture.raw, task_descriptor + DEPLOY_TABLE_OFFSET, 8));
    put(fixture.raw, task_table + DEPLOY_TASK_IMPORT_COUNT, 2, 4);
    put(fixture.raw, import_descriptor + DEPLOY_TABLE_COUNT_FIELD, 2, 4);
    for (size_t index = 0; index < DEPLOY_IMPORT_STRIDE; ++index) {
        fixture.raw[import_table + DEPLOY_IMPORT_STRIDE + index] =
            fixture.raw[import_table + index];
    }
    /* The copied row must receive a distinct local destination symbol.  The
     * export key is an existing bounded symbol that is not part of the
     * task-local produced-key set, so it keeps this fixture self-contained
     * without inventing a second string-table representation. */
    const size_t export_descriptor = DEPLOY_HEADER_TABLES
        + DEPLOY_TABLE_EXPORT * DEPLOY_TABLE_DESC_SIZE;
    const size_t export_table = static_cast<size_t>(get(
        fixture.raw, export_descriptor + DEPLOY_TABLE_OFFSET, 8));
    const uint64_t export_key = get(
        fixture.raw, export_table + DEPLOY_EXPORT_KEY, 8);
    put(fixture.raw,
        import_table + DEPLOY_IMPORT_STRIDE
            + DEPLOY_IMPORT_DESTINATION,
        export_key, 8);
    auto parsed = deploy::ManifestView::parse(
        fixture.raw, sizeof(fixture.raw), fixture.workspace);
    if (!parsed || parsed.value().import_count() != 2) {
        return false;
    }
    auto decoded = deploy::DeploymentPlan::decode(
        parsed.value(), fixture.plans);
    if (!decoded) {
        return false;
    }
    fixture.plan = std::move(decoded.value());
    return fixture.plan.import_count() == 2;
}

[[nodiscard]] constexpr auto ceiling(
    obj_kind_t kind,
    uint64_t rights = RIGHT_MASK) noexcept -> CapView {
    return CapView{
        .version = CAP_ATTENUATION_VERSION_CURRENT,
        .kind = kind,
        .size = CAP_ATTENUATION_SIZE,
        .rights = rights,
        .words = {},
    };
}

[[nodiscard]] auto test_descriptor_containment() noexcept -> bool {
    using deploy::attenuation::DescriptorForm;
    using deploy::attenuation::valid_descriptor;
    using deploy::attenuation::within;

    auto invalid_memory = ceiling(OBJECT_KIND_MEMORY);
    if (deploy::valid_authority_ceiling(invalid_memory)) {
        return false;
    }
    auto invalid_version = ceiling(OBJECT_KIND_THREAD);
    invalid_version.version = 0;
    auto invalid_size = ceiling(OBJECT_KIND_THREAD);
    invalid_size.size = 0;
    auto invalid_kind = ceiling(OBJECT_KIND_THREAD);
    invalid_kind.kind = OBJECT_KIND_COUNT;
    auto invalid_rights = ceiling(OBJECT_KIND_THREAD);
    invalid_rights.rights = ~uint64_t{RIGHT_MASK};
    if (valid_descriptor(invalid_version, DescriptorForm::Ceiling)
        || valid_descriptor(invalid_size, DescriptorForm::Ceiling)
        || valid_descriptor(invalid_kind, DescriptorForm::Ceiling)
        || valid_descriptor(invalid_rights, DescriptorForm::Ceiling)) {
        return false;
    }

    const auto thread_ceiling = ceiling(
        OBJECT_KIND_THREAD, RIGHT_DUPLICATE);
    auto thread_request = ceiling(
        OBJECT_KIND_THREAD,
        RIGHT_DUPLICATE | RIGHT_DELEGATE);
    if (!valid_descriptor(thread_ceiling, DescriptorForm::Ceiling)
        || within(
            thread_request, thread_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)) {
        return false;
    }

    auto memory_ceiling = ceiling(OBJECT_KIND_MEMORY);
    memory_ceiling.words[0] = 100;
    memory_ceiling.words[1] = 100;
    memory_ceiling.words[2] = VM_READ | VM_WRITE;
    auto memory_request = memory_ceiling;
    memory_request.words[0] = 120;
    memory_request.words[1] = 10;
    memory_request.words[2] = VM_READ;
    if (!deploy::valid_authority_ceiling(memory_ceiling)
        || !within(
            memory_request, memory_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)) {
        return false;
    }
    auto memory_escape = memory_request;
    memory_escape.words[0] = 190;
    memory_escape.words[1] = 20;
    auto memory_access = memory_request;
    memory_access.words[2] = VM_READ | VM_WRITE | VM_EXECUTE;
    auto memory_reserved = memory_request;
    memory_reserved.words[3] = 1;
    auto memory_overflow = memory_ceiling;
    memory_overflow.words[0] = UINT64_MAX - 1;
    memory_overflow.words[1] = 2;
    auto duplicate_typed = ceiling(OBJECT_KIND_MEMORY);
    duplicate_typed.words[0] = 1;
    if (within(
            memory_escape, memory_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            memory_access, memory_request,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            memory_reserved, memory_request,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || valid_descriptor(memory_overflow, DescriptorForm::Ceiling)
        || valid_descriptor(duplicate_typed, DescriptorForm::DuplicateRequest)) {
        return false;
    }

    auto vspace_ceiling = ceiling(OBJECT_KIND_VSPACE);
    vspace_ceiling.words[0] = 0x1000;
    vspace_ceiling.words[1] = 0x4000;
    vspace_ceiling.words[2] = VM_READ | VM_WRITE;
    auto vspace_request = vspace_ceiling;
    vspace_request.words[0] = 0x2000;
    vspace_request.words[1] = 0x1000;
    vspace_request.words[2] = VM_READ;
    auto vspace_escape = vspace_request;
    vspace_escape.words[0] = 0x4000;
    vspace_escape.words[1] = 0x2000;
    auto vspace_unaligned = vspace_request;
    vspace_unaligned.words[0] = 0x1800;
    auto vspace_overflow = vspace_ceiling;
    vspace_overflow.words[0] = UINT64_MAX - 0xfff;
    vspace_overflow.words[1] = 0x2000;
    if (!within(
            vspace_request, vspace_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            vspace_escape, vspace_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || valid_descriptor(vspace_unaligned, DescriptorForm::Ceiling)
        || valid_descriptor(vspace_overflow, DescriptorForm::Ceiling)) {
        return false;
    }

    auto pool_ceiling = ceiling(OBJECT_KIND_RESOURCE_POOL);
    pool_ceiling.words[0] = 100;
    pool_ceiling.words[1] = 10;
    pool_ceiling.words[2] = (uint64_t{1} << OBJECT_KIND_MEMORY)
        | (uint64_t{1} << OBJECT_KIND_VSPACE);
    auto pool_request = pool_ceiling;
    pool_request.words[0] = 50;
    pool_request.words[1] = 5;
    pool_request.words[2] = uint64_t{1} << OBJECT_KIND_MEMORY;
    auto pool_budget = pool_request;
    pool_budget.words[0] = 101;
    auto pool_mask = pool_request;
    pool_mask.words[2] |= uint64_t{1} << OBJECT_KIND_ENDPOINT;
    if (!within(
            pool_request, pool_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            pool_budget, pool_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            pool_mask, pool_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)) {
        return false;
    }

    auto endpoint_ceiling = ceiling(OBJECT_KIND_ENDPOINT);
    endpoint_ceiling.words[0] = 3;
    endpoint_ceiling.words[1] = 3;
    endpoint_ceiling.words[2] = 4;
    auto endpoint_request = endpoint_ceiling;
    endpoint_request.words[0] = 3;
    endpoint_request.words[1] = 7;
    endpoint_request.words[2] = 2;
    auto endpoint_fixed = endpoint_request;
    endpoint_fixed.words[1] = 1;
    auto endpoint_badge = endpoint_request;
    endpoint_badge.words[0] = 1;
    auto endpoint_limit = endpoint_request;
    endpoint_limit.words[2] = 5;
    if (!within(
            endpoint_request, endpoint_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            endpoint_fixed, endpoint_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            endpoint_badge, endpoint_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            endpoint_limit, endpoint_ceiling,
            DEPLOY_IMPORT_TYPED_DELEGATE)) {
        return false;
    }

    auto channel_unbound = ceiling(OBJECT_KIND_CHANNEL);
    channel_unbound.words[0] = CAP_CHANNEL_SIDE_A;
    auto channel_exact = channel_unbound;
    channel_exact.words[1] = 9;
    channel_exact.words[2] = UINT64_MAX;
    auto channel_other = channel_exact;
    channel_other.words[1] = 10;
    auto channel_b = channel_exact;
    channel_b.words[0] = CAP_CHANNEL_SIDE_B;
    auto channel_zero_badge = channel_unbound;
    channel_zero_badge.words[2] = UINT64_MAX;
    auto channel_partial_fixed = channel_exact;
    channel_partial_fixed.words[2] = UINT64_MAX - 1;
    if (!within(
            channel_unbound, channel_unbound,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            channel_exact, channel_unbound,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || !within(
            channel_exact, channel_unbound,
            DEPLOY_IMPORT_CHANNEL_MINT)
        || within(
            channel_exact, channel_exact,
            DEPLOY_IMPORT_CHANNEL_MINT)
        || within(
            channel_other, channel_exact,
            DEPLOY_IMPORT_TYPED_DELEGATE)
        || within(
            channel_b, channel_unbound,
            DEPLOY_IMPORT_CHANNEL_MINT)
        || within(
            channel_zero_badge, channel_unbound,
            DEPLOY_IMPORT_CHANNEL_MINT)
        || valid_descriptor(channel_partial_fixed, DescriptorForm::TypedRequest)) {
        return false;
    }

    auto pager_ceiling = ceiling(OBJECT_KIND_PAGER);
    auto pager_request = pager_ceiling;
    auto pager_extra = pager_ceiling;
    pager_extra.words[0] = 1;
    if (!within(pager_request, pager_ceiling, DEPLOY_IMPORT_TYPED_DELEGATE)
        || valid_descriptor(pager_extra, DescriptorForm::Ceiling)) return false;

    return !deploy::valid_authority_ceiling(ceiling(9))
        && !deploy::valid_authority_ceiling(ceiling(10));
}

[[nodiscard]] auto test_registration_and_lease_lifetime() noexcept -> bool {
    Authorities authorities{};
    SourceFixture<3> source{};
    if (!source.open()) {
        return false;
    }
    const auto first_slot = source.add(41, OBJECT_KIND_THREAD);
    const auto second_slot = source.add(42, OBJECT_KIND_THREAD);
    const auto reused_slot = source.add(44, OBJECT_KIND_THREAD);
    if (!first_slot || !second_slot || !reused_slot || !source.adopt()) {
        return false;
    }
    const auto thread = ceiling(OBJECT_KIND_THREAD);
    const auto first = source.register_source(
        authorities, *first_slot, 1, thread);
    const auto second = source.register_source(
        authorities, *second_slot, 2, thread);
    const auto rejected = source.register_source(
        authorities,
        deploy::LocalSlot{
            .pool = 999, .index = 0, .kind = OBJECT_KIND_THREAD},
        3, thread);
    if (!first || !second || rejected) {
        return false;
    }
    auto first_lease = authorities.lease(*first);
    if (!first_lease || source.close() != STATUS_BUSY
        || authorities.lease(*first)) {
        return false;
    }
    first_lease->release();
    if (source.close() != STATUS_OK
        || authorities.active_entries() != 0
        || authorities.lease(*first)) {
        return false;
    }
    SourceFixture<1> replacement{};
    if (!replacement.open()) {
        return false;
    }
    const auto replacement_slot = replacement.add(45, OBJECT_KIND_THREAD);
    if (!replacement_slot || !replacement.adopt()) {
        return false;
    }
    const auto reused = replacement.register_source(
        authorities, *replacement_slot, 4, thread);
    if (!reused || reused->slot != 0 || reused->generation != 2
        || replacement.close() != STATUS_OK) {
        return false;
    }
    return authorities.active_entries() == 0;
}

[[nodiscard]] auto test_generation_exhaustion() noexcept -> bool {
    using Exhausted = deploy::AuthoritySet<1, 1, 1>;
    Exhausted authorities{};
    SourceFixture<1> source{};
    if (!source.open()) {
        return false;
    }
    const auto slot = source.add(51, OBJECT_KIND_THREAD);
    if (!slot || !source.adopt()
        || !source.register_source(
            authorities, *slot, 7, ceiling(OBJECT_KIND_THREAD))
        || source.close() != STATUS_OK) {
        return false;
    }
    SourceFixture<1> replacement{};
    if (!replacement.open()) {
        return false;
    }
    const auto replacement_slot = replacement.add(52, OBJECT_KIND_THREAD);
    if (!replacement_slot || !replacement.adopt()
        || replacement.register_source(
               authorities, *replacement_slot, 8,
               ceiling(OBJECT_KIND_THREAD))) {
        return false;
    }
    return replacement.close() == STATUS_OK
        && authorities.active_entries() == 0;
}

[[nodiscard]] auto test_registration_slot_admission() noexcept -> bool {
    FakeBackend::reset();
    Authorities authorities{};
    const auto thread = ceiling(OBJECT_KIND_THREAD);

    SourceFixture<2> stale_source{};
    if (!stale_source.open()) {
        return false;
    }
    const auto stale_slot = stale_source.add(53, OBJECT_KIND_THREAD);
    if (!stale_slot || !stale_source.adopt()
        || stale_source.close() != STATUS_OK
        || stale_source.register_source(
               authorities, *stale_slot, 40, thread)
        || authorities.active_entries() != 0) {
        return false;
    }

    SourceFixture<2> source{};
    if (!source.open()) {
        return false;
    }
    const auto owned_slot = source.add(54, OBJECT_KIND_THREAD);
    if (!owned_slot || !source.adopt()) {
        return false;
    }
    const auto foreign = deploy::LocalSlot{
        .pool = 999, .index = 0, .kind = OBJECT_KIND_THREAD};
    const auto wrong_kind = source.aggregate.manager_slot();
    if (source.register_source(authorities, foreign, 41, thread)
        || source.register_source(authorities, wrong_kind, 42, thread)
        || authorities.active_entries() != 0
        || source.close() != STATUS_OK) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_duplicate_import_and_projection() noexcept -> bool {
    FakeBackend::reset();
    Fixture fixture{};
    if (!make_plan(fixture)) {
        return false;
    }
    auto plan = fixture.plan.lease();
    if (!plan) {
        return false;
    }
    Space space{};
    if (space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    Authorities authorities{};
    SourceFixture<4> source{};
    if (!source.open()) {
        return false;
    }
    const auto source_slot = source.add(77, OBJECT_KIND_THREAD);
    if (!source_slot || !source.adopt()) {
        return false;
    }
    const auto id = source.register_source(
        authorities, *source_slot, 10, ceiling(OBJECT_KIND_THREAD));
    if (!id) {
        return false;
    }
    const deploy::ImportBinding binding{*id, {}};
    deploy::ImportProjection output{};
    const status_t status =
        deploy::ImportTransaction<Space, Authorities>::run(
            space, plan->task(0), 0, 1, &binding, authorities, &output);
    const auto resolved = space.lookup_remote(
        output.remote_index, output.manager);
    if (status != STATUS_OK || !output.valid() || !resolved
        || resolved->selector != 91 || resolved->cspace != output.manager
        || FakeBackend::duplicate_calls != 1) {
        return false;
    }
    if (space.close() != STATUS_OK
        || source.close() != STATUS_OK) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_typed_and_channel_imports() noexcept -> bool {
    FakeBackend::reset();
    Fixture typed_fixture{};
    if (!make_plan(typed_fixture, DEPLOY_IMPORT_TYPED_DELEGATE)) {
        return false;
    }
    auto typed_plan = typed_fixture.plan.lease();
    if (!typed_plan) {
        return false;
    }
    Space typed_space{};
    if (typed_space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    auto descriptor = Space::owner_type{sys::cap::CapRef{88, 0}};
    const auto descriptor_slot = typed_space.adopt_local(
        std::move(descriptor), OBJECT_KIND_MEMORY);
    Authorities typed_authorities{};
    SourceFixture<4> typed_source{};
    if (!typed_source.open()) {
        return false;
    }
    const auto typed_source_slot = typed_source.add(
        78, OBJECT_KIND_THREAD);
    if (!typed_source_slot || !typed_source.adopt()) {
        return false;
    }
    const auto typed_id = typed_source.register_source(
        typed_authorities, *typed_source_slot, 11,
        ceiling(OBJECT_KIND_THREAD));
    if (!descriptor_slot || !typed_id) {
        return false;
    }
    const deploy::ImportBinding typed_binding{
        *typed_id, *descriptor_slot};
    deploy::ImportProjection typed_output{};
    const auto typed_status = deploy::ImportTransaction<Space, Authorities>::run(
            typed_space, typed_plan->task(0), 0, 1, &typed_binding,
            typed_authorities, &typed_output);
    if (typed_status != STATUS_OK
        || FakeBackend::typed_calls != 1
        || !typed_output.valid()) {
        return false;
    }
    if (typed_space.close() != STATUS_OK
        || typed_source.close() != STATUS_OK) {
        return false;
    }

    Fixture channel_fixture{};
    if (!make_plan(
            channel_fixture, DEPLOY_IMPORT_CHANNEL_MINT,
            OBJECT_KIND_CHANNEL, 9)) {
        return false;
    }
    auto channel_plan = channel_fixture.plan.lease();
    if (!channel_plan) {
        return false;
    }
    Space channel_space{};
    if (channel_space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    Authorities channel_authorities{};
    SourceFixture<4> channel_source{};
    if (!channel_source.open()) {
        return false;
    }
    const auto channel_source_slot = channel_source.add(
        79, OBJECT_KIND_CHANNEL);
    if (!channel_source_slot || !channel_source.adopt()) {
        return false;
    }
    const auto channel_id = channel_source.register_source(
        channel_authorities, *channel_source_slot, 12,
        ceiling(OBJECT_KIND_CHANNEL));
    if (!channel_id) {
        return false;
    }
    const deploy::ImportBinding channel_binding{*channel_id, {}};
    deploy::ImportProjection channel_output{};
    const auto channel_status = deploy::ImportTransaction<Space, Authorities>::run(
            channel_space, channel_plan->task(0), 0, 1, &channel_binding,
            channel_authorities, &channel_output);
    if (channel_status != STATUS_OK
        || FakeBackend::channel_calls != 1
        || !channel_output.valid()) {
        return false;
    }
    return channel_space.close() == STATUS_OK
        && channel_source.close() == STATUS_OK;
}

[[nodiscard]] auto test_import_failure_preserves_source() noexcept -> bool {
    FakeBackend::reset();
    Fixture fixture{};
    if (!make_plan(fixture)) {
        return false;
    }
    auto plan = fixture.plan.lease();
    if (!plan) {
        return false;
    }
    Space space{};
    if (space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    Authorities authorities{};
    SourceFixture<4> source{};
    if (!source.open()) {
        return false;
    }
    const auto source_slot = source.add(81, OBJECT_KIND_THREAD);
    if (!source_slot || !source.adopt()) {
        return false;
    }
    const auto id = source.register_source(
        authorities, *source_slot, 13,
        ceiling(OBJECT_KIND_THREAD));
    if (!id) {
        return false;
    }
    const deploy::ImportBinding bad_binding{
        *id,
        deploy::LocalSlot{
            .pool = 10,
            .index = 0,
            .kind = OBJECT_KIND_MEMORY}};
    deploy::ImportProjection rejected{};
    if (deploy::ImportTransaction<Space, Authorities>::run(
            space, plan->task(0), 0, 1, &bad_binding, authorities,
            &rejected) != STATUS_BAD_ARGS
        || FakeBackend::duplicate_calls != 0
        || space.remote_size() != 0) {
        return false;
    }
    FakeBackend::fail_after = 1;
    const deploy::ImportBinding binding{*id, {}};
    deploy::ImportProjection output{};
    if (deploy::ImportTransaction<Space, Authorities>::run(
            space, plan->task(0), 0, 1, &binding, authorities, &output)
            != STATUS_BUSY
        || space.remote_size() != 0
        || authorities.live_leases() != 0
        || !authorities.lease(*id)) {
        return false;
    }
    FakeBackend::fail_after = 0;
    if (deploy::ImportTransaction<Space, Authorities>::run(
            space, plan->task(0), 0, 1, &binding, authorities, &output)
            != STATUS_OK
        || !output.valid()
        || space.close() != STATUS_OK
        || source.close() != STATUS_OK) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_non_ok_nonzero_result_is_closed() noexcept -> bool {
    FakeBackend::reset();
    BatchFixture fixture{};
    if (!make_two_import_plan(fixture)) {
        return false;
    }
    auto plan = fixture.plan.lease();
    if (!plan) {
        return false;
    }
    Space space{};
    if (space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    Authorities authorities{};
    SourceFixture<4> source{};
    if (!source.open()) {
        return false;
    }
    const auto source_slot = source.add(83, OBJECT_KIND_THREAD);
    if (!source_slot || !source.adopt()) {
        return false;
    }
    const auto id = source.register_source(
        authorities, *source_slot, 15,
        ceiling(OBJECT_KIND_THREAD));
    if (!id) {
        return false;
    }
    const deploy::ImportBinding bindings[2]{{*id, {}}, {*id, {}}};
    deploy::ImportProjection outputs[2]{};
    FakeBackend::fail_nonzero_after = 2;
    const auto status = deploy::ImportTransaction<Space, Authorities>::run(
        space, plan->task(0), 0, 2, bindings, authorities, outputs);
    if (status != STATUS_BUSY
        || space.remote_live_size() != 0
        || FakeBackend::closed_count != 2
        || FakeBackend::closed_selectors[0] != 92
        || FakeBackend::closed_selectors[1] != 91
        || outputs[0].valid() || outputs[1].valid()) {
        return false;
    }
    auto source_retry = authorities.lease(*id);
    if (!source_retry) {
        return false;
    }
    source_retry.reset();
    FakeBackend::fail_nonzero_after = 0;
    if (space.close() != STATUS_OK
        || source.close() != STATUS_OK) {
        return false;
    }

    FakeBackend::reset();
    auto second_plan = fixture.plan.lease();
    if (!second_plan) {
        return false;
    }
    Space failing_space{};
    if (failing_space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    Authorities failing_authorities{};
    SourceFixture<4> failing_source{};
    if (!failing_source.open()) {
        return false;
    }
    const auto failing_source_slot = failing_source.add(
        84, OBJECT_KIND_THREAD);
    if (!failing_source_slot || !failing_source.adopt()) {
        return false;
    }
    const auto failing_id = failing_source.register_source(
        failing_authorities, *failing_source_slot, 16,
        ceiling(OBJECT_KIND_THREAD));
    if (!failing_id) {
        return false;
    }
    FakeBackend::fail_nonzero_after = 1;
    FakeBackend::close_failure_selector = 91;
    const deploy::ImportBinding failing_binding{*failing_id, {}};
    deploy::ImportProjection failing_output{};
    const auto failing_status =
        deploy::ImportTransaction<Space, Authorities>::run(
            failing_space, second_plan->task(0), 0, 1, &failing_binding,
            failing_authorities, &failing_output);
    FakeBackend::close_failure_selector = 0;
    if (failing_status != STATUS_BUSY
        || !FakeBackend::ownership_faulted
        || FakeBackend::closed_count == 0
        || FakeBackend::closed_selectors[0] != 91
        || failing_space.remote_live_size() != 0
        || failing_space.close() != STATUS_OK
        || failing_source.close() != STATUS_OK) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_remote_capacity_preflight() noexcept -> bool {
    FakeBackend::reset();
    Fixture fixture{};
    if (!make_plan(fixture)) {
        return false;
    }
    auto plan = fixture.plan.lease();
    if (!plan) {
        return false;
    }
    Space space{};
    if (space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    const auto manager = space.lookup(
        space.manager_slot(), OBJECT_KIND_CSPACE);
    if (!manager) {
        return false;
    }
    for (size_t index = 0; index < Space::remote_capacity(); ++index) {
        auto owner = Space::owner_type{sys::cap::CapRef{
            static_cast<cap_t>(100 + index), manager->selector}};
        if (!space.adopt_remote_index(std::move(owner))) {
            return false;
        }
    }
    Authorities authorities{};
    SourceFixture<4> source{};
    if (!source.open()) {
        return false;
    }
    const auto source_slot = source.add(82, OBJECT_KIND_THREAD);
    if (!source_slot || !source.adopt()) {
        return false;
    }
    const auto id = source.register_source(
        authorities, *source_slot, 14,
        ceiling(OBJECT_KIND_THREAD));
    if (!id) {
        return false;
    }
    const deploy::ImportBinding binding{*id, {}};
    deploy::ImportProjection output{};
    const auto status = deploy::ImportTransaction<Space, Authorities>::run(
        space, plan->task(0), 0, 1, &binding, authorities, &output);
    return status == STATUS_NO_MEMORY
        && FakeBackend::duplicate_calls == 0
        && space.close() == STATUS_OK
        && source.close() == STATUS_OK;
}

[[nodiscard]] auto test_lease_capacity_pressure() noexcept -> bool {
    FakeBackend::reset();
    using PressureAuthorities = deploy::AuthoritySet<2, 2>;
    PressureAuthorities authorities{};
    SourceFixture<2> source{};
    if (!source.open()) {
        return false;
    }
    const auto first_slot = source.add(93, OBJECT_KIND_THREAD);
    const auto second_slot = source.add(94, OBJECT_KIND_THREAD);
    if (!first_slot || !second_slot || !source.adopt()) {
        return false;
    }
    const auto first = source.register_source(
        authorities, *first_slot, 30, ceiling(OBJECT_KIND_THREAD));
    const auto second = source.register_source(
        authorities, *second_slot, 31, ceiling(OBJECT_KIND_THREAD));
    if (!first || !second || authorities.active_entries() != 2) {
        return false;
    }
    auto held_first = authorities.lease(*first);
    auto held_second = authorities.lease(*second);
    if (!held_first || !held_second
        || authorities.live_leases() != 2
        || authorities.lease(*first)
        || authorities.active_entries() != 2) {
        return false;
    }
    held_first->release();
    auto reacquired = authorities.lease(*first);
    if (!reacquired || authorities.live_leases() != 2) {
        return false;
    }
    if (source.close() != STATUS_BUSY
        || authorities.lease(*first) || authorities.lease(*second)
        || authorities.active_entries() != 2
        || FakeBackend::resource_close_calls != 0) {
        return false;
    }
    held_second->release();
    reacquired->release();
    return source.close() == STATUS_OK
        && authorities.active_entries() == 0
        && authorities.live_leases() == 0
        && FakeBackend::resource_close_calls == 1;
}

[[nodiscard]] auto test_batch_boundaries_and_rollback() noexcept -> bool {
    BatchFixture fixture{};
    if (!make_two_import_plan(fixture)) {
        return false;
    }
    auto plan = fixture.plan.lease();
    if (!plan) {
        return false;
    }

    /* BatchMax is an admission bound, so an oversized batch has no lease or
     * backend side effect even when every row and binding is otherwise valid. */
    FakeBackend::reset();
    Space bounded_space{};
    if (bounded_space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    SourceFixture<2> bounded_source{};
    if (!bounded_source.open()) {
        return false;
    }
    const auto bounded_slot = bounded_source.add(
        87, OBJECT_KIND_THREAD);
    if (!bounded_slot || !bounded_source.adopt()) {
        return false;
    }
    Authorities bounded_authorities{};
    const auto bounded_id = bounded_source.register_source(
        bounded_authorities, *bounded_slot, 20,
        ceiling(OBJECT_KIND_THREAD));
    if (!bounded_id) {
        return false;
    }
    const deploy::ImportBinding bounded_bindings[2]{
        {*bounded_id, {}}, {*bounded_id, {}}};
    deploy::ImportProjection bounded_outputs[2]{};
    if (deploy::ImportTransaction<Space, Authorities, 1>::run(
            bounded_space, plan->task(0), 0, 2, bounded_bindings,
            bounded_authorities, bounded_outputs) != STATUS_BAD_ARGS
        || FakeBackend::duplicate_calls != 0
        || bounded_authorities.live_leases() != 0
        || bounded_authorities.active_entries() != 1
        || bounded_space.remote_size() != 0
        || bounded_space.close() != STATUS_OK
        || bounded_source.close() != STATUS_OK) {
        return false;
    }

    /* A zero-selector failure after one success reverse-closes only the
     * current batch.  The source lease and registration remain usable. */
    FakeBackend::reset();
    Space zero_space{};
    if (zero_space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    SourceFixture<2> zero_source{};
    if (!zero_source.open()) {
        return false;
    }
    const auto zero_slot = zero_source.add(88, OBJECT_KIND_THREAD);
    if (!zero_slot || !zero_source.adopt()) {
        return false;
    }
    Authorities zero_authorities{};
    const auto zero_id = zero_source.register_source(
        zero_authorities, *zero_slot, 21,
        ceiling(OBJECT_KIND_THREAD));
    if (!zero_id) {
        return false;
    }
    const deploy::ImportBinding zero_bindings[2]{
        {*zero_id, {}}, {*zero_id, {}}};
    deploy::ImportProjection zero_outputs[2]{};
    FakeBackend::fail_after = 2;
    const auto zero_status =
        deploy::ImportTransaction<Space, Authorities>::run(
            zero_space, plan->task(0), 0, 2, zero_bindings,
            zero_authorities, zero_outputs);
    FakeBackend::fail_after = 0;
    auto zero_retry = zero_authorities.lease(*zero_id);
    if (zero_status != STATUS_BUSY
        || FakeBackend::duplicate_calls != 2
        || FakeBackend::closed_count != 1
        || FakeBackend::closed_selectors[0] != 91
        || zero_space.remote_size() != 1
        || zero_space.remote_live_size() != 0
        || !zero_retry
        || zero_outputs[0].valid() || zero_outputs[1].valid()) {
        return false;
    }
    zero_retry.reset();
    if (zero_space.close() != STATUS_OK
        || zero_source.close() != STATUS_OK) {
        return false;
    }

    /* If reverse close is BUSY, the TaskSpace owner remains live for the
     * enclosing unpublished Task's later strong-close retry. */
    FakeBackend::reset();
    Space retained_space{};
    if (retained_space.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    SourceFixture<2> retained_source{};
    if (!retained_source.open()) {
        return false;
    }
    const auto retained_slot = retained_source.add(
        89, OBJECT_KIND_THREAD);
    if (!retained_slot || !retained_source.adopt()) {
        return false;
    }
    Authorities retained_authorities{};
    const auto retained_id = retained_source.register_source(
        retained_authorities, *retained_slot, 22,
        ceiling(OBJECT_KIND_THREAD));
    if (!retained_id) {
        return false;
    }
    const deploy::ImportBinding retained_bindings[2]{
        {*retained_id, {}}, {*retained_id, {}}};
    deploy::ImportProjection retained_outputs[2]{};
    FakeBackend::fail_after = 2;
    FakeBackend::close_failure_selector = 91;
    const auto retained_status =
        deploy::ImportTransaction<Space, Authorities>::run(
            retained_space, plan->task(0), 0, 2, retained_bindings,
            retained_authorities, retained_outputs);
    FakeBackend::fail_after = 0;
    FakeBackend::close_failure_selector = 0;
    if (retained_status != STATUS_BUSY
        || retained_space.remote_size() != 1
        || retained_space.remote_live_size() != 1
        || FakeBackend::closed_count != 1
        || FakeBackend::closed_selectors[0] != 91
        || retained_space.close_remote(0) != STATUS_OK
        || retained_space.remote_live_size() != 0
        || retained_space.close() != STATUS_OK
        || retained_source.close() != STATUS_OK) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_registered_space_bootstrap_close() noexcept -> bool {
    FakeBackend::reset();
    Space raw{};
    if (raw.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    const auto source_slot = raw.adopt_local(
        Space::owner_type{sys::cap::CapRef{85, 0}},
        OBJECT_KIND_THREAD);
    if (!source_slot) {
        return false;
    }
    deploy::RegisteredSpace<Space, 2> bootstrap{};
    Space closed{};
    if (bootstrap.adopt(std::move(closed))
        || closed.phase() != deploy::Phase::Closed
        || !bootstrap.adopt(std::move(raw))
        || raw.phase() != deploy::Phase::Closed
        || bootstrap.phase() != deploy::Phase::Open) {
        return false;
    }
    Authorities authorities{};
    const auto id = bootstrap.register_source(
        authorities, *source_slot, 17,
        ceiling(OBJECT_KIND_THREAD));
    if (!id) {
        return false;
    }
    auto lease = authorities.lease(*id);
    if (!lease) {
        return false;
    }
    if (bootstrap.close() != STATUS_BUSY
        || FakeBackend::resource_close_calls != 0
        || FakeBackend::close_calls != 0) {
        return false;
    }
    lease.reset();
    if (bootstrap.close() != STATUS_OK
        || FakeBackend::resource_close_calls != 1
        || bootstrap.phase() != deploy::Phase::Closed) {
        return false;
    }

    Space replacement{};
    if (replacement.open(
            sys::cap::CapRef{1, 0}, 4096, 64, 0x100, 16, 2)
        != STATUS_OK) {
        return false;
    }
    if (bootstrap.adopt(std::move(replacement))
        || replacement.phase() != deploy::Phase::Open
        || replacement.close() != STATUS_OK) {
        return false;
    }
    return true;
}

[[nodiscard]] auto test_move_rejected_at_parser() noexcept -> bool {
    FakeBackend::reset();
    Fixture fixture{};
    for (size_t index = 0; index < sizeof(fixture.raw); ++index) {
        fixture.raw[index] = deploy::host::kGolden[index];
    }
    const size_t descriptor = DEPLOY_HEADER_TABLES
        + DEPLOY_TABLE_IMPORT * DEPLOY_TABLE_DESC_SIZE;
    const size_t import = static_cast<size_t>(get(
        fixture.raw,
        descriptor + DEPLOY_TABLE_OFFSET,
        8));
    put(fixture.raw, import + DEPLOY_IMPORT_MODE,
        DEPLOY_IMPORT_MOVE, 2);
    auto parsed = deploy::ManifestView::parse(
        fixture.raw, sizeof(fixture.raw), fixture.workspace);
    return !parsed && FakeBackend::duplicate_calls == 0
        && FakeBackend::typed_calls == 0 && FakeBackend::channel_calls == 0;
}

using Test = bool (*)() noexcept;

struct Case final {
    const char* name;
    Test test;
};

} // namespace

int main() {
    const Case cases[] = {
        {"descriptor containment", test_descriptor_containment},
        {"registration/lease lifetime", test_registration_and_lease_lifetime},
        {"authority generation exhaustion", test_generation_exhaustion},
        {"registration slot admission", test_registration_slot_admission},
        {"duplicate import/projection", test_duplicate_import_and_projection},
        {"typed and channel imports", test_typed_and_channel_imports},
        {"failed import preserves source", test_import_failure_preserves_source},
        {"non-OK non-zero result cleanup",
         test_non_ok_nonzero_result_is_closed},
        {"remote capacity preflight", test_remote_capacity_preflight},
        {"lease capacity pressure", test_lease_capacity_pressure},
        {"batch boundaries and rollback",
         test_batch_boundaries_and_rollback},
        {"bootstrap registered-space close",
         test_registered_space_bootstrap_close},
        {"move parser rejection", test_move_rejected_at_parser},
    };
    size_t passed = 0;
    size_t failed = 0;
    for (const Case& test : cases) {
        if (test.test()) {
            ++passed;
        } else {
            ++failed;
            printf("FAIL: %s\n", test.name);
        }
    }
    printf("task authority: passed=%zu failed=%zu\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
