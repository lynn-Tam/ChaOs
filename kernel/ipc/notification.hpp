#pragma once

#include <object/ref.hpp>
#include <wait.hpp>

namespace ipc {

class Notification final : private libk::noncopyable_nonmovable {
public:
    static constexpr bool fixed_usage = true;
    auto events() noexcept -> Source& { return events_; }
    void bind_work(WorkQueue& work, object::Key key) noexcept { events_.bind_work(work, key); }
    ~Notification() noexcept { libk_assert(!cleanup_); }
    // The immutable badge belongs to the signaling capability.
    auto signal(u64 badge) noexcept -> bool { return badge && events_.post(badge); }
    void retire(object::cleanup&& cleanup) noexcept {
        { sync::Lock guard{lock_}; libk_assert(!cleanup_); cleanup_ = std::move(cleanup); }
        events_.close();
    }
private:
    void complete() noexcept {
        object::cleanup cleanup;
        { sync::Lock guard{lock_};
          if (!events_.drained()) return;
          cleanup = std::move(cleanup_);
        }
        if (cleanup) cleanup.complete();
    }
    sync::Spin lock_;
    Src<~u64{}> events_{Source::Drained::bind<&Notification::complete>(*this)};
    object::cleanup cleanup_;
};

} // namespace ipc
