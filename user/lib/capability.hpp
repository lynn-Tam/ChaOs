#pragma once

#include <stddef.h>

#include <libk/concepts.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/optional.hpp>
#include <libk/utility.hpp>
#include <uapi/capability.h>
#include <uapi/status.h>

namespace myos::cap {

// A selector plus the CSpace in which that selector is installed.  A zero
// CSpace is the caller's current CSpace; nonzero values are borrowed manager
// authorities used only by the remote CAP_CLOSE ABI.
struct CapRef final {
    myos_cap_t selector{};
    myos_cap_t cspace{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return selector != 0;
    }

    [[nodiscard]] friend constexpr auto operator==(
        const CapRef&, const CapRef&) noexcept -> bool = default;
};

template<typename Backend>
concept CapBackend = requires(CapRef reference, myos_status_t status) {
    { Backend::close(reference) } -> libk::SameAs<myos_status_t>;
    { Backend::ownership_fault(status) } noexcept;
};

// Backend is deliberately static: production userspace gets a direct syscall
// call and host tests inject a fixed fake without a vtable, heap or queue.
template<CapBackend Backend>
class BasicOwnedCap final {
public:
    constexpr BasicOwnedCap() noexcept = default;

    constexpr explicit BasicOwnedCap(CapRef reference) noexcept
        : reference_(reference) {}

    BasicOwnedCap(const BasicOwnedCap&) = delete;
    auto operator=(const BasicOwnedCap&) -> BasicOwnedCap& = delete;

    constexpr BasicOwnedCap(BasicOwnedCap&& other) noexcept
        : reference_(other.release()) {}

    auto operator=(BasicOwnedCap&& other) noexcept -> BasicOwnedCap& {
        if (this == &other) {
            return *this;
        }
        if (reference_) {
            const myos_status_t status = close();
            if (status != MYOS_STATUS_OK) {
                Backend::ownership_fault(status);
            }
        }
        reference_ = other.release();
        return *this;
    }

    ~BasicOwnedCap() noexcept {
        if (!reference_) {
            return;
        }
        // Destruction is permitted one bounded fallback only.  A failed
        // close cannot be queued or silently discarded without losing the
        // selector, so the backend must fail-stop.
        const myos_status_t status = Backend::close(reference_);
        if (status == MYOS_STATUS_OK) {
            reference_ = {};
            return;
        }
        Backend::ownership_fault(status);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(reference_);
    }

    [[nodiscard]] constexpr auto reference() const noexcept -> CapRef {
        return reference_;
    }

    [[nodiscard]] constexpr auto selector() const noexcept -> myos_cap_t {
        return reference_.selector;
    }

    [[nodiscard]] constexpr auto cspace() const noexcept -> myos_cap_t {
        return reference_.cspace;
    }

    // Explicit close retains the reference on every non-OK result.
    [[nodiscard]] auto close() noexcept -> myos_status_t {
        if (!reference_) {
            return MYOS_STATUS_OK;
        }
        const myos_status_t status = Backend::close(reference_);
        if (status == MYOS_STATUS_OK) {
            reference_ = {};
        }
        return status;
    }

    // Transfer ownership without invoking the backend.
    [[nodiscard]] constexpr auto release() noexcept -> CapRef {
        return libk::exchange(reference_, CapRef{});
    }

private:
    CapRef reference_{};
};

// One bounded ownership journal.  Remote selectors are drained first, in
// reverse insertion order, before current-CSpace roots.  This makes a
// borrowed manager CSpace structurally safe to close even when it was adopted
// before its remote children.
template<size_t Capacity, CapBackend Backend>
class BasicCapJournal final {
public:
    using owner_type = BasicOwnedCap<Backend>;

    BasicCapJournal() noexcept = default;
    BasicCapJournal(const BasicCapJournal&) = delete;
    auto operator=(const BasicCapJournal&) -> BasicCapJournal& = delete;

    BasicCapJournal(BasicCapJournal&& other) noexcept
        : entries_(libk::move(other.entries_)) {}

    auto operator=(BasicCapJournal&& other) noexcept -> BasicCapJournal& {
        if (this == &other) {
            return *this;
        }
        const myos_status_t status = close();
        if (status != MYOS_STATUS_OK) {
            Backend::ownership_fault(status);
        }
        entries_ = libk::move(other.entries_);
        return *this;
    }

    ~BasicCapJournal() noexcept {
        const myos_status_t status = close();
        if (status != MYOS_STATUS_OK) {
            Backend::ownership_fault(status);
        }
    }

