#include <mm/vspace.hpp>
#include <mm/vspace_work.hpp>

#include "vspace_internal.hpp"

#include <mm/virtual_layout.hpp>
#include <cap/cspace.hpp>
#include <cap/resolved.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <libk/limits.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
#include <object/memory_pool.hpp>
#include <object/vspace_pool.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

Mapping::~Mapping() noexcept {
    KASSERT(ipc_relations_.empty());
}

MappedPage::~MappedPage() noexcept {
    KASSERT(!tree_hook_.is_linked());
    KASSERT(!source_ && !alias_);
}

const MemoryAttachmentOps MappingAuthority::memory_ops_{
    .invalidate = &MappingAuthority::invalidate_memory,
    .released = &MappingAuthority::memory_released,
};

const cap::GrantAttachmentOps MappingAuthority::grant_ops_{
    .invalidate = &MappingAuthority::invalidate_grant,
    .released = &MappingAuthority::grant_released,
};

MappingAuthority::MappingAuthority(
    VSpace& owner,
    object::ObjectRef&& memory,
    MemoryObject& object,
    cap::MemoryAuthority frozen,
    AuthoritySource source) noexcept
    : owner_(&owner),
      memory_ref_(libk::move(memory)),
      memory_(&object),
      frozen_(frozen),
      source_(source),
      memory_attachment_(this, memory_ops_) {}

MappingAuthority::~MappingAuthority() noexcept {
    KASSERT(mappings_.empty());
    KASSERT(pages_.empty());
    KASSERT(!invalidation_hook_.is_linked());
    KASSERT(relations_released());
    KASSERT(!memory_work_ && !grant_work_);
    if (grant_attachment_) {
        grant_attachment_.reset();
    }
}

auto MappingAuthority::attach_memory() noexcept
    -> libk::Expected<void, MemoryError> {
    return memory_->attach(memory_attachment_);
}

auto MappingAuthority::attach_grant(
    const cap::GrantLease& grant) noexcept
    -> libk::Expected<void, cap::GrantError> {
    KASSERT(source_ == AuthoritySource::Capability);
    KASSERT(!grant_attachment_);
    auto& attachment = grant_attachment_.emplace(this, grant_ops_);
    auto attached = grant.attach(attachment);
    if (!attached) {
        grant_attachment_.reset();
    }
    return attached;
}

auto MappingAuthority::detach_relations() noexcept -> bool {
    if (!relations_detached_) {
        static_cast<void>(memory_attachment_.detach());
        if (grant_attachment_) {
            static_cast<void>(grant_attachment_->detach());
        }
        relations_detached_ = true;
    }
    if (memory_work_) {
        memory_work_.reset();
    }
    if (grant_work_) {
        grant_work_.reset();
    }
    return relations_released();
}

auto MappingAuthority::relations_released() const noexcept -> bool {
    return relations_detached_
        && !memory_attachment_.attached()
        && !memory_attachment_.busy()
        && (!grant_attachment_
            || (!grant_attachment_->attached()
                && !grant_attachment_->busy()));
}

void MappingAuthority::invalidate_memory(
    void* context,
    MemoryWork&& work,
    MemoryInvalidation) noexcept {
    auto& authority = *static_cast<MappingAuthority*>(context);
    authority.owner_->request_invalidation(authority, libk::move(work));
}

void MappingAuthority::memory_released(void* context) noexcept {
    auto& authority = *static_cast<MappingAuthority*>(context);
    authority.release_notified_.store<libk::MemoryOrder::Release>(true);
}

void MappingAuthority::invalidate_grant(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation) noexcept {
    auto& authority = *static_cast<MappingAuthority*>(context);
    authority.owner_->request_invalidation(authority, libk::move(work));
}

void MappingAuthority::grant_released(void* context) noexcept {
    auto& authority = *static_cast<MappingAuthority*>(context);
    authority.release_notified_.store<libk::MemoryOrder::Release>(true);
}

VSpace::VSpace(
    Pmm& pmm,
    KernelVSpace& kernel,
    VSpaceExecutor& work) noexcept
    : pmm_(&pmm),
      kernel_(&kernel),
      work_(&work),
      regions_(pmm),
      mappings_(pmm),
      reservations_(pmm),
      guards_(pmm),
      authorities_(pmm),
      pages_(pmm),
      ipc_relations_(pmm) {}

