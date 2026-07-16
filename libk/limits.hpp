#pragma once
namespace libk {

template<typename T>
struct numeric_limits;

template<>
struct numeric_limits<unsigned char> {
    static constexpr unsigned char max() noexcept { return (unsigned char)~0u; }
    static constexpr unsigned char min() noexcept { return 0; }
};

template<>
struct numeric_limits<unsigned short> {
    static constexpr unsigned short max() noexcept { return (unsigned short)~0u; }
    static constexpr unsigned short min() noexcept { return 0; }
};

template<>
struct numeric_limits<unsigned int> {
    static constexpr unsigned int max() noexcept { return ~0u; }
    static constexpr unsigned int min() noexcept { return 0; }
};

template<>
struct numeric_limits<unsigned long> {
    static constexpr unsigned long max() noexcept { return ~0ul; }
    static constexpr unsigned long min() noexcept { return 0; }
};

template<>
struct numeric_limits<unsigned long long> {
    static constexpr unsigned long long max() noexcept { return ~0ull; }
    static constexpr unsigned long long min() noexcept { return 0; }
};

} // namespace libk