#pragma once

#include <base/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/noncopyable.hpp>
#include <sync.hpp>
#include <array>

namespace sched {

class Sc;

class Domain final : private libk::noncopyable_nonmovable {
public:
    static constexpr u32 share_scale = 1'000'000;

    enum class Error : u8 {
        NotAdmitted,
        InvalidCpu,
        Quota,
        Busy,
    };

    using Result = std::expected<void, Error>;

    Domain(usize cpu_count, u32 limit, u32 reserved) noexcept;
    ~Domain() noexcept;

    [[nodiscard]] auto admit(
        Sc& context,
        CpuId home_cpu) noexcept -> Result;
    [[nodiscard]] auto unadmit(Sc& context) noexcept -> Result;

    [[nodiscard]] auto allows(CpuId cpu) const noexcept -> bool;

private:
    [[nodiscard]] static auto share_of(const Sc& context) noexcept
        -> u32;

    mutable sync::Spin
        lock_{};
    usize count_{};
    u32 capacity_{};
    std::array<u32, MaxCpus> admitted_{};
};

} // namespace sched
