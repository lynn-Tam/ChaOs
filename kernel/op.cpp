#include <op.hpp>
#include <sched/sched.hpp>

// Accepted work keeps its original CPU quota independently of the submitting
// Thread/SC. Both the provider and this Domain link share final Gate admission.
auto Op::admit_cpu(Publish publish) noexcept -> status_t {
    auto domain = cpu_.as<sched::Domain>();
    return domain ? domain->get().admit(cpu_link_, publish) : STATUS_BAD_ARGS;
}
auto Op::schedule() noexcept -> bool {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    auto domain = cpu.as<sched::Domain>();
    if (!domain) return false;
    auto check = [&]() noexcept {
        sync::Lock guard{lock_};
        return cpu_.id() == cpu.id() && phase_ != OP_DONE && phase_ != OP_DRAINING && !safety_ && !canceled_;
    };
    return domain->get().post(*runner_, libk::delegate<bool() noexcept>::bind(check)) == STATUS_OK;
}
void Op::run() noexcept {
    object::ref<> provider;
    Source::Action progress;
    { sync::Lock guard{lock_}; provider = provider_; progress = progress_; }
    if (progress) progress();
}
void Op::cancel_work() noexcept {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = cpu_; safety_ = true; if (phase_ == OP_ACCEPTED) canceled_ = true; }
    if (auto domain = cpu.as<sched::Domain>()) (void)domain->get().withdraw(*runner_, runner_ == &job_);
    wake();
}
auto Op::detach() noexcept -> bool {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    if (auto domain = cpu.as<sched::Domain>(); domain && !domain->get().release(cpu_link_)) return false;
    return bindings_.close();
}
void Op::release_cpu() noexcept {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = std::move(cpu_); progress_ = {}; }
    if (auto domain = cpu.as<sched::Domain>()) {
        libk_assert(domain->get().release(cpu_link_));
        (void)domain->get().withdraw(*runner_, runner_ == &job_);
    }
}

auto Op::commit() noexcept -> bool {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    auto domain = cpu.as<sched::Domain>();
    bool committed{};
    auto publish = [&]() noexcept {
        sync::Lock guard{lock_};
        libk_assert(phase_ == OP_ACCEPTED);
        if (canceled_ || safety_) return;
        phase_ = OP_COMMITTED; committed = true;
    };
    auto commit = [&]() noexcept { (void)bindings_.publish(Source::Action::bind(publish)); };
    if (domain) (void)domain->get().commit(Source::Action::bind(commit));
    else commit();
    return committed;
}

auto Op::start(object::ref<> cpu, const object::ref<>& self, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates, Source::Action install, const cap::GrantPin* provider_grant) noexcept -> status_t {
    cpu_domain(std::move(cpu));
    auto admitted = [&]() noexcept {
        auto commit = [&]() noexcept { accept(); if (install) install(); };
        return bindings_.admit([&](std::span<cap::Attachment*> cells, Source::Action publish) noexcept {
            const cap::Graph::Attach items[]{{provider_grant, cells[0], provider_grant ? nullptr : &self},
                {nullptr, cells[1], &cpu_}};
            return admission.attach(items, gates, publish) ? STATUS_OK : STATUS_CLOSED;
        }, Source::Action::bind(commit));
    };
    return admit_cpu(Publish::bind(admitted));
}