VSpace::~VSpace() noexcept {
    KASSERT(state_ == VSpaceState::Quiescent);
    KASSERT(root_region_ == nullptr);
    KASSERT(!root_);
    KASSERT(!claim_.region);
    KASSERT(invalidations_.empty());
    KASSERT(pending_kind_ == PendingKind::None);
    KASSERT(!ticket_ && !retire_batch_ && !cleanup_);
    KASSERT(!work_hook_.is_linked());
    KASSERT(bindings_ == 0);
}

auto VSpace::initialize() noexcept -> libk::Expected<void, VSpaceError> {
    if (state_ != VSpaceState::Building || root_) {
        return libk::unexpected(VSpaceError::InvalidState);
    }
    auto root = kernel_->create_user_root(*pmm_);
    if (!root) {
        return libk::unexpected(VSpaceError::OutOfMemory);
    }
    const VirtRange range{
        VirtAddr{kernel::mm::layout::LowGuardEnd},
        kernel::mm::layout::UserEnd - kernel::mm::layout::LowGuardEnd};
    const RegionPolicy policy{
        .access = AccessMask::of(
            Access::Read, Access::Write, Access::Execute),
        .types = MemoryTypes::of(
            MemoryType::Normal, MemoryType::Uncached, MemoryType::Device),
    };
    auto made = regions_.create(range, nullptr, policy);
    if (!made) {
        return libk::unexpected(node_error(made.error()));
    }
    [[maybe_unused]] auto& installed_root =
        root_.emplace(libk::move(root).value());
    root_region_ = made.value().object;
    root_region_->key_ = RegionKey{made.value().key};
    state_ = VSpaceState::Live;
    return libk::expected();
}

auto VSpace::state() const noexcept -> VSpaceState {
    kernel::sync::IrqLockGuard guard{lock_};
    return state_;
}

auto VSpace::binding_count() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return bindings_;
}

auto VSpace::attach_execution() noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (state_ != VSpaceState::Live
        || bindings_ == libk::numeric_limits<usize>::max()) {
        return false;
    }
    ++bindings_;
    return true;
}

void VSpace::detach_execution() noexcept {
    bool ready{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(bindings_ != 0);
        --bindings_;
        ready = state_ == VSpaceState::Stopping && bindings_ == 0;
    }
    if (ready) {
        schedule_work();
    }
}

auto VSpace::prepare_retire() noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (state_ != VSpaceState::Live || bindings_ != 0) {
        return false;
    }
    state_ = VSpaceState::Stopping;
    KASSERT(root_region_ != nullptr);
    root_region_->state_ = RegionState::Retiring;
    return true;
}

auto VSpace::root_key() const noexcept -> RegionKey {
    kernel::sync::IrqLockGuard guard{lock_};
    return root_region_ != nullptr ? root_region_->key_ : RegionKey{};
}

auto VSpace::translation() noexcept -> TranslationView {
    KASSERT(root_);
    return TranslationView{coherence_, root_->token()};
}

auto VSpace::commit_translation(
    TranslationState::Mutation&& mutation,
    ShootdownPlan&& plan,
    RetireBatch& retire) noexcept
    -> libk::Expected<VmStatus, VSpaceError> {
    KASSERT(pending_kind_ != PendingKind::None);
    auto& ticket = ticket_.emplace(
        kernel::sync::Completion::Notifier::bind<
            &VSpace::translation_ready>(*this));
    const ShootdownStatus status = mutation.commit(
        libk::move(plan), ticket, &retire);
    if (status == ShootdownStatus::Complete) {
        KASSERT(finish_pending());
        return libk::expected(VmStatus::Complete);
    }
    schedule_work();
    return libk::expected(VmStatus::Pending);
}

void VSpace::queue_layout(LayoutNode& node) noexcept {
    KASSERT(node.pending_next_ == nullptr);
    node.pending_next_ = pending_layout_;
    pending_layout_ = &node;
}

void VSpace::queue_page(MappedPage& page) noexcept {
    KASSERT(page.pending_next_ == nullptr);
    page.pending_next_ = pending_pages_;
    pending_pages_ = &page;
}