    // The cap is adopted before attempting this fallible insertion.  If the
    // journal is full, owner destruction performs the bounded close fallback.
    [[nodiscard]] auto adopt(CapRef reference) noexcept -> bool {
        if (!reference) {
            return false;
        }
        owner_type owner{reference};
        return adopt(libk::move(owner));
    }

    // Transfer an already-owned selector without changing its CSpace
    // identity.  A full journal leaves the caller's owner armed so its
    // destructor or explicit close still targets the original selector.
    [[nodiscard]] auto adopt(owner_type&& owner) noexcept -> bool {
        if (!owner) {
            return false;
        }
        return entries_.try_push_back(libk::move(owner));
    }

    // Append a current-CSpace owner and return its never-reused journal
    // index.  A full journal leaves the temporary owner armed until its
    // bounded destructor close, matching the existing CapRef adoption path.
    [[nodiscard]] auto adopt_index(owner_type&& owner) noexcept
        -> libk::optional<size_t> {
        if (!owner || entries_.size() == Capacity) {
            return libk::nullopt;
        }
        const size_t index = entries_.size();
        if (!entries_.try_push_back(libk::move(owner))) {
            return libk::nullopt;
        }
        return index;
    }

    [[nodiscard]] auto adopt_index(CapRef reference) noexcept
        -> libk::optional<size_t> {
        if (!reference) {
            return libk::nullopt;
        }
        owner_type owner{reference};
        return adopt_index(libk::move(owner));
    }

    [[nodiscard]] auto adopt(
        myos_cap_t selector,
        myos_cap_t cspace = 0) noexcept -> bool {
        return adopt(CapRef{selector, cspace});
    }

    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return entries_.size();
    }

    // Journal size is cumulative (slots are tombstoned after close).  This
    // live count is a derived view for publication/rollback evidence; it is
    // not a second ownership state.
    [[nodiscard]] auto live_size() const noexcept -> size_t {
        size_t count = 0;
        for (const owner_type& owner : entries_) {
            if (owner) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] static constexpr auto capacity() noexcept -> size_t {
        return Capacity;
    }

    [[nodiscard]] auto borrow(size_t index) const noexcept
        -> libk::optional<CapRef> {
        if (index >= entries_.size() || !entries_[index]) {
            return libk::nullopt;
        }
        return entries_[index].reference();
    }

    [[nodiscard]] auto close_at(size_t index) noexcept -> myos_status_t {
        if (index >= entries_.size()) {
            return MYOS_STATUS_INVALID_CAP;
        }
        return entries_[index].close();
    }

    [[nodiscard]] auto close() noexcept -> myos_status_t {
        const myos_status_t remote = close_pass(true);
        if (remote != MYOS_STATUS_OK) {
            return remote;
        }
        const myos_status_t current = close_pass(false);
        if (current != MYOS_STATUS_OK) {
            return current;
        }
        entries_.clear();
        return MYOS_STATUS_OK;
    }

private:
    [[nodiscard]] auto close_pass(bool remote) noexcept -> myos_status_t {
        for (size_t index = entries_.size(); index != 0; --index) {
            owner_type& owner = entries_[index - 1];
            if (!owner || ((owner.cspace() != 0) != remote)) {
                continue;
            }
            const myos_status_t status = owner.close();
            if (status != MYOS_STATUS_OK) {
                return status;
            }
        }
        return MYOS_STATUS_OK;
    }

    libk::InplaceVector<owner_type, Capacity> entries_{};
};

// A deployment owns one manager CSpace, current-CSpace roots and selectors
// installed in that manager.  The manager is private so remote adoption is
// the only way to create a remote owner; callers cannot release or close it
// while remote selectors remain armed.
template<size_t LocalCapacity, size_t RemoteCapacity, CapBackend Backend>
class BasicDeploymentCaps final {
public:
    using owner_type = BasicOwnedCap<Backend>;

    BasicDeploymentCaps() noexcept = default;
    BasicDeploymentCaps(const BasicDeploymentCaps&) = delete;
    auto operator=(const BasicDeploymentCaps&) -> BasicDeploymentCaps& = delete;

    BasicDeploymentCaps(BasicDeploymentCaps&& other) noexcept
        : sealed_(other.sealed_),
          local_(libk::move(other.local_)),
          manager_(libk::move(other.manager_)),
          remote_(libk::move(other.remote_)) {
        other.sealed_ = true;
    }

    auto operator=(BasicDeploymentCaps&& other) noexcept
        -> BasicDeploymentCaps& {
        if (this == &other) {
            return *this;
        }
        const myos_status_t status = close();
        if (status != MYOS_STATUS_OK) {
            Backend::ownership_fault(status);
        }
        sealed_ = other.sealed_;
        local_ = libk::move(other.local_);
        manager_ = libk::move(other.manager_);
        remote_ = libk::move(other.remote_);
        other.sealed_ = true;
        return *this;
    }

