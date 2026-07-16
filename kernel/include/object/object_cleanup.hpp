#pragma once

#include <core/debug.hpp>
#include <libk/noncopyable.hpp>
#include <libk/utility.hpp>
#include <object/object_anchor.hpp>

namespace kernel::object {

// Move-only ownership of the ObjectPool retire pin. Synchronous objects
// complete it in their retire hook; asynchronous objects retain it until all
// subsystem cleanup (including hardware retirement) is actually complete.
// It changes only the anchor's canonical cleanup gate and never creates a
// second payload lifecycle counter.
class ObjectCleanup final : private libk::noncopyable {
public:
    ObjectCleanup() noexcept = default;

    ObjectCleanup(ObjectCleanup&& other) noexcept
        : anchor_(libk::exchange(other.anchor_, nullptr)),
          generation_(libk::exchange(other.generation_, u64{})) {}

    auto operator=(ObjectCleanup&& other) noexcept -> ObjectCleanup& {
        if (this != &other) {
            KASSERT(anchor_ == nullptr);
            anchor_ = libk::exchange(other.anchor_, nullptr);
            generation_ = libk::exchange(other.generation_, u64{});
        }
        return *this;
    }

    ~ObjectCleanup() noexcept { KASSERT(anchor_ == nullptr); }

    [[nodiscard]] explicit operator bool() const noexcept {
        return anchor_ != nullptr;
    }

    void complete() noexcept {
        ObjectAnchor* const anchor = libk::exchange(anchor_, nullptr);
        const u64 generation = libk::exchange(generation_, u64{});
        KASSERT(anchor != nullptr);
        anchor->ops_->finish_cleanup(
            anchor->owner_, *anchor, generation);
    }

private:
    template<typename T>
    friend class ObjectPool;

    ObjectCleanup(ObjectAnchor& anchor, u64 generation) noexcept
        : anchor_(&anchor), generation_(generation) {}

    ObjectAnchor* anchor_{};
    u64 generation_{};
};

} // namespace kernel::object
