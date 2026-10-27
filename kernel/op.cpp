#include <op.hpp>
#include <sched/sched.hpp>

// Accepted work keeps its original CPU quota independently of the submitting
// Thread/SC. Its Work::Link owns Domain cancellation and drain; capability
// bindings retain only provider authority, without a duplicate Domain link.
auto Op::admit_cpu(Publish publish) noexcept -> status_t {
    auto domain = cpu_.as<sched::ScDomain>();
    return domain ? domain->get().admit(cpu_link_, publish) : STATUS_BAD_ARGS;
}
auto Op::schedule() noexcept -> bool {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    auto domain = cpu.as<sched::ScDomain>();
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
    if (auto domain = cpu.as<sched::ScDomain>()) (void)domain->get().withdraw(*runner_, runner_ == &job_);
    wake();
}
auto Op::detach() noexcept -> bool {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    if (auto domain = cpu.as<sched::ScDomain>(); domain && !domain->get().release(cpu_link_)) return false;
    return bindings_.close();
}
void Op::release_cpu() noexcept {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = std::move(cpu_); progress_ = {}; }
    if (auto domain = cpu.as<sched::ScDomain>()) {
        libk_assert(domain->get().release(cpu_link_));
        (void)domain->get().withdraw(*runner_, runner_ == &job_);
    }
}

auto Op::commit() noexcept -> bool {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    auto domain = cpu.as<sched::ScDomain>();
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
    std::span<const object::ref<>*> gates, Source::Action install, std::span<const cap::GrantPin*> grants) noexcept -> status_t {
    if (grants.size() > 2) return STATUS_BAD_ARGS;
    const auto providers = std::max<usize>(1, grants.size());
    std::array<const object::ref<>*, cap::Graph::max_attachments + 1> publication_gates;
    if (gates.size() >= publication_gates.size()) return STATUS_BAD_ARGS;
    std::copy(gates.begin(), gates.end(), publication_gates.begin());
    cpu_domain(std::move(cpu));
    publication_gates[gates.size()] = &cpu_;
    auto admitted = [&]() noexcept {
        auto commit = [&]() noexcept { accept(); if (install) install(); };
        return bindings_.admit([&](std::span<cap::Attachment*> cells, Source::Action publish) noexcept {
            std::array<cap::Graph::Attach, 2> items;
            if (grants.empty()) items[0] = {nullptr, cells[0], &self};
            else for (usize i = 0; i < grants.size(); ++i) items[i] = {grants[i], cells[i]};
            return admission.attach(std::span{items}.first(providers),
                std::span{publication_gates}.first(gates.size() + 1), publish) ? STATUS_OK : STATUS_CLOSED;
        }, Source::Action::bind(commit), providers);
    };
    return admit_cpu(Publish::bind(admitted));
}
