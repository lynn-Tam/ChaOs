#include <test/test.hpp>

#include <libk/align.hpp>
#include <array>
#include <libk/byte_reader.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/fmt.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/inplace_ring.hpp>
#include <limits>
#include <libk/scope_guard.hpp>
#include <libk/sync/atomic.hpp>
#include <utility>

namespace {
struct MoveOnly {
    int value;
    explicit MoveOnly(int n) noexcept : value(n) {}
    MoveOnly(const MoveOnly&) = delete;
    auto operator=(const MoveOnly&) -> MoveOnly& = delete;
    MoveOnly(MoveOnly&&) = default;
    auto operator=(MoveOnly&&) -> MoveOnly& = default;
};

enum class AtomicPhase : uint32_t {
    Empty,
    Ready,
    Online,
};

struct UnsupportedAtomicValue {
    uint32_t first;
    uint32_t second;
};

struct TreeNode final {
    int key{};
    int tie{};
    libk::IntrusiveTreeHook hook{};
};

struct TreeLess final {
    [[nodiscard]] auto operator()(
        const TreeNode& lhs,
        const TreeNode& rhs) const noexcept -> bool {
        return lhs.key < rhs.key
            || (lhs.key == rhs.key && lhs.tie < rhs.tie);
    }
};

using TestTree = libk::IntrusiveTree<
    TreeNode, &TreeNode::hook, TreeLess>;

static_assert(libk::AtomicValue<uint8_t>);
static_assert(libk::AtomicValue<uint16_t>);
static_assert(libk::AtomicValue<uint32_t>);
static_assert(libk::AtomicValue<uint64_t>);
static_assert(libk::AtomicValue<AtomicPhase>);
static_assert(libk::AtomicValue<int*>);
static_assert(!libk::AtomicValue<UnsupportedAtomicValue>);
static_assert(!libk::AtomicValue<volatile uint32_t>);
static_assert(libk::AtomicHasScalarLayout<uint32_t>);
static_assert(libk::AtomicHasScalarLayout<int*>);
static_assert(!std::is_copy_constructible_v<libk::Atomic<uint32_t>>);
static_assert(!std::is_move_constructible_v<libk::Atomic<uint32_t>>);
static_assert(std::is_trivially_destructible_v<libk::Atomic<uint32_t>>);

template<typename AtomicType>
concept HasAcquireLoad = requires(const AtomicType& value) {
    value.template load<libk::MemoryOrder::Acquire>();
};

template<typename AtomicType>
concept HasReleaseLoad = requires(const AtomicType& value) {
    value.template load<libk::MemoryOrder::Release>();
};

template<typename AtomicType>
concept HasReleaseStore = requires(AtomicType& value) {
    value.template store<libk::MemoryOrder::Release>({});
};

template<typename AtomicType>
concept HasAcquireStore = requires(AtomicType& value) {
    value.template store<libk::MemoryOrder::Acquire>({});
};

template<typename AtomicType>
concept HasValidAcqRelCas = requires(
    AtomicType& value,
    AtomicPhase& expected) {
    value.template compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, AtomicPhase::Online);
};

template<typename AtomicType>
concept HasInvalidReleaseAcquireCas = requires(
    AtomicType& value,
    AtomicPhase& expected) {
    value.template compare_exchange_strong<
        libk::MemoryOrder::Release,
        libk::MemoryOrder::Acquire>(expected, AtomicPhase::Online);
};

static_assert(HasAcquireLoad<libk::Atomic<AtomicPhase>>);
static_assert(!HasReleaseLoad<libk::Atomic<AtomicPhase>>);
static_assert(HasReleaseStore<libk::Atomic<AtomicPhase>>);
static_assert(!HasAcquireStore<libk::Atomic<AtomicPhase>>);
static_assert(HasValidAcqRelCas<libk::Atomic<AtomicPhase>>);
static_assert(!HasInvalidReleaseAcquireCas<libk::Atomic<AtomicPhase>>);

[[nodiscard]] auto same_text(libk::StrView actual, const char* expected) noexcept
    -> bool {
    return actual == libk::StrView::from_cstr(expected);
}

