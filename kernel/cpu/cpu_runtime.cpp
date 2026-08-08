#include <cpu/cpu_runtime.hpp>

#include <diag/concurrency.hpp>
#include <libk/assert.hpp>

namespace kernel {

CpuRuntime::~CpuRuntime() noexcept {
    dispatcher_storage.reset();
    if (idle_thread) {
        KASSERT(idle_thread.retire());
        idle_thread.reset();
    }
    if (diagnostics != nullptr) {
        diag::concurrency::destroy(*this);
    }
}

} // namespace kernel
