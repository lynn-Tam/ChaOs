#include <thread/wait.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <sched/dispatcher.hpp>
#include <sched/binding.hpp>
#include <thread/thread.hpp>

namespace kernel {

WaitRelation::~WaitRelation() noexcept {
    KASSERT(owner_ != nullptr && ops_ != nullptr);
    KASSERT(!attached());
}

void WaitRelation::attach(Thread& thread, CpuRegistry& cpus) noexcept {
    KASSERT(!attached());
    KASSERT(thread.binding() != nullptr);
    thread_ = &thread;
    cpus_ = &cpus;
}

void WaitRelation::notify() noexcept {
    KASSERT(complete());
    KASSERT(thread_ != nullptr && cpus_ != nullptr);
    sched::Binding* const binding = thread_->binding();
    KASSERT(binding != nullptr);
    KASSERT(sched::wake(*cpus_, *binding));
}

void WaitRelation::resume(arch::TrapContext& trap) noexcept {
    KASSERT(complete());
    detach();
    ops_->resume(owner_, trap);
}

void WaitRelation::cancel() noexcept {
    KASSERT(!complete());
    detach();
    ops_->cancel(owner_);
}

void WaitRelation::detach() noexcept {
    KASSERT(attached());
    thread_ = nullptr;
    cpus_ = nullptr;
}

} // namespace kernel
