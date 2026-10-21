#include <posix.hpp>
#include <unistd.h>
#include <array>

alignas(4096) static std::array<uint8_t, 8193> data = [] {
    std::array<uint8_t, 8193> value{};
    for (size_t i = 0; i < value.size(); ++i)
        value[i] = i % 251 + 1;
    return value;
}();
alignas(4096) static uint8_t zero[4097];
int main() {
    if (const auto status = px_init(); status != STATUS_OK) return status;
    volatile auto* initialized = data.data();
    volatile auto* bss = zero;
    for (size_t i = 0; i < data.size(); ++i)
        if (initialized[i] != i % 251 + 1)
            return STATUS_INTERNAL;
    for (size_t i = 0; i < sizeof(zero); ++i)
        if (bss[i])
            return STATUS_INTERNAL;
    for (size_t i = 0; i < data.size(); ++i)
        initialized[i] = 0;
    for (size_t i = 0; i < sizeof(zero); ++i)
        bss[i] = 1;
    constexpr std::string_view message{"ELF data, COW and BSS ok\n"};
    if (write(STDOUT_FILENO, message.data(), message.size()) != int64_t(message.size())) return STATUS_BACKING_FAILED;
    return 0;
}
