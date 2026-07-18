#include <test/test.hpp>

#include <libk/align.hpp>
#include <libk/array.hpp>
#include <libk/byte_reader.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/fmt.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/limits.hpp>
#include <libk/optional.hpp>
#include <libk/scope_guard.hpp>
#include <libk/sync/atomic.hpp>
#include <libk/utility.hpp>
#include <libk/variant.hpp>

namespace {

enum class ExpectedError {
    rejected,
    recovered,
};

using IntResult = libk::Expected<int, ExpectedError>;
using IntOptional = libk::optional<int>;

struct VariantA {
    explicit VariantA(int input) noexcept : value(input) {}

    int value;
};

[[nodiscard]] constexpr auto operator==(
    const VariantA& lhs,
    const VariantA& rhs) noexcept -> bool {
    return lhs.value == rhs.value;
}

struct VariantB {
    VariantB(int left_input, int right_input) noexcept
        : left(left_input), right(right_input) {}

    int left;
    int right;
};

[[nodiscard]] constexpr auto operator==(
    const VariantB& lhs,
    const VariantB& rhs) noexcept -> bool {
    return lhs.left == rhs.left && lhs.right == rhs.right;
}

struct MoveOnly {
    explicit MoveOnly(int input) noexcept : value(input) {}

    MoveOnly(const MoveOnly&) = delete;
    auto operator=(const MoveOnly&) -> MoveOnly& = delete;

    MoveOnly(MoveOnly&& other) noexcept : value(other.value) {
        other.value = -1;
    }

    auto operator=(MoveOnly&& other) noexcept -> MoveOnly& {
        value = other.value;
        other.value = -1;
        return *this;
    }

