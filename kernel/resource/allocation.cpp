#include <resource/allocation.hpp>

#include <cap/grant_graph.hpp>
#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <resource/pool.hpp>
#include <operation/completion.hpp>

namespace kernel::resource {

Allocation::Allocation() noexcept
    : revoke_(kernel::sync::Completion::Notifier::bind<
          &Allocation::ready>(*this)),
      stop_(execution::Stop::Notifier::bind<
          &Allocation::target_ready>(*this)) {}

void Allocation::ready() noexcept {
    KASSERT(pool_ != nullptr && revoke_.complete());
    pool_->ready(*this);
}

void Allocation::target_ready() noexcept {
    KASSERT(pool_ != nullptr);
    pool_->target_ready(*this);
}

void Allocation::child_closed() noexcept {
    KASSERT(pool_ != nullptr);
    pool_->child_closed(*this);
}

CloseWait::CloseWait(kernel::cap::GrantGraph& graph) noexcept
    : graph_(&graph),
      completion_(kernel::sync::Completion::Notifier::bind<
          &CloseWait::refunded>(*this)),
      relation_(kernel::operation::Completion::bind<
          CloseWait,
          &CloseWait::complete,
          &CloseWait::read,
          &CloseWait::release,
          &CloseWait::cancel>(*this)) {
    completion_.initialize(1);
}

auto CloseWait::notifier() noexcept -> RefundNotifier {
    return RefundNotifier::bind<&CloseWait::refunded>(*this);
}

void CloseWait::refunded() noexcept {
    if (!completion_.complete()) {
        completion_.acknowledge();
        return;
    }
    relation_.signal();
}

auto CloseWait::read() noexcept -> kernel::operation::Result {
    KASSERT(completion_.complete());
    return {};
}

void CloseWait::release() noexcept {
    kernel::cap::GrantGraph* const graph = graph_;
    KASSERT(graph != nullptr);
    graph->destroy_close_wait(*this);
}

auto CloseWait::cancel() noexcept -> bool {
    if (committed_ || complete()) {
        return false;
    }
    completion_.acknowledge();
    return true;
}

Allocation::~Allocation() noexcept {
    KASSERT(state_ == AllocationState::Empty);
    KASSERT(pool_ == nullptr && graph_ == nullptr && !root_.valid());
    KASSERT(!target_ && previous_ == nullptr && next_ == nullptr);
    KASSERT(!stop_.started() || stop_.complete());
    KASSERT(!independent_close_);
}

AllocationTxn::AllocationTxn(AllocationTxn&& other) noexcept
    : graph_(libk::exchange(other.graph_, nullptr)),
      allocation_(libk::exchange(other.allocation_, nullptr)),
      root_(libk::move(other.root_)) {}

auto AllocationTxn::operator=(AllocationTxn&& other) noexcept
    -> AllocationTxn& {
    if (this == &other) {
        return *this;
    }
    reset();
    graph_ = libk::exchange(other.graph_, nullptr);
    allocation_ = libk::exchange(other.allocation_, nullptr);
    root_ = libk::move(other.root_);
    return *this;
}

AllocationTxn::~AllocationTxn() noexcept { reset(); }

auto AllocationTxn::acquire() const noexcept
    -> libk::Expected<kernel::cap::GrantLease, kernel::cap::GrantError> {
    if (graph_ == nullptr || allocation_ == nullptr) {
        return libk::unexpected(kernel::cap::GrantError::InvalidState);
    }
    return root_.acquire();
}

void AllocationTxn::commit() noexcept {
    KASSERT(graph_ != nullptr && allocation_ != nullptr && root_);
    kernel::cap::GrantGraph* const graph =
        libk::exchange(graph_, nullptr);
    Allocation* const allocation =
        libk::exchange(allocation_, nullptr);
    graph->commit_allocation(*allocation);
    root_.reset();
}

void AllocationTxn::reset() noexcept {
    kernel::cap::GrantGraph* const graph =
        libk::exchange(graph_, nullptr);
    Allocation* const allocation =
        libk::exchange(allocation_, nullptr);
    if (graph == nullptr) {
        KASSERT(allocation == nullptr);
        root_.reset();
        return;
    }
    KASSERT(allocation != nullptr && root_);
    graph->abort_allocation(*allocation);
    root_.reset();
}

} // namespace kernel::resource
