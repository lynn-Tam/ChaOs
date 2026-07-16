#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/optional.hpp>
#include <libk/noncopyable.hpp>
#include <libk/array.hpp>
#include <libk/assert.hpp>
#include <mm/addr.hpp>

namespace arch::riscv64 {

inline constexpr size_t ptes_per_pg = 512;

class Pte;

class PtePerm {
public:
    [[nodiscard]] static constexpr auto supervisor_rx() noexcept
        -> PtePerm {
        return PtePerm{read_bit_ | execute_bit_};
    }

    [[nodiscard]] static constexpr auto supervisor_ro() noexcept
        -> PtePerm {
        return PtePerm{read_bit_};
    }

    [[nodiscard]] static constexpr auto supervisor_rw() noexcept
        -> PtePerm {
        return PtePerm{read_bit_ | write_bit_};
    }

    [[nodiscard]] static constexpr auto user_rx() noexcept
        -> PtePerm {
        return PtePerm{read_bit_ | execute_bit_ | user_bit_};
    }

    [[nodiscard]] static constexpr auto user_ro() noexcept
        -> PtePerm {
        return PtePerm{read_bit_ | user_bit_};
    }

    [[nodiscard]] static constexpr auto user_x() noexcept
        -> PtePerm {
        return PtePerm{execute_bit_ | user_bit_};
    }

    [[nodiscard]] static constexpr auto user_rw() noexcept
        -> PtePerm {
        return PtePerm{read_bit_ | write_bit_ | user_bit_};
    }

    [[nodiscard]] friend constexpr auto operator==(
        PtePerm lhs, PtePerm rhs) noexcept -> bool {
        return lhs.bits_ == rhs.bits_;
    }

private:
    friend class Pte;

    constexpr explicit PtePerm(uint64_t bits) noexcept
        : bits_(bits) {}

    [[nodiscard]] constexpr auto pte_bits() const noexcept -> uint64_t {
        return bits_;
    }

    static constexpr uint64_t read_bit_ = uint64_t{1} << 1;
    static constexpr uint64_t write_bit_ = uint64_t{1} << 2;
    static constexpr uint64_t execute_bit_ = uint64_t{1} << 3;
    static constexpr uint64_t user_bit_ = uint64_t{1} << 4;

    uint64_t bits_;
};

static_assert(sizeof(PtePerm) == sizeof(uint64_t));

class Pte{
public:
    constexpr Pte() noexcept = default;

    // Raw construction is the real hardware decode boundary.  Classification
    // below deliberately answers what kind of walk entry the R/W/X bits name;
    // target decoders additionally reject encodings this implementation does
    // not support.
    [[nodiscard]] static constexpr auto from_raw(uint64_t bits) noexcept
        -> Pte {
        return Pte{bits};
    }

    [[nodiscard]] static constexpr auto non_leaf(kernel::mm::Page next_table) noexcept
        -> libk::optional<Pte> {
            const auto encoded = encode_ppn(next_table);
            if(!encoded){
                return libk::nullopt;
            }
            return Pte{*encoded | valid_bit};
    }
    [[nodiscard]] static constexpr auto leaf_4k(kernel::mm::Page page, PtePerm permissions) noexcept
        -> libk::optional<Pte>{
            const auto encoded = encode_ppn(page);
            if(!encoded){
                return libk::nullopt;
            }
            return Pte{*encoded | permissions.pte_bits() | valid_bit | accessed_bit_ |dirty_bit_};
    }

    [[nodiscard]] constexpr auto raw() const noexcept -> uint64_t {
        return bits_;
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return (bits_ & valid_bit) != 0;
    }

    [[nodiscard]] constexpr auto is_leaf() const noexcept -> bool{
        if (!valid()) {
            return false;
        }

        const uint64_t access = bits_ & access_mask;
        const bool write_without_read =
            (access & write_bit_) != 0
            && (access & read_bit_) == 0;
        return access != 0 && !write_without_read;
    }

    [[nodiscard]] constexpr auto is_non_leaf() const noexcept -> bool{
        return valid() && (bits_ & access_mask) == 0;
    }

    [[nodiscard]] constexpr auto next_table_page() const noexcept
        -> libk::optional<kernel::mm::Page> {
            if (!is_supported_non_leaf()) {
                return libk::nullopt;
            }
            const uint64_t ppn = (bits_ & ppn_mask_) >> ppn_shift_;

            return kernel::mm::Page{kernel::mm::Pfn{static_cast<uintptr_t>(ppn)}};
        }

    // Decodes the target page of a valid 4 KiB leaf for construction-time
    // invariant checks and representation tests. This does not translate a
    // runtime virtual address.
    [[nodiscard]] constexpr auto leaf_page() const noexcept
        -> libk::optional<kernel::mm::Page> {
        if (!is_supported_leaf()) {
            return libk::nullopt;
        }
        const uint64_t ppn =
            (bits_ & ppn_mask_) >> ppn_shift_;
        return kernel::mm::Page{
            kernel::mm::Pfn{static_cast<uintptr_t>(ppn)}};
    }

    // Compares only the leaf permission bits used by PtePerm. This is
    // an inspection helper for pre-publication policy checks and PTE tests.
    [[nodiscard]] constexpr auto has_permissions(
        PtePerm expected) const noexcept -> bool {
        return is_supported_leaf()
            && (bits_ & perm_mask_)
                == expected.pte_bits();
    }