void VSpace::queue_authority(MappingAuthority& authority) noexcept {
    for (MappingAuthority* current = pending_authorities_;
         current != nullptr;
         current = current->pending_next_) {
        if (current == &authority) {
            return;
        }
    }
    authority.pending_next_ = pending_authorities_;
    pending_authorities_ = &authority;
}

void VSpace::detach_mapping(Mapping& mapping) noexcept {
    KASSERT(mapping.ipc_relations_.empty());
    MappingAuthority& authority = *mapping.authority_;
    if (mapping.authority_hook_.is_linked()) {
        authority.mappings_.erase(mapping);
    }
    mapping.state_ = MappingState::Detached;
    mappings_.destroy(mapping);
    if (authority.mappings_.empty()) {
        queue_authority(authority);
    }
}

void VSpace::destroy_layout(LayoutNode& node) noexcept {
    switch (node.kind_) {
    case LayoutKind::Region:
        regions_.destroy(static_cast<AddressRegion&>(node));
        break;
    case LayoutKind::Mapping:
        detach_mapping(static_cast<Mapping&>(node));
        break;
    case LayoutKind::Reserved:
        reservations_.destroy(static_cast<ReservedLeaf&>(node));
        break;
    case LayoutKind::Guard:
        guards_.destroy(static_cast<Guard&>(node));
        break;
    }
}

void VSpace::release_page(MappedPage& page) noexcept {
    kernel_->aliases().release(page.page_, page.type_);
    pages_.destroy(page);
}

void VSpace::finish_authorities() noexcept {
    MappingAuthority** link = &pending_authorities_;
    while (*link != nullptr) {
        MappingAuthority* const authority = *link;
        KASSERT(authority->mappings_.empty());
        KASSERT(authority->pages_.empty());
        if (authority->invalidation_hook_.is_linked()) {
            invalidations_.erase(*authority);
        }
        authority->invalidation_requested_ = false;
        const bool released = authority->detach_relations();
        if (!released) {
            link = &authority->pending_next_;
            continue;
        }
        *link = authority->pending_next_;
        authority->pending_next_ = nullptr;
        authorities_.destroy(*authority);
    }
}

auto VSpace::finish_pending() noexcept -> bool {
    if (pending_kind_ == PendingKind::None) {
        finish_authorities();
        try_finish_retire();
        return true;
    }
    if (ticket_ && !ticket_->complete()) {
        return false;
    }
    if (retire_batch_) {
        KASSERT(retire_batch_->release());
        retire_batch_.reset();
    }
    if (ticket_) {
        ticket_.reset();
    }
    while (pending_pages_ != nullptr) {
        MappedPage* const page = pending_pages_;
        pending_pages_ = page->pending_next_;
        page->pending_next_ = nullptr;
        release_page(*page);
    }
    while (pending_layout_ != nullptr) {
        LayoutNode* const node = pending_layout_;
        pending_layout_ = node->pending_next_;
        node->pending_next_ = nullptr;
        destroy_layout(*node);
    }
    while (pending_protected_ != nullptr) {
        LayoutNode* const node = pending_protected_;
        pending_protected_ = node->pending_next_;
        node->pending_next_ = nullptr;
        KASSERT(node->kind_ == LayoutKind::Mapping);
        static_cast<Mapping*>(node)->state_ = MappingState::Live;
    }
    pending_kind_ = PendingKind::None;
    finish_authorities();
    try_finish_retire();
    return true;
}

} // namespace kernel::mm
namespace kernel::mm {

void VSpace::retire(object::ObjectCleanup&& cleanup) noexcept {
    bool can_start{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(state_ == VSpaceState::Stopping);
        KASSERT(bindings_ == 0);
        KASSERT(!cleanup_);
        [[maybe_unused]] auto& retained =
            cleanup_.emplace(libk::move(cleanup));
        can_start = pending_kind_ == PendingKind::None
            && claim_.region == nullptr
            && coherence_.active_cpus().empty();
    }
    if (can_start) {
        static_cast<void>(start_region_destroy(
            VmContext{.local = kernel::CpuId{0}},
            *root_region_,
            false,
            PendingKind::Retire));
    } else {
        schedule_work();
    }
    complete_cleanup();
}

} // namespace kernel::mm
