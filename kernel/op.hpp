#pragma once

#include <cap/grant.hpp>
#include <wait.hpp>
#include <uapi/cap.h>

namespace sched { class ScDomain; }

// Common accepted-work lifetime and cancellation. Concrete providers own the
// resources and hardware proof; this record only owns authority and results.
class Op final {
    mutable sync::Spin lock_;
    object::ref<> cpu_;
    Work::Link cpu_link_{Work::Fn::bind<&Op::invalidated>(*this), Work::Fn::bind<&Op::wake>(*this)};
    Source::Action progress_;
    Work job_{Work::Fn::bind<&Op::run>(*this), Work::Fn::bind<&Op::cancel_work>(*this)};
    Work* runner_;
    using Publish = libk::delegate<status_t() noexcept>;
    auto admit_cpu(Publish) noexcept -> status_t;
    void release_cpu() noexcept;
    void run() noexcept;
    void cancel_work() noexcept;
    u64 phase_{};
    status_t status_{STATUS_PENDING};
    u64 value_{};
    bool canceled_{}, safety_{};
    object::ref<> provider_;
    Source::Action wake_;
    object::cleanup cleanup_;
    Src<OP_ACCEPTED | OP_COMMITTED | OP_DRAINING | OP_DONE> events_{Source::Drained::bind<&Op::cleanup>(*this)};

    void invalidated() noexcept {
        { sync::Lock guard{lock_}; safety_ = true; if (phase_ == OP_ACCEPTED) canceled_ = true; }
        wake();
    }
    void wake() noexcept {
        object::ref<> provider;
        Source::Action wake;
        { sync::Lock guard{lock_}; provider = provider_; wake = wake_; }
        if (wake) wake();
    }
    cap::Bindings<2> bindings_{Source::Action::bind<&Op::invalidated>(*this), Source::Action::bind<&Op::wake>(*this)};

    void cleanup() noexcept {
        object::cleanup done;
        if (!bindings_.drained() || !events_.drained()) return;
        {
            sync::Lock guard{lock_};
            if (phase_ != OP_DONE) return;
            done = std::move(cleanup_);
        }
        if (done) done.complete();
    }
public:
    struct Submission {
        object::ref<>* execution{};
        libk::delegate<status_t(Source::Action, std::span<const cap::GrantPin*>) noexcept> publish{};
    };
    // wake only posts prepaid provider intent; it must not execute the provider.
    explicit Op(object::ref<> provider = {}, Source::Action wake = {}, Source::Action progress = {}, Work* runner = nullptr) noexcept
        : progress_(progress), runner_(runner ? runner : &job_), provider_(std::move(provider)), wake_(wake) {}
    ~Op() noexcept {
        libk_assert(!cleanup_);
    }
    void bind(WorkQueue& work, object::Key owner) noexcept { bindings_.bind(owner); job_.bind(owner); cpu_link_.bind(owner); events_.bind_work(work, owner); }
    void cpu_domain(object::ref<> cpu) noexcept { libk_assert(!phase_); cpu_ = std::move(cpu); }
    auto schedule() noexcept -> bool;
    // Continuing business returns directly to its CPU domain; only safety tails
    // and externally resumed completion use the provider maintenance wake.
    void post() noexcept { if (!schedule()) wake(); }
    auto start(object::ref<> cpu, const object::ref<>& self, cap::Graph::Admission = {},
        std::span<const object::ref<>*> gates = {}, Source::Action install = {}, std::span<const cap::GrantPin*> grants = {}) noexcept -> status_t;
    auto canceled() const noexcept -> bool { sync::Lock guard{lock_}; return canceled_; }
    auto provider() const noexcept -> object::ref<> { sync::Lock guard{lock_}; return provider_; }
    void links(u64 cursor, LinkRecords& out) const noexcept { bindings_.links(out, LINK_USE, cursor); }
    auto events() noexcept -> Source& { return events_; }
    auto query() const noexcept -> WaitResult { sync::Lock guard{lock_}; return {status_, value_, phase_}; }
    void accept() noexcept { sync::Lock guard{lock_}; libk_assert(!phase_); phase_ = OP_ACCEPTED; }
    template<class Publication> auto accept(Source::Action install, Publication&& publish) noexcept -> status_t {
        auto commit = [&] noexcept { accept(); install(); };
        auto submit = [&]() noexcept {
            return bindings_.admit([&](std::span<cap::Attachment*> cells, Source::Action admitted) noexcept {
                return publish(cells, cpu_, admitted);
            }, Source::Action::bind(commit), 1);
        };
        return admit_cpu(Publish::bind(submit));
    }
    auto commit() noexcept -> bool;
    auto cancel() noexcept -> status_t {
        object::ref<> provider;
        Source::Action wake;
        {
            sync::Lock guard{lock_};
            if (phase_ && phase_ != OP_ACCEPTED) return STATUS_TOO_LATE;
            canceled_ = true; provider = provider_; wake = wake_;
        }
        if (wake) wake();
        return STATUS_OK;
    }
    void drain() noexcept {
        Source::Publication publication;
        { sync::Lock guard{lock_};
          libk_assert(phase_ != OP_DONE);
          if (phase_ != OP_DRAINING) { phase_ = OP_DRAINING; publication = events_.prepare(phase_, true); }
        }
    }
    void publish() noexcept {
        Source::Publication publication;
        {
            sync::Lock guard{lock_};
            if (events_.snapshot().value != phase_) publication = events_.prepare(phase_, true);
        }
    }
    auto ready() const noexcept -> bool { return bindings_.ready(); }
    auto detach() noexcept -> bool;
    // The provider has removed its active transaction before publishing Done.
    void finish(status_t status = STATUS_OK, u64 value = 0) noexcept {
        libk_assert(bindings_.drained());
        bool closing, internal;
        object::ref<> provider;
        {
            sync::Lock guard{lock_};
            libk_assert(phase_ != OP_DONE);
            internal = !provider_;
            provider = std::move(provider_); wake_ = {};
        }
        provider.reset();
        release_cpu();
        Source::Publication publication;
        {
            sync::Lock guard{lock_};
            status_ = canceled_ ? STATUS_CANCELED : status; value_ = value; phase_ = OP_DONE;
            closing = bool(cleanup_);
            publication = events_.prepare(phase_, true);
        }
        publication.publish();
        if (closing || internal) events_.close();
    }
    void retire(object::cleanup&& done) noexcept {
        u64 phase;
        { sync::Lock guard{lock_}; libk_assert(!cleanup_); cleanup_ = std::move(done); phase = phase_; }
        if (!phase) { libk_assert(bindings_.close()); finish(STATUS_CANCELED); }
        else if (phase != OP_DONE) { (void)cancel(); return; }
        if (events_.drained()) cleanup();
        else events_.close();
    }
};