    ~BasicDeploymentCaps() noexcept {
        const myos_status_t status = close();
        if (status != MYOS_STATUS_OK) {
            Backend::ownership_fault(status);
        }
    }

    // The manager is installed at most once and must be a current-CSpace cap.
    [[nodiscard]] auto install_manager(owner_type&& manager) noexcept -> bool {
        if (sealed_ || manager_ || !manager || manager.cspace() != 0) {
            return false;
        }
        manager_ = libk::move(manager);
        return true;
    }

    // Local roots are current-CSpace selectors.  The owner remains armed when
    // the aggregate rejects the adoption (for example after sealing).
    [[nodiscard]] auto adopt_local(owner_type&& owner) noexcept -> bool {
        if (sealed_ || !owner || owner.cspace() != 0) {
            return false;
        }
        const CapRef reference = owner.release();
        return local_.adopt(reference);
    }

    // Preflight is observational only: it does not reserve a journal slot or
    // otherwise create a second admission state.
    [[nodiscard]] auto can_adopt_remote() const noexcept -> bool {
        return !sealed_ && manager_
            && remote_.size() < remote_.capacity();
    }

    [[nodiscard]] auto can_adopt_remote(
        const owner_type& owner) const noexcept -> bool {
        return can_adopt_remote()
            && owner
            && owner.cspace() == manager_.selector();
    }

    // Remote-producing callers construct the owner with the destination
    // manager CSpace at the syscall boundary.  Adoption accepts only that
    // exact identity and moves it without rewriting the CapRef.
    [[nodiscard]] auto adopt_remote(owner_type&& owner) noexcept -> bool {
        if (!can_adopt_remote(owner)) {
            return false;
        }
        return remote_.adopt(libk::move(owner));
    }

    [[nodiscard]] auto adopt_remote_index(owner_type&& owner) noexcept
        -> libk::optional<size_t> {
        if (!can_adopt_remote(owner)) {
            return libk::nullopt;
        }
        return remote_.adopt_index(libk::move(owner));
    }

    [[nodiscard]] auto close_remote(size_t index) noexcept -> myos_status_t {
        if (sealed_) {
            return MYOS_STATUS_CLOSED;
        }
        return remote_.close_at(index);
    }

    [[nodiscard]] auto adopt_local_slot(
        owner_type&& owner) noexcept -> libk::optional<size_t> {
        if (sealed_ || !owner || owner.cspace() != 0
            || local_.size() == local_.capacity()) {
            return libk::nullopt;
        }
        return local_.adopt_index(libk::move(owner));
    }

    [[nodiscard]] auto close_local(size_t index) noexcept -> myos_status_t {
        if (sealed_) {
            return MYOS_STATUS_CLOSED;
        }
        return local_.close_at(index);
    }

    [[nodiscard]] auto borrow_manager() const noexcept
        -> libk::optional<CapRef> {
        if (!manager_) {
            return libk::nullopt;
        }
        return manager_.reference();
    }

    [[nodiscard]] auto borrow_local(size_t index) const noexcept
        -> libk::optional<CapRef> {
        return local_.borrow(index);
    }

    [[nodiscard]] auto borrow_remote(size_t index) const noexcept
        -> libk::optional<CapRef> {
        return remote_.borrow(index);
    }

    [[nodiscard]] constexpr auto sealed() const noexcept -> bool {
        return sealed_;
    }

    [[nodiscard]] constexpr auto local_size() const noexcept -> size_t {
        return local_.size();
    }

    [[nodiscard]] constexpr auto remote_size() const noexcept -> size_t {
        return remote_.size();
    }

    [[nodiscard]] auto remote_live_size() const noexcept -> size_t {
        return remote_.live_size();
    }

    // The first attempt seals adoption even when close returns an error.  A
    // retry may finish the same remote->manager->local drain, but no new cap
    // can enter after that first attempt.
    [[nodiscard]] auto close() noexcept -> myos_status_t {
        sealed_ = true;
        const myos_status_t remote_status = remote_.close();
        if (remote_status != MYOS_STATUS_OK) {
            return remote_status;
        }
        const myos_status_t manager_status = manager_.close();
        if (manager_status != MYOS_STATUS_OK) {
            return manager_status;
        }
        return local_.close();
    }

private:
    bool sealed_{};
    // Declaration order is intentional: destruction is remote, manager,
    // local, matching close() even if a future no-fail path skips the body.
    BasicCapJournal<LocalCapacity, Backend> local_{};
    owner_type manager_{};
    BasicCapJournal<RemoteCapacity, Backend> remote_{};
};

} // namespace myos::cap
