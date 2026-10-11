#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <libk/noncopyable.hpp>
#include <atomic>

namespace libk {

static_assert(std::atomic<uint64_t>::is_always_lock_free
    && sizeof(std::atomic<uint64_t>) == sizeof(uint64_t));

enum class ring_status : uint8_t {
    ready,
    full,
    empty,
    invalid_peer,
    exhausted,
};

// One producer and one consumer per lane. data belongs to the producer;
// cursor belongs to the consumer. Map each peer's part read-only. Endpoints
// start on fresh zeroed storage and cannot be reconstructed on a live lane.
// A new session supplies new storage, never resets a live sequence.
template<size_t Words, size_t Capacity>
    requires(Words > 0 && Capacity > 0 && (Capacity & (Capacity - 1)) == 0)
struct shared_ring final {
    using value_type = std::array<uint64_t, Words>;

    struct alignas(64) data final {
        std::atomic<uint64_t> published{};
        alignas(64) std::atomic<uint64_t> entries[Capacity][Words]{};
    };

    struct alignas(64) cursor final {
        std::atomic<uint64_t> released{};
    };

    class producer final : private noncopyable_nonmovable {
    public:
        producer(data& data, const cursor& cursor) noexcept
            : data_(data), cursor_(cursor) {}

        [[nodiscard]] auto push(const value_type& entry) noexcept -> ring_status {
            if (!refresh()) return ring_status::invalid_peer;
            if (next_ == UINT64_MAX) return ring_status::exhausted;
            if (next_ - released_ == Capacity) return ring_status::full;
            auto& destination = data_.entries[next_ & (Capacity - 1)];
            for (size_t word = 0; word < Words; ++word) {
                destination[word].store(entry[word], std::memory_order_relaxed);
            }
            ++next_;
            return ring_status::ready;
        }

        // Publish the complete batch before signaling the peer. Signal once
        // for every nonempty publication: empty-transition guesses alone can
        // lose a wake when the peer concurrently drains and waits.
        [[nodiscard]] auto publish() noexcept -> bool {
            if (failed_ || published_ == next_) return false;
            data_.published.store(next_, std::memory_order_release);
            published_ = next_;
            return true;
        }

        [[nodiscard]] auto refresh() noexcept -> bool {
            if (failed_) return false;
            const auto released = cursor_.released.load(std::memory_order_acquire);
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
        data& data_;
        const cursor& cursor_;
        uint64_t next_{};
        uint64_t published_{};
        uint64_t released_{};
        bool failed_{};
    };

    class consumer final : private noncopyable_nonmovable {
    public:
        consumer(const data& data, cursor& cursor) noexcept
            : data_(data), cursor_(cursor) {}

        [[nodiscard]] auto pop(value_type& entry) noexcept -> ring_status {
            if (failed_) return ring_status::invalid_peer;
            const auto published = data_.published.load(std::memory_order_acquire);
            if (published < published_ || published < next_
                || published - released_ > Capacity) {
                failed_ = true;
                return ring_status::invalid_peer;
            }
            published_ = published;
            if (next_ == published) return ring_status::empty;
            const auto& source = data_.entries[next_ & (Capacity - 1)];
            // Atomic words also make hostile concurrent descriptor mutation
            // a defined snapshot. The service must validate this local copy;
            // the ring does not promise a coherent message from a hostile peer.
            for (size_t word = 0; word < Words; ++word) {
                entry[word] = source[word].load(std::memory_order_relaxed);
            }
            ++next_;
            return ring_status::ready;
        }

        // Release only after all popped descriptors have been copied into
        // locally owned request state. This acknowledges ring storage, not I/O.
        [[nodiscard]] auto release() noexcept -> bool {
            if (failed_ || released_ == next_) return false;
            cursor_.released.store(next_, std::memory_order_release);
            released_ = next_;
            return true;
        }

        [[nodiscard]] auto consumed() const noexcept -> uint64_t { return next_; }

    private:
        const data& data_;
        cursor& cursor_;
        uint64_t next_{};
        uint64_t published_{};
        uint64_t released_{};
        bool failed_{};
    };
};

} // namespace libk
