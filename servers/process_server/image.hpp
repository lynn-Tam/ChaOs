#pragma once

#include <user/lib/image_materializer.hpp>
#include <user/lib/deployment_syscall.hpp>
#include <user/lib/cap_attenuation.hpp>
#include <libk/noncopyable.hpp>
#include <user/lib/service.hpp>
#include <uapi/pager.h>

namespace myos::process {

// One resident scratch page is shared by this single-threaded pager loop.
// Instance state contains only segment sources and per-child pager relations.
class PageBuffer final {
    friend class Image;
    cap::OwnedCap staging_, descriptor_;
    myos_cap_t pool_{}, cspace_{}, events_{};
    uint8_t bytes_[4096]{};
public:
    auto open(myos_cap_t pool, myos_cap_t cspace, myos_cap_t events) noexcept -> myos_status_t {
        pool_ = pool; cspace_ = cspace; events_ = events;
        const auto staging = memory_create(pool, 4096, MYOS_VM_READ | MYOS_VM_WRITE);
        if (staging.status != MYOS_STATUS_OK) return staging.status;
        staging_ = cap::OwnedCap{{staging.value, 0}};
        const auto descriptor = memory_create(pool, 4096, MYOS_VM_READ | MYOS_VM_WRITE);
        if (descriptor.status != MYOS_STATUS_OK) return descriptor.status;
        descriptor_ = cap::OwnedCap{{descriptor.value, 0}};
        return MYOS_STATUS_OK;
    }
};

// Construction sources outlive their children and borrow the mapped package.
// Immutable pages use the Files object; writable pages get one private object
// per segment. Only that object's PageSlot owns residency and dirty state.
class Image final : private libk::noncopyable_nonmovable {
    struct Private final {
        cap::OwnedCap pager, memory;
        boot::Segment segment{};
    };
    Private private_[MYOS_DEPLOY_TASK_MAPPING_MAX];
    size_t count_{};
    PageBuffer* buffer_{};
    myos_cap_t package_{};
    uintptr_t package_address_{};
    size_t package_size_{};