bool test_checked_arithmetic_reports_overflow(const TestContext&) noexcept {
    constexpr size_t max = std::numeric_limits<size_t>::max();

    const auto add = libk::checked_add<size_t>(40, 2);
    const auto add_overflow = libk::checked_add<size_t>(max, 1);
    const auto multiply = libk::checked_multiply<size_t>(7, 6);
    const auto multiply_overflow =
        libk::checked_multiply<size_t>(max / 2 + 1, 2);
    const auto exact_align =
        libk::checked_align_up<size_t>(0x1000, 0x1000);
    const auto rounded_align =
        libk::checked_align_up<size_t>(0x1001, 0x1000);
    const auto align_overflow =
        libk::checked_align_up<size_t>(max - 1, 4);
    const auto zero_align =
        libk::checked_align_up<size_t>(16, 0);
    const auto non_power_align =
        libk::checked_align_up<size_t>(16, 3);

    return add.has_value() && add.value() == 42
        && !add_overflow.has_value()
        && multiply.has_value() && multiply.value() == 42
        && !multiply_overflow.has_value()
        && exact_align.has_value() && exact_align.value() == 0x1000
        && rounded_align.has_value() && rounded_align.value() == 0x2000
        && !align_overflow.has_value()
        && !zero_align.has_value()
        && !non_power_align.has_value();
}

bool test_array_and_string_views_preserve_empty_contracts(
    const TestContext&) noexcept {
    std::array<int, 0> empty_array{};
    std::array<int, 3> values{{4, 5, 6}};
    const libk::StrView empty{};
    const libk::StrView text{"kernel"};

    return empty_array.empty()
        && empty_array.size() == 0
        && empty_array.begin() == empty_array.end()
        && !values.empty()
        && values.size() == 3
        && values[0] == 4
        && values[2] == 6
        && empty.empty()
        && empty.begin() == nullptr
        && empty.end() == nullptr
        && text == "kernel"
        && text.starts_with("ker")
        && !text.starts_with("kernel-space")
        && text.substr(3) == "nel"
        && text.substr(0, 0).empty();
}

bool test_byte_reader_failure_is_transactional(
    const TestContext&) noexcept {
    alignas(8) const uint8_t bytes[] = {
        0x01, 0x23, 0x45, 0x67,
        'o', 'k', 0,
        0xff,
    };

    libk::ByteReader reader{bytes, sizeof(bytes)};
    uint32_t word{};
    libk::StrView text{};
    if (!reader.read_be32(word)
        || word != UINT32_C(0x01234567)
        || !reader.read_cstr(text)
        || text != "ok"
        || !reader.align(8)
        || reader.remaining() != 0) {
        return false;
    }

    libk::ByteReader little{bytes, sizeof(bytes)};
    uint16_t half{};
    uint32_t little_word{};
    if (!little.read_le16(half)
        || half != UINT16_C(0x2301)
        || little.offset() != sizeof(uint16_t)
        || !little.read_le32(little_word)
        || little_word != UINT32_C(0x6b6f6745)) {
        return false;
    }

    libk::ByteReader short_reader{bytes, sizeof(uint32_t)};
    uint64_t wide = UINT64_C(0xfeedfacecafebeef);
    const uint8_t* const original_ptr = short_reader.ptr();
    if (short_reader.read_be64(wide)
        || wide != UINT64_C(0xfeedfacecafebeef)
        || short_reader.ptr() != original_ptr
        || short_reader.remaining() != sizeof(uint32_t)) {
        return false;
    }

    libk::ByteSpan output{bytes, 1};
    if (short_reader.take_bytes(sizeof(bytes), output)
        || output.data() != bytes
        || output.size() != 1
        || short_reader.ptr() != original_ptr) {
        return false;
    }

    libk::ByteReader unterminated{bytes, sizeof(uint32_t)};
    libk::StrView unchanged{"unchanged"};
    if (unterminated.read_cstr(unchanged)
        || unchanged != "unchanged"
        || unterminated.remaining() != sizeof(uint32_t)) {
        return false;
    }

    libk::ByteReader alignment_failure{bytes, 2};
    if (!alignment_failure.skip(1)) {
        return false;
    }
    const uint8_t* const unaligned_ptr = alignment_failure.ptr();
    if (alignment_failure.align(3)
        || alignment_failure.ptr() != unaligned_ptr
        || alignment_failure.remaining() != 1
        || alignment_failure.align(8)
        || alignment_failure.ptr() != unaligned_ptr
        || alignment_failure.remaining() != 1) {
        return false;
    }

    libk::ByteReader empty_reader{nullptr, 0};
    return empty_reader.ptr() == nullptr
        && empty_reader.remaining() == 0
        && empty_reader.align(4)
        && !empty_reader.align(3)
        && empty_reader.skip(0);
}

