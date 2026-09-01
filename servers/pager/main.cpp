#include <user/lib/bootstrap.hpp>
#include <user/lib/syscall.hpp>
#include <uapi/bootstrap.h>
#include <uapi/pager.h>
#include <uapi/status.h>

namespace {

constexpr myos_word_t StagingAddress = 0x4300'1000;
constexpr myos_word_t IpcAddress = 0x4300'0000;
constexpr myos_word_t PageSize = 4096;
constexpr myos_word_t ServiceBadge = 1;

[[noreturn]] void stop(myos_status_t status = MYOS_STATUS_INTERNAL) noexcept {
    myos::exit(status);
}

[[nodiscard]] auto valid_page_in(const myos_pager_request& request) noexcept
    -> bool {
    return request.version == MYOS_PAGER_REQUEST_VERSION
        && request.kind == MYOS_PAGER_REQUEST_PAGE_IN
        && request.flags == MYOS_PAGER_REQUEST_FLAGS_NONE
        && request.delivery_generation != 0
        && request.claim_generation != 0
        && request.page_generation != 0
        && request.payload.page_in.count == 1
        && request.payload.page_in.backing_epoch != 0;
}

[[noreturn]] void run(const myos::bootstrap::BootstrapView& info) noexcept {
    const myos_cap_t pager = info.selector(MYOS_BOOTSTRAP_CAP_PAGER);
    const myos_cap_t target = info.selector(MYOS_BOOTSTRAP_CAP_TARGET_MEMORY);
    const myos_cap_t staging =
        info.selector(MYOS_BOOTSTRAP_CAP_STAGING_MEMORY);
    const myos_cap_t service =
        info.selector(MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    const myos_cap_t readiness =
        info.selector(MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION);
    const myos_cap_t staging_region =
        info.selector(MYOS_BOOTSTRAP_CAP_STAGING_REGION);
    if (pager == 0 || target == 0 || staging == 0 || service == 0
        || readiness == 0 || staging_region == 0) {
        stop(MYOS_STATUS_BAD_ARGS);
    }

    if (myos::pager_bind(pager, service, ServiceBadge).status
        != MYOS_STATUS_OK
        || myos::notification_signal(readiness).status != MYOS_STATUS_OK) {
        stop();
    }

    auto* const staging_page =
        reinterpret_cast<volatile myos_word_t*>(StagingAddress);
    for (;;) {
        const auto wake = myos::notification_wait(service);
        if (wake.status != MYOS_STATUS_OK
            || (wake.value & ServiceBadge) == 0) {
            stop(wake.status == MYOS_STATUS_OK
                     ? MYOS_STATUS_BAD_ARGS
                     : wake.status);
        }
        const auto claimed = myos::pager_claim(pager);
        if (claimed.status != MYOS_STATUS_OK) {
            stop(claimed.status);
        }
        auto* const request = reinterpret_cast<myos_pager_request*>(IpcAddress);
        if (request == nullptr || !valid_page_in(*request)) {
            stop(MYOS_STATUS_BAD_ARGS);
        }
        *staging_page = 0;
        request->payload.page_in.content_epoch = 1;
        for (;;) {
            const auto unmapped = myos::vm_unmap(
                staging_region, StagingAddress, PageSize);
            if (unmapped.status == MYOS_STATUS_OK
                || unmapped.status == MYOS_STATUS_PENDING) {
                break;
            }
            if (unmapped.status != MYOS_STATUS_BUSY
                && unmapped.status != MYOS_STATUS_RETRY) {
                stop(unmapped.status);
            }
            myos::yield();
        }
        for (;;) {
            const auto supplied = myos::pager_supply(
                pager, target, staging, request->page_index);
            if (supplied.status == MYOS_STATUS_OK) {
                break;
            }
            if (supplied.status != MYOS_STATUS_BUSY) {
                stop(supplied.status);
            }
            myos::yield();
        }
    }
}

} // namespace

extern "C" [[noreturn]] void myos_main(
    const void* bootstrap,
    myos_word_t bootstrap_size) noexcept {
    const auto info = myos::bootstrap::BootstrapView::parse(
        bootstrap, bootstrap_size);
    if (!info || info->cpu_count() == 0) {
        stop(MYOS_STATUS_BAD_ARGS);
    }
    run(*info);
}
