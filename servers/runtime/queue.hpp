#pragma once

#include <optional>
#include <utility>


#include <sys/queue.hpp>

namespace myos::io {
class ServerMemory final {
public:
    [[nodiscard]] auto create(myos_cap_t pool, myos_cap_t vspace, uintptr_t address) noexcept
        -> myos_status_t {
        if (mappings_[0].memory) return MYOS_STATUS_OK;
        constexpr size_t sizes[] = {4096, 4096, PayloadSize};
        for (size_t index = 0; index < 3; ++index) {
            auto memory = MappedMemory::create(pool, vspace, address + index * 4096, sizes[index]);
            if (!memory) return memory.error();
            mappings_[index] = std::move(*memory);
            // Storage roots must not fault for queue/payload storage while
            // completing a downstream request under memory pressure.
            for (size_t offset = 0; offset < sizes[index]; offset += 4096)
                *reinterpret_cast<volatile uint8_t*>(mappings_[index].address + offset) = 0;
        }
        return MYOS_STATUS_OK;
    }

    void initialize() noexcept {
        libk::construct_at(reinterpret_cast<ClientPage*>(mappings_[0].address));
        libk::construct_at(reinterpret_cast<ServerPage*>(mappings_[1].address));
        // A new client must not observe its predecessor's payload bytes.
        auto* bytes = payload();
        for (size_t index = 0; index < PayloadSize; ++index) bytes[index] = 0;
    }

    [[nodiscard]] auto export_pages(myos_cap_t cspace, cap::OwnedCap (&exports)[3],
        bool writable_payload = false) noexcept
        -> myos_status_t {
        // The tail is padding, outside both ring objects. This is private
        // writable descriptor scratch; peers can only read this page.
        constexpr size_t scratch = 4096 - MYOS_CAP_ATTENUATION_SIZE;
        static_assert(offsetof(ServerPage, submissions) + sizeof(Submissions::Cursor) <= scratch);
        auto& wire = *reinterpret_cast<uint8_t (*)[MYOS_CAP_ATTENUATION_SIZE]>(
            mappings_[1].address + scratch);
        for (size_t index = 0; index < 3; ++index) {
            const myos_cap_attenuation view{
                .version = MYOS_CAP_ATTENUATION_VERSION_CURRENT,
                .kind = MYOS_OBJECT_KIND_MEMORY,
                .size = MYOS_CAP_ATTENUATION_SIZE,
                .rights = MYOS_RIGHT_MAP | MYOS_RIGHT_DUPLICATE | MYOS_RIGHT_REVOKE,
                .words = {0, mappings_[index].size / 4096,
                    index == 0 || (index == 2 && writable_payload)
                        ? MYOS_VM_READ | MYOS_VM_WRITE : MYOS_VM_READ, MYOS_VM_NORMAL}};
            myos::cap::encode(view, wire);
            const auto exported = cap_typed_delegate(mappings_[index].memory.selector(), cspace,
                mappings_[1].memory.selector(), scratch);
            if (exported.status != MYOS_STATUS_OK) return exported.status;
            exports[index] = cap::OwnedCap{{exported.value, 0}};
        }
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto client() const noexcept -> const ClientPage& {
        return *reinterpret_cast<const ClientPage*>(mappings_[0].address);
    }
    [[nodiscard]] auto server() noexcept -> ServerPage& {
        return *reinterpret_cast<ServerPage*>(mappings_[1].address);
    }
    [[nodiscard]] auto payload() noexcept -> uint8_t* {
        return reinterpret_cast<uint8_t*>(mappings_[2].address);
    }

private:
    MappedMemory mappings_[3]{};
};

class ServerSession final : private libk::noncopyable_nonmovable {
public:
    [[nodiscard]] auto prepare(myos_cap_t pool, myos_cap_t vspace,
        myos_cap_t cspace, uintptr_t address,
        bool writable_payload = false) noexcept -> myos_status_t {
        cspace_ = cspace;
        writable_payload_ = writable_payload;
        return memory_.create(pool, vspace, address);
    }
    [[nodiscard]] auto bind(myos_cap_t channel, myos_cap_t events, uint64_t value = 0) noexcept -> myos_status_t {
        events_ = events;
        value_ = value;
        return control_.open(channel, events);
    }
    [[nodiscard]] auto open(myos_cap_t channel, myos_cap_t events, myos_cap_t pool,
        myos_cap_t vspace, myos_cap_t cspace, uintptr_t address, uint64_t value = 0) noexcept
        -> myos_status_t {
        const auto status = prepare(pool, vspace, cspace, address);
        return status == MYOS_STATUS_OK ? bind(channel, events, value) : status;
    }

    void abort() noexcept {
        closing_ = true;
        if (queue_) queue_->stop();
        peer_event_ = {};
        reply_.reset();
    }
    [[nodiscard]] auto done() const noexcept -> bool {
        if (!closing_ || reply_ || (queue_ && queue_->active() != 0)) return false;
        for (const auto& exported : exports_) if (exported) return false;
        return true;
    }
    void reset() noexcept {
        if (!done()) cap::SyscallBackend::ownership_fault(MYOS_STATUS_BUSY);
        queue_.reset();
        peer_event_ = {};
        closing_ = false;
    }