    int value;
};

using TestVariant = libk::variant<VariantA, VariantB>;
using SingleVariant = libk::variant<VariantA>;

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
static_assert(!libk::is_copy_constructible_v<libk::Atomic<uint32_t>>);
static_assert(!libk::is_move_constructible_v<libk::Atomic<uint32_t>>);
static_assert(libk::is_trivially_destructible_v<libk::Atomic<uint32_t>>);

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

static_assert(libk::variant_size_v<TestVariant> == 2);
static_assert(libk::variant_size_v<SingleVariant> == 1);
static_assert(libk::is_same_v<
    libk::variant_alternative_t<0, TestVariant>,
    VariantA>);
static_assert(libk::is_same_v<
    libk::variant_alternative_t<1, TestVariant>,
    VariantB>);

[[nodiscard]] auto same_text(libk::StrView actual, const char* expected) noexcept
    -> bool {
    return actual == libk::StrView::from_cstr(expected);
}

bool test_expected_value_semantics_and_chaining(const TestContext&) noexcept {
    const auto source = IntResult::success(20);
    auto copied = source;
    auto assigned = IntResult::failure(ExpectedError::rejected);
    assigned = source;

    auto chained = libk::move(assigned)
        .transform([](int value) { return value + 1; })
        .and_then([](int value) {
            return libk::Expected<long, ExpectedError>::success(value * 2L);
        });

    auto recovered = IntResult::failure(ExpectedError::rejected)
        .or_else([](ExpectedError error) {
            return error == ExpectedError::rejected
                ? IntResult::success(7)
                : IntResult::failure(ExpectedError::recovered);
        });

    auto moved = libk::Expected<MoveOnly, ExpectedError>::success(9);
    auto transformed = libk::move(moved).transform(
        [](MoveOnly&& value) { return value.value; });

    return copied.has_value() && copied.value() == 20
        && chained.has_value() && chained.value() == 42
        && recovered.has_value() && recovered.value() == 7
        && transformed.has_value() && transformed.value() == 9;
}

static_assert(requires(IntOptional value) {
    value.and_then([](int&) { return libk::optional<long>{2L}; });
});

static_assert(!requires(IntOptional value) {
    value.and_then([](int&) { return 2; });
});

static_assert(requires(IntOptional value) {
    value.transform([](int& input) -> int& { return input; });
});

static_assert(!requires(IntOptional value) {
    value.transform([](int&) {});
});

static_assert(requires(IntOptional value) {
    value.or_else([] { return IntOptional{1}; });
});

static_assert(!requires(IntOptional value) {
    value.or_else([] { return libk::optional<long>{1L}; });
});

static_assert(requires(IntOptional value) {
    value.value_or(1);
});

static_assert(requires(const IntOptional value) {
    value.value_or(1);
});

static_assert(requires(libk::optional<MoveOnly> value) {
    libk::move(value).value_or(MoveOnly{1});
});

bool test_optional_monadic_operations(const TestContext&) noexcept {
    bool empty_transform_called = false;
    bool empty_and_then_called = false;
    bool kept_or_else_called = false;
    bool empty_or_else_called = false;

    IntOptional chain_source{20};
    auto chained = chain_source.transform([](int& value) { return value + 1; })
        .and_then([](int&& value) {
            return libk::optional<long>{value * 2L};
        });

    IntOptional empty_source{libk::nullopt};
    auto empty_transform = empty_source.transform([&empty_transform_called](int&) {
        empty_transform_called = true;
        return 17;
    });

    auto empty_chain = empty_source.and_then([&empty_and_then_called](int&) {
        empty_and_then_called = true;
        return libk::optional<long>{99L};
    });

    auto kept = IntOptional{7}.or_else([&kept_or_else_called] {
        kept_or_else_called = true;
        return IntOptional{11};
    });

    auto recovered = IntOptional{libk::nullopt}.or_else([&empty_or_else_called] {
        empty_or_else_called = true;
        return IntOptional{11};
    });

    auto nested = IntOptional{3}.transform([](int value) {
        return IntOptional{value + 4};
    });

    int external = 31;
    IntOptional reference_source{1};
    auto from_reference = reference_source.transform([&external](int&) -> int& {
        return external;
    });
    external = 42;

    auto moved_payload = libk::optional<MoveOnly>{libk::optional_in_place, 9};
    auto moved_value = libk::move(moved_payload).transform(
        [](MoveOnly&& value) { return value.value; });

    auto moved_or_else_source = libk::optional<MoveOnly>{libk::optional_in_place, 13};
    auto moved_or_else = libk::move(moved_or_else_source).or_else([] {
        return libk::optional<MoveOnly>{libk::optional_in_place, 99};
    });

    const IntOptional const_lvalue{5};
    auto from_const_lvalue = const_lvalue.transform([](const int& value) {
        return value + 1;
    });

    auto from_rvalue = IntOptional{6}.and_then([](int&& value) {
        return IntOptional{value + 1};
    });

    const IntOptional const_rvalue{8};
    auto from_const_rvalue = libk::move(const_rvalue).transform([](const int&& value) {
        return value + 1;
    });

    return chained.has_value() && chained.value() == 42
        && !empty_transform.has_value()
        && !empty_transform_called
        && !empty_chain.has_value()
        && !empty_and_then_called
        && kept.has_value() && kept.value() == 7
        && !kept_or_else_called
        && recovered.has_value() && recovered.value() == 11
        && empty_or_else_called
        && nested.has_value() && nested.value().has_value()
        && nested.value().value() == 7
        && from_reference.has_value() && from_reference.value() == 31
        && moved_value.has_value() && moved_value.value() == 9
        && moved_or_else.has_value() && moved_or_else.value().value == 13
        && from_const_lvalue.has_value() && from_const_lvalue.value() == 6
        && from_rvalue.has_value() && from_rvalue.value() == 7
        && from_const_rvalue.has_value() && from_const_rvalue.value() == 9;
}

bool test_optional_value_or(const TestContext&) noexcept {
    IntOptional kept{10};
    IntOptional empty{libk::nullopt};
    const IntOptional const_kept{12};
    const IntOptional const_empty{libk::nullopt};

    const auto moved_kept =
        libk::optional<MoveOnly>{libk::optional_in_place, 21}.value_or(
            MoveOnly{30});
    const auto moved_empty =
        libk::optional<MoveOnly>{libk::nullopt}.value_or(MoveOnly{30});

    const IntOptional const_rvalue{8};

    return kept.value_or(3) == 10
        && empty.value_or(3) == 3
        && const_kept.value_or(4) == 12
        && const_empty.value_or(4) == 4
        && IntOptional{5}.value_or(6) == 5
        && IntOptional{libk::nullopt}.value_or(6) == 6
        && libk::move(const_rvalue).value_or(99) == 8
        && moved_kept.value == 21
        && moved_empty.value == 30;
}

bool test_checked_arithmetic_reports_overflow(const TestContext&) noexcept {
    constexpr size_t max = libk::numeric_limits<size_t>::max();

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
    libk::Array<int, 0> empty_array{};
    libk::Array<int, 3> values{{4, 5, 6}};
    const libk::StrView empty{};
    const libk::StrView text{"kernel"};

    return empty_array.empty()
        && empty_array.size() == 0
        && empty_array.data() == nullptr
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
    libk::InplaceVector<MoveOnly, 2> moved{libk::move(source)};
    return source.empty()
        && moved.size() == 2
        && moved[0].value == 7
        && moved[1].value == 9;
}

bool test_align_and_single_variant_contracts(const TestContext&) noexcept {
    constexpr size_t max = libk::numeric_limits<size_t>::max();
    const auto overflow = libk::checked_align_up(max, size_t{8});
    SingleVariant value{VariantA{17}};

    return libk::align_up<size_t>(0x1001, 0x1000) == 0x2000
        && !overflow.has_value()
        && value.index() == 0
        && libk::get<0>(value).value == 17
        && libk::get<VariantA>(value).value == 17;
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

bool test_variant_tracks_active_alternative(const TestContext&) noexcept {
    TestVariant value{VariantA{7}};
    if (!libk::holds_alternative<VariantA>(value)
        || libk::get<VariantA>(value).value != 7
        || libk::get_if<VariantB>(&value) != nullptr) {
        return false;
    }

    auto& placed = value.emplace<VariantB>(2, 5);
    return placed.left == 2
        && placed.right == 5
        && value.index() == 1
        && libk::holds_alternative<VariantB>(value)
        && libk::get_if<VariantA>(&value) == nullptr
        && libk::get<VariantB>(value).left == 2
        && libk::get<VariantB>(value).right == 5;
}

bool test_variant_visit_and_equality(const TestContext&) noexcept {
    TestVariant left{VariantA{9}};
    TestVariant same{VariantA{9}};
    TestVariant different{VariantB{4, 5}};

    const int left_value = libk::visit(
        [](const auto& payload) -> int {
            if constexpr (requires { payload.right; }) {
                return payload.left + payload.right;
            } else {
                return payload.value;
            }
        },
        left);

    const int different_value = libk::visit(
        [](const auto& payload) -> int {
            if constexpr (requires { payload.right; }) {
                return payload.left + payload.right;
            } else {
                return payload.value;
            }
        },
        different);

    return left_value == 9
        && different_value == 9
        && left == same
        && !(left == different);
}

bool test_variant_move_preserves_move_only_payload(const TestContext&) noexcept {
    using MoveVariant = libk::variant<MoveOnly, VariantA>;

    MoveVariant source{libk::in_place_type<MoveOnly>, 11};
    MoveVariant moved{libk::move(source)};
    if (!libk::holds_alternative<MoveOnly>(moved)
        || libk::get<MoveOnly>(moved).value != 11) {
        return false;
    }

    moved = VariantA{3};
    return libk::holds_alternative<VariantA>(moved)
        && libk::get<VariantA>(moved).value == 3;
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

bool test_intrusive_tree_order_and_removal(const TestContext&) noexcept {
    TreeNode nodes[] = {
        {7, 0}, {2, 0}, {9, 0}, {1, 0}, {5, 0}, {8, 0},
        {10, 0}, {3, 0}, {6, 0}, {4, 0}, {5, 1},
    };
    TestTree tree{};
    for (TreeNode& node : nodes) {
        tree.insert(node);
    }
    if (tree.size() != 11 || tree.minimum() != &nodes[3]) {
        return false;
    }

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
        auto moved = libk::move(guard);
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
        "Expected preserves value semantics and chain propagation",
        test_expected_value_semantics_and_chaining);
    (void)registry.add(
        "libk",
        "optional supports monadic chaining without hidden fallback calls",
        test_optional_monadic_operations);
    (void)registry.add(
        "libk",
        "optional value_or selects contained or fallback value",
        test_optional_value_or);
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
        "alignment and single-alternative variant contracts hold",
        test_align_and_single_variant_contracts);
    (void)registry.add(
        "libk",
        "fmt remains bounded and independent of copy elision",
        test_fmt_is_bounded_and_copy_elision_independent);
    (void)registry.add(
        "libk",
        "variant tracks its active alternative",
        test_variant_tracks_active_alternative);
    (void)registry.add(
        "libk",
        "variant visit and equality follow the active payload",
        test_variant_visit_and_equality);
    (void)registry.add(
        "libk",
        "variant moves a move-only payload",
        test_variant_move_preserves_move_only_payload);
    (void)registry.add(
        "libk",
        "atomic scalar operations preserve compare-exchange contract",
        test_atomic_scalar_and_compare_exchange_contract);
    (void)registry.add(
        "libk",
        "intrusive AVL tree preserves ordered unique membership",
        test_intrusive_tree_order_and_removal);
    (void)registry.add(
        "libk",
        "scope exit runs rollback once and supports explicit commit",
        test_scope_exit_runs_once_and_can_release);
}
