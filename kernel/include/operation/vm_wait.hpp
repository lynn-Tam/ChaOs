#pragma once

#include <libk/intrusive_list.hpp>
#include <object/object_ref.hpp>
#include <operation/completion.hpp>

namespace kernel::mm { class VSpace; }

namespace kernel::operation {

// Observes the VM transaction current at admission. The accepted object
// reference survives capability revocation and pins the observer's source.
class VmWait final : private libk::noncopyable_nonmovable {
public:
    VmWait(object::ObjectRef&& target, mm::VSpace& space) noexcept;
    ~VmWait() noexcept;
    auto completion() noexcept -> Completion& { return completion_; }
    void start() noexcept;

private:
    friend class mm::VSpace;
    auto complete() const noexcept -> bool;
    auto read() noexcept -> Result;
    auto cancel() noexcept -> bool;
    void release() noexcept;

    object::ObjectRef target_;
    mm::VSpace* space_{};
    libk::IntrusiveListHook hook_{};
    libk::Atomic<bool> ready_{};
    Completion completion_;
};

} // namespace kernel::operation
