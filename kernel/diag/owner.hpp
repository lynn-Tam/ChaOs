#pragma once

#include <diag/concurrency_private.hpp>
#include <diag/panic.hpp>
#include <mm/pmm.hpp>

namespace kernel::diag {

// Per-CPU diagnostic owner.  CpuRuntime exposes only a pointer to this type;
// all recorder/shard/report pages and their teardown stay in the diagnostic
// module.  The owner page itself carries stable panic/lock handles while the
// concurrency core and its child pages are separately owned here.
struct CpuDiagnostics final {
    PanicSlot panic{};
    sync::CpuLockTrace locks{};
    concurrency::CpuDiagnosticsCore* concurrency{};

    mm::OwnedPage concurrency_page{};
    mm::OwnedPage observation_store_page{};
    mm::OwnedPage observation_pages[
        concurrency::ObservationShard::pages]{};
    mm::OwnedPage flight_page{};
    mm::OwnedPage flight_records[
        concurrency::FlightRecorder::page_count]{};
    mm::OwnedPage lock_profile_page{};
};

static_assert(sizeof(CpuDiagnostics) <= mm::page_size);

} // namespace kernel::diag
