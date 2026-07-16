#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/checked_arithmetic.hpp>
#include <mm/boot_map.hpp>

namespace kernel::mm {

enum class DirectMapError : u8 {
    InvalidLayout,
    OutsideWindow,
    NotMapped,
    Overflow,
};

struct DirectMapLayout final {
    PhysAddr physical_base{};
    VirtAddr virtual_base{};
    usize window_size{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return virtual_base.valid() && window_size != 0;
    }
};

// Checked affine view of the RAM ranges materialized in KernelVSpace. The
// normalized ranges are the semantic truth; a PTE/direct pointer is only a
// projection of this policy. MMIO is never admitted through this boundary.
class DirectMap final : private libk::noncopyable_nonmovable {
    class ConstructionKey final {
        friend class DirectMap;
        constexpr ConstructionKey() noexcept = default;
    };

public:
    using InitResult = libk::Expected<void, DirectMapError>;

    [[nodiscard]] static auto initialize_in(
        libk::ManualLifetime<DirectMap>& storage,
        const RegionList& memory,
        DirectMapLayout layout) noexcept -> InitResult;

    [[nodiscard]] explicit operator bool() const noexcept {
        return window_size_ != 0 && !ranges_.empty();
    }

    [[nodiscard]] auto range_count() const noexcept -> usize {
        return ranges_.size();
    }

    [[nodiscard]] auto range(usize index) const noexcept -> PageRange;

    [[nodiscard]] auto map(PhysAddr address, usize size) const noexcept
        -> libk::Expected<VirtAddr, DirectMapError>;

    [[nodiscard]] auto unmap(VirtAddr address, usize size) const noexcept
        -> libk::Expected<PhysAddr, DirectMapError>;

    template<typename T>
    [[nodiscard]] auto ptr(
        PhysAddr address,
        usize count = 1) const noexcept
        -> libk::Expected<T*, DirectMapError> {
        const auto size = libk::checked_multiply(sizeof(T), count);
        if (!size || *size == 0) {
            return libk::unexpected(DirectMapError::Overflow);
        }
        const auto mapped = map(address, *size);
        if (!mapped) {
            return libk::unexpected(mapped.error());
        }
        return libk::expected(
            reinterpret_cast<T*>(mapped.value().raw()));
    }

    [[nodiscard]] auto physical_base() const noexcept -> PhysAddr {
        return physical_base_;
    }

    [[nodiscard]] auto virtual_base() const noexcept -> VirtAddr {
        return virtual_base_;
    }

    [[nodiscard]] auto window_size() const noexcept -> usize {
        return window_size_;
    }

    DirectMap(
        [[maybe_unused]] ConstructionKey key,
        PhysAddr physical_base,
        VirtAddr virtual_base,
        usize window_size) noexcept
        : physical_base_(physical_base),
          virtual_base_(virtual_base),
          window_size_(window_size) {}

private:
    [[nodiscard]] auto initialize(const RegionList& memory) noexcept
        -> InitResult;
    [[nodiscard]] auto contains(PageRange range) const noexcept -> bool;

    libk::InplaceVector<PageRange, max_regions> ranges_{};
    PhysAddr physical_base_{};
    VirtAddr virtual_base_{};
    usize window_size_{};
};

} // namespace kernel::mm
