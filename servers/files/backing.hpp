#pragma once

#include <servers/files/reader.hpp>
#include <user/lib/file_protocol.hpp>
#include <uapi/pager.h>

namespace myos::files {

// A mount owns one backing per immutable file, shared by all issued views.
// Only MemoryObject owns resident-page state; this record owns an in-flight
// disk read and its exact Pager claim, never a second page cache.
class Backing final {
    cap::OwnedCap pager_, memory_, staging_;
    size_t file_{};
    uint64_t size_{};
    myos_pager_request claim_{};
    uint8_t bytes_[io::BufferSize]{};
    Read read_{};

    void discard() noexcept {
        cap::OwnedCap* objects[]{&memory_, &pager_, &staging_};
        for (auto* object : objects) {
            if (*object) {
                service::require(object_destroy(object->selector()).status);
                *object = {};
            }
        }
    }
    static void completed(Read& read, myos_status_t status) noexcept {
        auto& self = *static_cast<Backing*>(read.context);
        if (status == MYOS_STATUS_OK) {
            status = memory_populate(self.staging_.selector(), 0).status;
        }
        if (status == MYOS_STATUS_OK) {
            service::copy(reinterpret_cast<void*>(service::IpcAddress), self.bytes_, sizeof(self.bytes_));
            status = memory_write(self.staging_.selector(), 0, 0, sizeof(self.bytes_)).status;
        }
        self.claim_.payload.page_in.content_epoch = 1;
        service::copy(reinterpret_cast<void*>(service::IpcAddress), &self.claim_, sizeof(self.claim_));
        if (status == MYOS_STATUS_OK)
            status = pager_supply(self.pager_.selector(), self.memory_.selector(), self.staging_.selector(), 0).status;
        if (status != MYOS_STATUS_OK)
            service::require(pager_fail(self.pager_.selector(), self.memory_.selector()).status);
        self.claim_ = {};
    }

public:
    auto open(myos_cap_t pool, myos_cap_t events, size_t file, uint64_t size) noexcept -> myos_status_t {
        if (memory_) return MYOS_STATUS_OK;
        if (size == 0) return MYOS_STATUS_BAD_ARGS;
        const auto pager = pager_create(pool, file + 1, 1);
        if (pager.status != MYOS_STATUS_OK) return pager.status;
        pager_ = cap::OwnedCap{{pager.value, 0}};
        const uint64_t rounded = (size + 4095) & ~uint64_t{4095};
        const auto memory = memory_create_pager(pool, rounded, MYOS_VM_READ | MYOS_VM_EXECUTE, pager_.selector());
        if (memory.status != MYOS_STATUS_OK) { discard(); return memory.status; }
        memory_ = cap::OwnedCap{{memory.value, 0}};
        const auto staging = memory_create(pool, 4096, MYOS_VM_READ | MYOS_VM_WRITE);
        if (staging.status != MYOS_STATUS_OK) { discard(); return staging.status; }
        staging_ = cap::OwnedCap{{staging.value, 0}};
        auto status = memory_seal(memory_.selector()).status;
        if (status == MYOS_STATUS_OK) status = pager_bind(pager_.selector(), events, service::EventsBadge).status;
        if (status != MYOS_STATUS_OK) { discard(); return status; }
        file_ = file;
        size_ = size;
        return MYOS_STATUS_OK;
    }

    auto export_view(myos_cap_t cspace, myos_cap_t descriptor, myos_word_t access,
                     io::ControlReply& reply) noexcept
        -> myos_status_t {
        const myos_word_t rights = MYOS_RIGHT_MAP | MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_DELEGATE;
        const myos_cap_attenuation view{
            .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT, .kind = MYOS_OBJECT_KIND_MEMORY,
            .size = MYOS_CAP_ATTENUATION_SIZE, .rights = rights,
            .words = {0, (size_ + 4095) / 4096, access, MYOS_VM_NORMAL}};
        auto& wire = *reinterpret_cast<uint8_t (*)[MYOS_CAP_ATTENUATION_SIZE]>(service::IpcAddress);
        deploy::attenuation::encode_wire(view, wire);
        const auto written = memory_write(descriptor, 0, 0, sizeof(wire));
        if (written.status != MYOS_STATUS_OK) return written.status;
        const auto exported = cap_typed_delegate(memory_.selector(), cspace, descriptor);
        if (exported.status != MYOS_STATUS_OK) return exported.status;
        if (!reply.offer(cap::OwnedCap{{exported.value, 0}}, rights)) return MYOS_STATUS_INTERNAL;
        reply.message.value = file_ + 1; // identity within this mount authority, never an access token
        reply.message.size = 8;
        for (size_t b = 0; b < 8; ++b) reply.message.data[b] = size_ >> (b * 8);
        return MYOS_STATUS_OK;
    }

    auto poll() noexcept -> myos_status_t {
        if (!memory_ || read_.active) return MYOS_STATUS_OK;
        const auto result = pager_claim(pager_.selector());
        if (result.status == MYOS_STATUS_WOULD_BLOCK) return MYOS_STATUS_OK;
        if (result.status != MYOS_STATUS_OK) return result.status;
        service::copy(&claim_, reinterpret_cast<void*>(service::IpcAddress), sizeof(claim_));
        if (claim_.version != MYOS_PAGER_REQUEST_VERSION || claim_.kind != MYOS_PAGER_REQUEST_PAGE_IN
            || claim_.flags != 0 || claim_.payload.page_in.count != 1
            || claim_.page_index >= (size_ + 4095) / 4096) return MYOS_STATUS_PEER_FAULT;
        for (auto& byte : bytes_) byte = 0;
        const auto offset = claim_.page_index * uint64_t{4096};
        const auto remaining = size_ - offset;
        read_ = {.file = file_, .offset = offset, .output = bytes_,
            .size = static_cast<size_t>(remaining < 4096 ? remaining : 4096),
            .context = this, .complete = completed, .active = true};
        return MYOS_STATUS_OK;
    }
    auto read() noexcept -> Read& { return read_; }
};

} // namespace myos::files