    [[nodiscard]] constexpr auto permissions() const noexcept
        -> libk::optional<PtePerm> {
        return is_supported_leaf()
            ? libk::optional<PtePerm>{PtePerm{bits_ & perm_mask_}}
            : libk::nullopt;
    }
private:
    constexpr explicit Pte(uint64_t bits) noexcept : bits_(bits) {}

    [[nodiscard]] static constexpr auto encode_ppn(kernel::mm::Page page) noexcept
        -> libk::optional<uint64_t>{
            const uint64_t ppn = page.frame().raw();
            if(!page.valid() || ppn > max_ppn_){
                return libk::nullopt;
            }
            return ppn<<ppn_shift_;
    }
    static constexpr uint64_t valid_bit = uint64_t{1} << 0;
    static constexpr uint64_t read_bit_ = uint64_t{1} << 1;
    static constexpr uint64_t write_bit_ = uint64_t{1} << 2;
    static constexpr uint64_t execute_bit_ = uint64_t{1} << 3;
    static constexpr uint64_t user_bit_ = uint64_t{1} << 4;
    static constexpr uint64_t global_bit_ = uint64_t{1} << 5;
    static constexpr uint64_t accessed_bit_ = uint64_t{1} << 6;
    static constexpr uint64_t dirty_bit_ = uint64_t{1} << 7;
    static constexpr uint64_t rsw_mask_ = uint64_t{3} << 8;

    static constexpr uint64_t access_mask = read_bit_ | write_bit_ | execute_bit_;
    static constexpr uint64_t perm_mask_ = access_mask | user_bit_;
    static constexpr size_t ppn_bits_ = 44;
    static constexpr size_t ppn_shift_ = 10;
    static constexpr uint64_t max_ppn_ = (uint64_t{1} << ppn_bits_) -1;
    static constexpr uint64_t ppn_mask_ = max_ppn_ << ppn_shift_;

    static constexpr uint64_t supported_branch_mask_ =
        ppn_mask_ | valid_bit | global_bit_ | rsw_mask_;
    static constexpr uint64_t supported_leaf_mask_ =
        ppn_mask_ | valid_bit | access_mask | user_bit_ | global_bit_
        | accessed_bit_ | dirty_bit_ | rsw_mask_;

    [[nodiscard]] constexpr auto is_supported_non_leaf() const noexcept
        -> bool {
        return is_non_leaf()
            && (bits_ & ~supported_branch_mask_) == 0;
    }

    [[nodiscard]] constexpr auto is_supported_leaf() const noexcept
        -> bool {
        return is_leaf()
            && (bits_ & ~supported_leaf_mask_) == 0;
    }

    uint64_t bits_{};
};

static_assert(sizeof(Pte) == sizeof(uint64_t));
static_assert(ptes_per_pg*sizeof(Pte) == kernel::mm::page_size);


class TableRef;
class TableView;
class alignas(kernel::mm::page_size) TablePage final: private libk::noncopyable_nonmovable{
public:
    TablePage() = delete;
    /*禁止拷贝和移动语义*/

private:
    friend class TableRef;
    friend class TableView;

    class ConstructionKey{
        friend class TableRef;
        constexpr ConstructionKey() noexcept = default;
    };
    explicit TablePage([[maybe_unused]] ConstructionKey key) noexcept : entries_ {} {}
    ~TablePage() noexcept = default;

    /* 8 byte * 512 = 4K 页 */
    libk::Array<Pte, ptes_per_pg> entries_;
};
static_assert(sizeof(TablePage) == kernel::mm::page_size);
static_assert(alignof(TablePage) == kernel::mm::page_size);

class Sv39VPage {
public:
    [[nodiscard]] static constexpr auto from(kernel::mm::VPage page) noexcept
        -> libk::optional<Sv39VPage> {
        const uintptr_t addr = page.base().raw();
        const uintptr_t expected_upper =
            (addr & sign_bit_) != 0 ? upper_addr_mask_ : 0;
        if ((addr & upper_addr_mask_) != expected_upper) {
            return libk::nullopt;
        }
        return Sv39VPage{page};
    }

    [[nodiscard]] constexpr auto generic_page() const noexcept -> kernel::mm::VPage {
        return page_;
    }

    [[nodiscard]] constexpr auto level0_index() const noexcept -> size_t {
        return index_at(0);
    }

    [[nodiscard]] constexpr auto level1_index() const noexcept -> size_t {
        return index_at(1);
    }

    [[nodiscard]] constexpr auto level2_index() const noexcept -> size_t {
        return index_at(2);
    }

private:
    constexpr explicit Sv39VPage(kernel::mm::VPage page) noexcept
        : page_(page) {}

    [[nodiscard]] constexpr auto index_at(size_t level) const noexcept -> size_t {
        const size_t shift = level * index_bits_;
        return static_cast<size_t>((page_.number().raw() >> shift) & index_mask_);
    }

    static constexpr size_t index_bits_ = 9;
    static constexpr uintptr_t index_mask_ = ptes_per_pg - 1;
    static constexpr uintptr_t address_bits_ = 39;
    static constexpr uintptr_t sign_bit_ = uintptr_t{1} << (address_bits_ - 1);
    static constexpr uintptr_t lower_address_mask_ =
        (uintptr_t{1} << address_bits_) - 1;
    static constexpr uintptr_t upper_addr_mask_ = ~lower_address_mask_;

    kernel::mm::VPage page_;
};

static_assert(sizeof(uintptr_t) == sizeof(uint64_t));
static_assert(ptes_per_pg == (size_t{1} << 9));

} // namespace arch::riscv64