    auto create(cap::CapRef pool, const boot::Segment& segment, myos_word_t& first) noexcept -> SysResult {
        first = 0;
        const auto size = deploy::Window::round_size(segment.memory_size);
        const auto address = reinterpret_cast<uintptr_t>(segment.file);
        if (size == 0 || address < package_address_ || address - package_address_ > package_size_
            || segment.file_size > package_size_ - (address - package_address_))
            return {.status = MYOS_STATUS_BAD_ARGS};
        if ((segment.access & MYOS_VM_WRITE) == 0) {
            const auto offset = address - package_address_;
            if (offset % 4096 != 0 || segment.file_size != size)
                return {.status = MYOS_STATUS_BAD_ARGS};
            first = offset / 4096;
            const myos_cap_attenuation view{
                .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT, .kind = MYOS_OBJECT_KIND_MEMORY,
                .size = MYOS_CAP_ATTENUATION_SIZE, .rights = MYOS_RIGHT_MAP,
                .words = {offset / 4096, size / 4096, segment.access, MYOS_VM_NORMAL}};
            auto& wire = *reinterpret_cast<uint8_t (*)[MYOS_CAP_ATTENUATION_SIZE]>(service::IpcAddress);
            deploy::attenuation::encode_wire(view, wire);
            const auto written = memory_write(buffer_->descriptor_.selector(), 0, 0, sizeof(wire));
            return written.status == MYOS_STATUS_OK
                ? cap_typed_delegate(package_, buffer_->cspace_, buffer_->descriptor_.selector()) : written;
        }
        if (count_ == MYOS_DEPLOY_TASK_MAPPING_MAX) return {.status = MYOS_STATUS_NO_MEMORY};
        auto& target = private_[count_];
        const auto pager = pager_create(buffer_->pool_, count_ + 1, 1);
        if (pager.status != MYOS_STATUS_OK) return pager;
        target.pager = cap::OwnedCap{{pager.value, 0}};
        ++count_; // Partial construction uses the same close path.
        const auto bound = pager_bind(pager.value, buffer_->events_, service::EventsBadge);
        if (bound.status != MYOS_STATUS_OK) return {.status = bound.status};
        const auto memory = memory_create_pager(pool.selector, size, segment.access,
            pager.value, MYOS_MEMORY_PAGER_PRIVATE);
        if (memory.status != MYOS_STATUS_OK) return memory;
        target.memory = cap::OwnedCap{{memory.value, 0}};
        target.segment = segment;
        return cap_duplicate(memory.value, buffer_->cspace_, MYOS_RIGHT_MAP);
    }

public:
    auto source(PageBuffer& buffer, myos_cap_t package, uintptr_t address, size_t size) noexcept -> deploy::ImageSource {
        buffer_ = &buffer;
        package_ = package; package_address_ = address; package_size_ = size;
        return {this, [](void* context, cap::CapRef pool, const boot::Segment& segment, myos_word_t& first) noexcept {
            return static_cast<Image*>(context)->create(pool, segment, first);
        }};
    }
    // Caller first closes every child. Pool revocation then cancels claims and
    // retires private objects; these local selectors do not own child lifetime.
    void close() noexcept {
        for (size_t i = 0; i < count_; ++i) {
            auto& target = private_[i];
            target.memory = {};
            if (target.pager) service::require(object_destroy(target.pager.selector()).status);
            target.pager = {};
            target.segment = {};
        }
        count_ = 0;
        package_ = 0; package_address_ = 0; package_size_ = 0;
    }
    auto poll() noexcept -> myos_status_t {
        for (size_t i = 0; i < count_; ++i) {
            auto& target = private_[i];
            const auto claimed = pager_claim(target.pager.selector());
            if (claimed.status == MYOS_STATUS_WOULD_BLOCK) continue;
            if (claimed.status != MYOS_STATUS_OK) return claimed.status;
            myos_pager_request request{};
            service::copy(&request, reinterpret_cast<void*>(service::IpcAddress), sizeof(request));
            if (request.kind != MYOS_PAGER_REQUEST_PAGE_IN || request.payload.page_in.count != 1
                || request.page_index >= deploy::Window::round_size(target.segment.memory_size) / 4096)
                return MYOS_STATUS_PEER_FAULT;
            const size_t offset = request.page_index * 4096;
            const auto available = offset < target.segment.file_size ? target.segment.file_size - offset : 0;
            const auto count = available < 4096 ? available : 4096;
            // This source fault is served by Files, below the resident process
            // service in the paging dependency graph. No child can serve it.
            if (count != 0) service::copy(buffer_->bytes_, target.segment.file + offset, count);
            for (size_t b = count; b < sizeof(buffer_->bytes_); ++b) buffer_->bytes_[b] = 0;
            auto status = memory_populate(buffer_->staging_.selector(), 0).status;
            if (status == MYOS_STATUS_OK) {
                service::copy(reinterpret_cast<void*>(service::IpcAddress), buffer_->bytes_, sizeof(buffer_->bytes_));
                status = memory_write(buffer_->staging_.selector(), 0, 0, sizeof(buffer_->bytes_)).status;
            }
            request.payload.page_in.content_epoch = 1;
            service::copy(reinterpret_cast<void*>(service::IpcAddress), &request, sizeof(request));
            if (status == MYOS_STATUS_OK)
                status = pager_supply(target.pager.selector(), target.memory.selector(), buffer_->staging_.selector(), 0).status;
            if (status != MYOS_STATUS_OK) {
                const auto failed = pager_fail(target.pager.selector(), target.memory.selector()).status;
                if (failed != MYOS_STATUS_OK) return failed;
            }
        }
        return MYOS_STATUS_OK;
    }
};
} // namespace myos::process
