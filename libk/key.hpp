#pragma once

#include <cstddef>
#include <cstdint>

namespace libk {

// Domain tags prevent identities from different registries being mixed.
// A zero slot or generation is reserved for an absent identity.
template<typename Domain>
struct key final {
    std::size_t slot{};
    std::uint64_t generation{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return slot != 0 && generation != 0;
    }
    friend constexpr auto operator==(key, key) noexcept -> bool = default;
};

} // namespace libk
