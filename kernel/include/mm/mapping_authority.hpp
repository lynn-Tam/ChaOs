#pragma once

#include <cap/grant.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <mm/address_region.hpp>
#include <mm/memory_object.hpp>
#include <mm/physical_alias.hpp>
#include <object/object_ref.hpp>

namespace kernel::mm {

namespace cap = kernel::cap;
namespace object = kernel::object;

class VSpace;

enum class AuthoritySource : u8 {
    Kernel,
    Capability,
};

class MappedPage final : private libk::noncopyable_nonmovable {
public:
    MappedPage(
        VirtAddr address,
        PageLease&& source,
        AliasLease&& alias) noexcept
        : address_(address),
          page_(source.page().page),
          access_(source.page().access),
          type_(source.page().type),
          source_(libk::move(source)),
          alias_(libk::move(alias)) {}

    ~MappedPage() noexcept;

    [[nodiscard]] auto address() const noexcept -> VirtAddr {
        return address_;
    }
    [[nodiscard]] auto page() const noexcept -> Page { return page_; }
    [[nodiscard]] auto access() const noexcept -> AccessMask { return access_; }
    [[nodiscard]] auto type() const noexcept -> MemoryType { return type_; }

private:
    friend class VSpace;
    friend class MappingAuthority;

    VirtAddr address_{};
    Page page_{};
    AccessMask access_{};
    MemoryType type_{MemoryType::Normal};
    PageLease source_{};
    AliasLease alias_{};
    libk::IntrusiveTreeHook tree_hook_{};
    MappedPage* pending_next_{};
};

struct MappedPageCompare final {
    [[nodiscard]] constexpr auto operator()(
        const MappedPage& lhs,
        const MappedPage& rhs) const noexcept -> bool {
        return lhs.address() < rhs.address();
    }
    [[nodiscard]] constexpr auto operator()(
        VirtAddr lhs,
        const MappedPage& rhs) const noexcept -> bool {
        return lhs < rhs.address();
    }
    [[nodiscard]] constexpr auto operator()(
        const MappedPage& lhs,
        VirtAddr rhs) const noexcept -> bool {
        return lhs.address() < rhs;
    }
};

class MappingAuthority final : private libk::noncopyable_nonmovable {
    using MappingList = libk::IntrusiveList<
        Mapping, &Mapping::authority_hook_>;
    using PageTree = libk::IntrusiveTree<
        MappedPage, &MappedPage::tree_hook_, MappedPageCompare>;

public:
    MappingAuthority(
        VSpace& owner,
        object::ObjectRef&& memory,
        MemoryObject& object,
        cap::MemoryAuthority frozen,
        AuthoritySource source) noexcept;
    ~MappingAuthority() noexcept;

    [[nodiscard]] auto source() const noexcept -> AuthoritySource {
        return source_;
    }
    [[nodiscard]] auto memory() noexcept -> MemoryObject& { return *memory_; }
    [[nodiscard]] auto frozen() const noexcept -> cap::MemoryAuthority {
        return frozen_;
    }

private:
    friend class VSpace;

    [[nodiscard]] auto attach_memory() noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto attach_grant(
        const cap::GrantLease& grant) noexcept
        -> libk::Expected<void, cap::GrantError>;
    [[nodiscard]] auto detach_relations() noexcept -> bool;
    [[nodiscard]] auto relations_released() const noexcept -> bool;

    static void invalidate_memory(
        void* context,
        MemoryWork&& work,
        MemoryInvalidation reason) noexcept;
    static void memory_released(void* context) noexcept;
    static void invalidate_grant(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void grant_released(void* context) noexcept;

    static const MemoryAttachmentOps memory_ops_;
    static const cap::GrantAttachmentOps grant_ops_;

    VSpace* owner_{};
    object::ObjectRef memory_ref_{};
    MemoryObject* memory_{};
    cap::MemoryAuthority frozen_{};
    AuthoritySource source_{AuthoritySource::Kernel};
    MappingList mappings_{};
    PageTree pages_{};
    libk::IntrusiveListHook invalidation_hook_{};
    MemoryAttachment memory_attachment_;
    libk::ManualLifetime<cap::GrantAttachment> grant_attachment_{};
    libk::ManualLifetime<MemoryWork> memory_work_{};
    libk::ManualLifetime<cap::GrantWork> grant_work_{};
    bool invalidation_requested_{};
    bool relations_detached_{};
    libk::Atomic<bool> release_notified_{};
    MappingAuthority* pending_next_{};
};

} // namespace kernel::mm
