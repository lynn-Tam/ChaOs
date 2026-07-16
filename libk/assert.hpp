#pragma once

namespace libk {

struct AssertInfo final {
    const char* expression{};
    const char* file{};
    const char* function{};
    unsigned line{};
};

[[noreturn]] void assert_fail(const AssertInfo& info) noexcept;

} // namespace libk

#if defined(__GNUC__) || defined(__clang__)
#define LIBK_ASSERT_FUNCTION __PRETTY_FUNCTION__
#else
#define LIBK_ASSERT_FUNCTION __func__
#endif

#define libk_assert(expr) \
    do { \
        if (!(expr)) [[unlikely]] { \
            ::libk::assert_fail({ \
                #expr, __FILE__, LIBK_ASSERT_FUNCTION, \
                static_cast<unsigned>(__LINE__)}); \
        } \
    } while (false)
