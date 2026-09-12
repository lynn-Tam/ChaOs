#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/array.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>

namespace libk {

enum class RingResult : uint8_t {
    Ready,
    Full,
    Empty,
    InvalidPeer,
    Exhausted,
};

// One producer and one consumer per lane. Data belongs to the producer;
// Cursor belongs to the consumer. Map each peer's part read-only. Endpoints
// start on fresh zeroed storage and cannot be reconstructed on a live lane.
// A new session supplies new storage, never resets a live sequence.
template<size_t Words, size_t Capacity>
    requires(Words > 0 && Capacity > 0 && (Capacity & (Capacity - 1)) == 0)
struct SharedRing final {
    using Entry = Array<uint64_t, Words>;

    struct alignas(64) Data final {
        Atomic<uint64_t> published{};
        alignas(64) Atomic<uint64_t> entries[Capacity][Words]{};
    };

    struct alignas(64) Cursor final {
        Atomic<uint64_t> released{};
    };

    class Producer final : private noncopyable_nonmovable {
    public:
        Producer(Data& data, const Cursor& cursor) noexcept
            : data_(data), cursor_(cursor) {}

        [[nodiscard]] auto push(const Entry& entry) noexcept -> RingResult {
            if (!refresh()) return RingResult::InvalidPeer;
            if (next_ == UINT64_MAX) return RingResult::Exhausted;
            if (next_ - released_ == Capacity) return RingResult::Full;
            auto& destination = data_.entries[next_ & (Capacity - 1)];
            for (size_t word = 0; word < Words; ++word) {
                destination[word].template store<MemoryOrder::Relaxed>(entry[word]);
            }
            ++next_;
            return RingResult::Ready;
        }

        // Publish the complete batch before signaling the peer. Signal once
        // for every nonempty publication: empty-transition guesses alone can
        // lose a wake when the peer concurrently drains and waits.
        [[nodiscard]] auto publish() noexcept -> bool {
            if (failed_ || published_ == next_) return false;
            data_.published.template store<MemoryOrder::Release>(next_);
            published_ = next_;
            return true;
        }

        [[nodiscard]] auto refresh() noexcept -> bool {
            if (failed_) return false;
            const auto released = cursor_.released.template load<MemoryOrder::Acquire>();
            // A peer cannot acknowledge an entry that we have not published.
            if (released < released_ || released > published_) {
                failed_ = true;
                return false;
            }
            released_ = released;
            return true;
        }

        [[nodiscard]] auto produced() const noexcept -> uint64_t { return next_; }
        [[nodiscard]] auto acknowledged() const noexcept -> uint64_t { return released_; }

    private:
        Data& data_;
        const Cursor& cursor_;
        uint64_t next_{};
        uint64_t published_{};
        uint64_t released_{};
        bool failed_{};
    };

    class Consumer final : private noncopyable_nonmovable {
    public:
        Consumer(const Data& data, Cursor& cursor) noexcept
            : data_(data), cursor_(cursor) {}

        [[nodiscard]] auto pop(Entry& entry) noexcept -> RingResult {
            if (failed_) return RingResult::InvalidPeer;
            const auto published = data_.published.template load<MemoryOrder::Acquire>();
            if (published < published_ || published < next_
                || published - released_ > Capacity) {
                failed_ = true;
                return RingResult::InvalidPeer;
            }
            published_ = published;
            if (next_ == published) return RingResult::Empty;
            const auto& source = data_.entries[next_ & (Capacity - 1)];
            // Atomic words also make hostile concurrent descriptor mutation
            // a defined snapshot. The service must validate this local copy;
            // the ring does not promise a coherent message from a hostile peer.
            for (size_t word = 0; word < Words; ++word) {
                entry[word] = source[word].template load<MemoryOrder::Relaxed>();
            }
            ++next_;
            return RingResult::Ready;
        }

        // Release only after all popped descriptors have been copied into
        // locally owned request state. This acknowledges ring storage, not I/O.
        [[nodiscard]] auto release() noexcept -> bool {
            if (failed_ || released_ == next_) return false;
            cursor_.released.template store<MemoryOrder::Release>(next_);
            released_ = next_;
            return true;
        }

        [[nodiscard]] auto consumed() const noexcept -> uint64_t { return next_; }

    private:
        const Data& data_;
        Cursor& cursor_;
        uint64_t next_{};
        uint64_t published_{};
        uint64_t released_{};
        bool failed_{};
    };
};

} // namespace libk