    template<class Handler>
    [[nodiscard]] auto poll(Handler&& application) noexcept -> myos_status_t {
        if (reply_) return MYOS_STATUS_OK;
        ControlPacket packet;
        const auto status = control_.receive(packet);
        if (status == MYOS_STATUS_WOULD_BLOCK || status == MYOS_STATUS_BUSY) return MYOS_STATUS_OK;
        if (status != MYOS_STATUS_OK) return status;
        return accept(packet, std::forward<Handler>(application));
    }

    template<class Handler>
    [[nodiscard]] auto accept(ControlPacket& packet, Handler&& application) noexcept -> myos_status_t {
        if (reply_) return MYOS_STATUS_BUSY;
        const auto& message = packet.message;
        reply_.emplace();
        reply_->message = ControlMessage{.operation = message.operation, .id = message.id};
        if (closing_) reply_->message.status = MYOS_STATUS_CLOSED;
        else if (message.operation == static_cast<uint64_t>(Control::Open)) {
            if (queue_ || packet.count != 1 || message.value != QueueDepth || message.size != 0)
                reply_->message.status = MYOS_STATUS_BAD_ARGS;
            else {
                memory_.initialize();
                auto created = memory_.export_pages(cspace_, exports_, writable_payload_);
                if (created != MYOS_STATUS_OK) return created;
                for (auto& exported : exports_) {
                    const auto copied = cap_duplicate(exported.selector(), cspace_, MYOS_RIGHT_MAP | MYOS_RIGHT_DUPLICATE);
                    if (copied.status != MYOS_STATUS_OK) return copied.status;
                    if (!reply_->offer(cap::OwnedCap{{copied.value, 0}}, MYOS_RIGHT_MAP)) return MYOS_STATUS_INTERNAL;
                }
                const auto signal = cap_duplicate(events_, cspace_, MYOS_RIGHT_SIGNAL | MYOS_RIGHT_DUPLICATE);
                if (signal.status != MYOS_STATUS_OK) return signal.status;
                if (!reply_->offer(cap::OwnedCap{{signal.value, 0}}, MYOS_RIGHT_SIGNAL)) return MYOS_STATUS_INTERNAL;
                peer_event_ = std::move(packet.capabilities[0]);
                queue_.emplace(memory_.client(), memory_.server());
                reply_->message.value = value_;
            }
        } else if (packet.count != 0 || !queue_) reply_->message.status = MYOS_STATUS_BAD_ARGS;
        else if (message.operation == static_cast<uint64_t>(Control::Cancel)) {
            Ticket ticket{};
            reply_->message.status = message.size != 0 ? MYOS_STATUS_BAD_ARGS
                : queue_->cancel(message.value, ticket);
        } else if (message.operation == static_cast<uint64_t>(Control::Close)) {
            if (message.size != 0) reply_->message.status = MYOS_STATUS_BAD_ARGS;
            else {
                queue_->stop();
                closing_ = true;
            }
        } else application(message, *reply_);
        return MYOS_STATUS_OK;
    }

    [[nodiscard]] auto queue() noexcept -> ServerQueue* { return queue_ ? &*queue_ : nullptr; }
    [[nodiscard]] auto payload() noexcept -> uint8_t* { return memory_.payload(); }
    [[nodiscard]] auto closing() const noexcept -> bool { return closing_; }
    [[nodiscard]] auto failed() const noexcept -> bool { return closing_ && !peer_event_; }
    [[nodiscard]] auto arm() noexcept -> myos_status_t { return control_.arm(); }

    [[nodiscard]] auto flush(bool close_ready) noexcept -> myos_status_t {
        if (queue_) {
            // A visible completion also returns its submission credit, so a
            // client can immediately refill a completed batch.
            (void)queue_->release();
            const bool completed = queue_->publish();
            if (completed && peer_event_) {
                const auto status = notification_signal(peer_event_.selector()).status;
                // The peer may revoke its notification while a borrowed I/O
                // completion is still in flight. Finish the local session.
                if (status == MYOS_STATUS_BUSY || status == MYOS_STATUS_CLOSED
                    || status == MYOS_STATUS_INVALID_CAP) abort();
                else if (status != MYOS_STATUS_OK) return status;
            }
        }
        if (closing_ && close_ready && (!queue_ || queue_->active() == 0)) {
            // This is the observable end of peer access, before Close's reply.
            // The slot's own mappings use their root grants and remain resident.
            for (auto& exported : exports_) if (exported) {
                const auto status = cap_revoke(exported.selector(), true).status;
                if (status != MYOS_STATUS_OK) return status;
                exported = {};
            }
        }
        if (!reply_ || (closing_ && (!close_ready || queue_->active() != 0))) return MYOS_STATUS_OK;
        myos_cap_transfer transfers[4]{};
        for (size_t index = 0; index < reply_->count; ++index)
            transfers[index] = {reply_->capabilities[index].selector(), reply_->rights[index], MYOS_CAP_COPY, 0};
        const auto status = control_.send(reply_->message, transfers, reply_->count);
        if (status == MYOS_STATUS_OK) reply_.reset();
        return status == MYOS_STATUS_WOULD_BLOCK || status == MYOS_STATUS_BUSY ? MYOS_STATUS_OK : status;
    }

private:
    ControlPort control_;
    ServerMemory memory_;
    std::optional<ServerQueue> queue_;
    cap::OwnedCap peer_event_;
    cap::OwnedCap exports_[3];
    std::optional<ControlReply> reply_;
    myos_cap_t events_{}, cspace_{};
    uint64_t value_{};
    bool closing_{};
    bool writable_payload_{};
};

} // namespace myos::io