bool test_inplace_vector_handles_aliasing_and_zero_capacity(
    const TestContext&) noexcept {
    libk::InplaceVector<int, 0> empty{};
    if (!empty.empty()
        || empty.data() != nullptr
        || empty.begin() != empty.end()
        || empty.try_push_back(1)) {
        return false;
    }

    libk::InplaceVector<int, 5> values{};
    if (!values.try_push_back(1)
        || !values.try_push_back(2)
        || !values.try_push_back(3)) {
        return false;
    }

    int* const inserted = values.insert(values.begin() + 1, values[2]);
    if (inserted != values.begin() + 1
        || values.size() != 4
        || values[0] != 1
        || values[1] != 3
        || values[2] != 2
        || values[3] != 3) {
        return false;
    }

    values.replace(values.begin(), values[3]);
    int* const next = values.erase(values.begin() + 2);
    if (next != values.begin() + 2
        || values.size() != 3
        || values[0] != 3
        || values[1] != 3
        || values[2] != 3) {
        return false;
    }

    libk::InplaceVector<MoveOnly, 2> source{};
    if (!source.try_emplace_back(7) || !source.try_emplace_back(9)) {
        return false;
    }
    libk::InplaceVector<MoveOnly, 2> moved{std::move(source)};
    return source.empty()
        && moved.size() == 2
        && moved[0].value == 7
        && moved[1].value == 9;
}

bool test_inplace_ring_erase_preserves_logical_order(
    const TestContext&) noexcept {
    libk::InplaceRing<int, 4> plain{};
    plain.emplace_back(1);
    plain.emplace_back(2);
    plain.emplace_back(3);
    auto plain_position = plain.begin();
    ++plain_position;
    auto plain_next = plain.erase(plain_position);
    if (plain.size() != 2
        || plain[0] != 1
        || plain[1] != 3
        || plain_next == plain.end()
        || *plain_next != 3) {
        return false;
    }

    libk::InplaceRing<int, 4> wrapped{};
    wrapped.emplace_back(1);
    wrapped.emplace_back(2);
    wrapped.emplace_back(3);
    wrapped.pop_front();
    wrapped.pop_front();
    wrapped.emplace_back(4);
    wrapped.emplace_back(5);
    auto wrapped_position = wrapped.begin();
    ++wrapped_position;
    auto wrapped_next = wrapped.erase(wrapped_position);
    return wrapped.size() == 2
        && wrapped[0] == 3
        && wrapped[1] == 5
        && wrapped_next != wrapped.end()
        && *wrapped_next == 5;
}

bool test_alignment(const TestContext&) noexcept {
    constexpr size_t max = std::numeric_limits<size_t>::max();
    const auto overflow = libk::checked_align_up(max, size_t{8});

    return libk::align_up<size_t>(0x1001, 0x1000) == 0x2000
        && !overflow.has_value()
;
}

bool test_fmt_is_bounded_and_copy_elision_independent(const TestContext&) noexcept {
    libk::fmt::fixed_buffer<96> output;
    const auto formatted = libk::fmt::format_to(
        output,
        "value={} hex={:#08x} bin={:#b}",
        -42,
        0x2au,
        5u);
    if (!formatted.ok()
        || !same_text(output.view(), "value=-42 hex=0x00002a bin=0b101")) {
        return false;
    }

    const char raw_text[3] = {'a', 'b', 'c'};
    libk::fmt::fixed_buffer<8> bounded_output;
    const auto bounded = libk::fmt::format_to(bounded_output, "{}", raw_text);
    if (!bounded.ok() || !same_text(bounded_output.view(), "abc")) {
        return false;
    }

    char truncated[4]{};
    const auto truncation = libk::fmt::format_to_n(truncated, "{}", 12345);
    return truncation.error == libk::fmt::errc::output_truncated
        && truncation.produced == 5
        && truncated[3] == '\0';
}

bool test_atomic_scalar_and_compare_exchange_contract(
    const TestContext&) noexcept {
    libk::Atomic<AtomicPhase> phase{AtomicPhase::Empty};
    phase.store<libk::MemoryOrder::Release>(AtomicPhase::Ready);
    if (phase.load<libk::MemoryOrder::Acquire>() != AtomicPhase::Ready) {
        return false;
    }

    AtomicPhase expected = AtomicPhase::Empty;
    if (phase.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, AtomicPhase::Online)
        || expected != AtomicPhase::Ready) {
        return false;
    }

    if (!phase.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, AtomicPhase::Online)) {
        return false;
    }

    return phase.exchange<libk::MemoryOrder::AcqRel>(AtomicPhase::Empty)
            == AtomicPhase::Online
        && phase.load<libk::MemoryOrder::Relaxed>() == AtomicPhase::Empty;
}

