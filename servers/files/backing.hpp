#include <servers/runtime/service.hpp>
#pragma once

#include <sys/queue.hpp>
#include <sys/storage.hpp>
#include <uapi/mem.h>

namespace sys::files {

// A mount owns one backing per immutable file, shared by all issued views.
// Only MemoryObject owns resident-page state; this record owns an in-flight
// disk read and its exact Pager claim, never a second page cache.
class Backing final {
    cap::OwnedCap pager_, memory_, staging_;
    size_t file_{};
    uint64_t size_{};
    PagerReq claim_{};
    uint8_t bytes_[io::BufferSize]{};
    io::read read_{};

    void discard() noexcept {
        cap::OwnedCap* objects[]{&memory_, &pager_, &staging_};
        for (auto* object : objects) {
            if (*object) {
                service::require(object_destroy(object->selector()).status);
                *object = {};
            }
        }
    }
    static void completed(io::read& read, status_t status) noexcept {
        auto& self = *static_cast<Backing*>(read.context);
        if (status == STATUS_OK) {
            status = memory_populate(self.staging_.selector(), 0).status;
        }
        if (status == STATUS_OK) {
            service::copy(reinterpret_cast<void*>(service::IpcAddress), self.bytes_, sizeof(self.bytes_));
            status = memory_write(self.staging_.selector(), 0, 0, sizeof(self.bytes_)).status;
        }
        if (status == STATUS_OK)
            status = pager_supply(self.pager_.selector(), self.memory_.selector(), self.staging_.selector(), 0, self.claim_.id).status;
        if (status != STATUS_OK)
            service::require(pager_fail(self.pager_.selector(), self.memory_.selector(), self.claim_.id).status);
        self.claim_ = {};
    }

public:
    auto open(cap_t pool, cap_t events, size_t file, uint64_t size) noexcept -> status_t {
        if (memory_) return STATUS_OK;
        if (size == 0) return STATUS_BAD_ARGS;
        const auto pager = pager_create(pool);
        if (pager.status != STATUS_OK) return pager.status;
        pager_ = cap::OwnedCap{{pager.value, 0}};
        const uint64_t rounded = (size + 4095) & ~uint64_t{4095};
        const auto memory = memory_create_pager(pool, rounded, VM_READ | VM_EXECUTE, pager_.selector());
        if (memory.status != STATUS_OK) { discard(); return memory.status; }
        memory_ = cap::OwnedCap{{memory.value, 0}};
        const auto staging = memory_create(pool, 4096, VM_READ | VM_WRITE);
        if (staging.status != STATUS_OK) { discard(); return staging.status; }
        staging_ = cap::OwnedCap{{staging.value, 0}};
        auto status = memory_seal(memory_.selector()).status;
        if (status == STATUS_OK) status = pager_bind(pager_.selector(), events, service::EventsBadge).status;
        if (status != STATUS_OK) { discard(); return status; }
        file_ = file;
        size_ = size;
        return STATUS_OK;
    }

    auto export_view(cap_t cspace, cap_t descriptor, word_t access,
                     io::ControlReply& reply) noexcept
        -> status_t {
        const word_t rights = RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_DELEGATE;
        const CapView view{
            .version = CAP_ATTENUATION_VERSION_CURRENT, .kind = OBJECT_KIND_MEMORY,
            .size = CAP_ATTENUATION_SIZE, .rights = rights,
            .words = {0, (size_ + 4095) / 4096, access}};
        auto& wire = *reinterpret_cast<uint8_t (*)[CAP_ATTENUATION_SIZE]>(service::IpcAddress);
        sys::cap::encode(view, wire);
        const auto written = memory_write(descriptor, 0, 0, sizeof(wire));
        if (written.status != STATUS_OK) return written.status;
        const auto exported = cap_typed_delegate(memory_.selector(), cspace, descriptor);
        if (exported.status != STATUS_OK) return exported.status;
        if (!reply.offer(cap::OwnedCap{{exported.value, 0}}, rights)) return STATUS_INTERNAL;
        reply.message.value = file_ + 1; // identity within this mount authority, never an access token
        reply.message.size = 8;
        for (size_t b = 0; b < 8; ++b) reply.message.data[b] = size_ >> (b * 8);
        return STATUS_OK;
    }

    auto poll() noexcept -> status_t {
        if (!memory_ || read_.active) return STATUS_OK;
        const auto result = pager_claim(pager_.selector());
        if (result.status == STATUS_WOULD_BLOCK) return STATUS_OK;
        if (result.status != STATUS_OK) return result.status;
        service::copy(&claim_, reinterpret_cast<void*>(service::IpcAddress), sizeof(claim_));
        if (!claim_.id || claim_.kind != PAGER_REQUEST_PAGE_IN
            || claim_.payload.page_in.count != 1
            || claim_.page_index >= (size_ + 4095) / 4096) return STATUS_PEER_FAULT;
        for (auto& byte : bytes_) byte = 0;
        const auto offset = claim_.page_index * uint64_t{4096};
        const auto remaining = size_ - offset;
        read_ = {.object = file_, .offset = offset, .output = bytes_,
            .size = static_cast<size_t>(remaining < 4096 ? remaining : 4096),
            .context = this, .complete = completed, .active = true};
        return STATUS_OK;
    }
    auto read() noexcept -> io::read& { return read_; }
};

} // namespace sys::files