/*luna change: test the shared saturating atomic increment contract, reason: normal epoch progress and max stability are the only focused invariants needed here*/
bool test_atomic_inc_sat_contract(const TestContext&) noexcept {
    libk::Atomic<uint64_t> value{};
    libk::atomic_inc_sat(value);
    if (value.load<libk::MemoryOrder::Relaxed>() != 1) {
        return false;
    }
    value.store<libk::MemoryOrder::Relaxed>(UINT64_MAX);
    libk::atomic_inc_sat(value);
    return value.load<libk::MemoryOrder::Relaxed>() == UINT64_MAX;
}

bool test_atomic_ref_uses_borrowed_storage(const TestContext&) noexcept {
    uint64_t storage{7};
    libk::AtomicRef value{storage};
    value.store<libk::MemoryOrder::Release>(11);
    const uint64_t previous =
        value.fetch_add<libk::MemoryOrder::Relaxed>(5);
    uint64_t expected = 16;
    const bool exchanged = value.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, 23);
    return previous == 11 && exchanged && storage == 23
        && value.load<libk::MemoryOrder::Acquire>() == 23;
}

bool test_intrusive_tree_order_and_removal(const TestContext&) noexcept {
    TreeNode nodes[] = {
        {7, 0}, {2, 0}, {9, 0}, {1, 0}, {5, 0}, {8, 0},
        {10, 0}, {3, 0}, {6, 0}, {4, 0}, {5, 1},
    };
    TestTree initial{};
    for (TreeNode& node : nodes) {
        initial.insert(node);
    }
    TestTree tree{std::move(initial)};
    if (!initial.empty() || tree.size() != 11 || tree.minimum() != &nodes[3]) {
        return false;
    }

    initial = std::move(tree);
    if (!tree.empty()) return false;
    tree = std::move(initial);
    tree.erase(nodes[0]);
    tree.erase(nodes[4]);
    tree.erase(nodes[3]);
    if (tree.size() != 8 || tree.minimum() != &nodes[1]) {
        return false;
    }

    int previous = -1;
    while (!tree.empty()) {
        TreeNode* const node = tree.minimum();
        if (node == nullptr || node->key < previous) {
            return false;
        }
        previous = node->key;
        tree.erase(*node);
    }
    return tree.size() == 0;
}

bool test_scope_exit_runs_once_and_can_release(const TestContext&) noexcept {
    int calls{};
    {
        auto guard = libk::on_scope_exit([&calls]() noexcept { ++calls; });
        auto moved = std::move(guard);
        static_cast<void>(moved);
    }
    if (calls != 1) {
        return false;
    }
    {
        auto guard = libk::on_scope_exit([&calls]() noexcept { ++calls; });
        if (!guard.release() || guard.release()) {
            return false;
        }
    }
    return calls == 1;
}

} // namespace

void register_libk_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "libk",
        "checked arithmetic reports overflow without asserting",
        test_checked_arithmetic_reports_overflow);
    (void)registry.add(
        "libk",
        "array and string views preserve empty and bounded contracts",
        test_array_and_string_views_preserve_empty_contracts);
    (void)registry.add(
        "libk",
        "byte reader failures leave cursor and outputs unchanged",
        test_byte_reader_failure_is_transactional);
    (void)registry.add(
        "libk",
        "inplace vector handles aliasing, moves, and zero capacity",
        test_inplace_vector_handles_aliasing_and_zero_capacity);
    (void)registry.add(
        "libk",
        "inplace ring erase preserves order across wrapped storage",
        test_inplace_ring_erase_preserves_logical_order);
    (void)registry.add(
        "libk",
        "alignment overflow is reported",
        test_alignment);
    (void)registry.add(
        "libk",
        "fmt remains bounded and independent of copy elision",
        test_fmt_is_bounded_and_copy_elision_independent);
    (void)registry.add(
        "libk",
        "atomic scalar operations preserve compare-exchange contract",
        test_atomic_scalar_and_compare_exchange_contract);
    /*luna change: register the saturating atomic increment focused test, reason: keep the generic no-wrap contract executable without concurrency pressure*/
    (void)registry.add(
        "libk",
        "atomic increment saturates at the unsigned maximum",
        test_atomic_inc_sat_contract);
    (void)registry.add(
        "libk",
        "atomic ref synchronizes borrowed scalar storage",
        test_atomic_ref_uses_borrowed_storage);
    (void)registry.add(
        "libk",
        "intrusive AVL tree preserves ordered unique membership",
        test_intrusive_tree_order_and_removal);
    (void)registry.add(
        "libk",
        "scope exit runs rollback once and supports explicit commit",
        test_scope_exit_runs_once_and_can_release);
}
