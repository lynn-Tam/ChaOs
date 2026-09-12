#include <libk/shared_ring.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

using Ring = libk::SharedRing<4, 32>;
using Result = libk::RingResult;

auto batching() -> bool {
    Ring::Data data{};
    Ring::Cursor cursor{};
    Ring::Producer producer{data, cursor};
    Ring::Consumer consumer{data, cursor};
    Ring::Entry entry{};
    for (uint64_t round = 0; round < 100; ++round) {
        for (uint64_t i = 0; i < 32; ++i) {
            if (producer.push({{round * 32 + i, i, 42, 77}}) != Result::Ready)
                return false;
        }
        if (consumer.pop(entry) != Result::Empty) return false;
        if (producer.push(entry) != Result::Full) return false;
        if (!producer.publish() || producer.publish()) return false;
        for (uint64_t i = 0; i < 32; ++i) {
            if (consumer.pop(entry) != Result::Ready
                || entry[0] != round * 32 + i || entry[1] != i
                || entry[2] != 42 || entry[3] != 77) return false;
        }
        if (consumer.pop(entry) != Result::Empty) return false;
        if (producer.push(entry) != Result::Full) return false;
        if (!consumer.release() || consumer.release()) return false;
    }
    return producer.refresh() && producer.acknowledged() == 3200;
}

auto invalid_peer() -> bool {
    {
        Ring::Data data{};
        Ring::Cursor cursor{};
        Ring::Producer producer{data, cursor};
        // Even a staged descriptor cannot be acknowledged before publication.
        if (producer.push({{1, 2, 3, 4}}) != Result::Ready) return false;
        cursor.released.store<libk::MemoryOrder::Release>(1);
        if (producer.refresh() || producer.publish()) return false;
        cursor.released.store<libk::MemoryOrder::Release>(0);
        if (producer.push({{}}) != Result::InvalidPeer) return false;
    }
    {
        Ring::Data data{};
        Ring::Cursor cursor{};
        Ring::Consumer consumer{data, cursor};
        Ring::Entry entry{};
        data.published.store<libk::MemoryOrder::Release>(UINT64_MAX);
        if (consumer.pop(entry) != Result::InvalidPeer) return false;
        data.published.store<libk::MemoryOrder::Release>(0);
        if (consumer.pop(entry) != Result::InvalidPeer) return false;
    }
    {
        Ring::Data data{};
        Ring::Cursor cursor{};
        Ring::Producer producer{data, cursor};
        Ring::Consumer consumer{data, cursor};
        Ring::Entry entry{};
        if (producer.push({{}}) != Result::Ready || !producer.publish()
            || consumer.pop(entry) != Result::Ready || !consumer.release()
            || !producer.refresh()) return false;
        cursor.released.store<libk::MemoryOrder::Release>(0);
        if (producer.refresh()) return false;
    }
    {
        Ring::Data data{};
        Ring::Cursor cursor{};
        Ring::Consumer consumer{data, cursor};
        Ring::Entry entry{};
        data.published.store<libk::MemoryOrder::Release>(2);
        if (consumer.pop(entry) != Result::Ready) return false;
        data.published.store<libk::MemoryOrder::Release>(1);
        if (consumer.pop(entry) != Result::InvalidPeer) return false;
    }
    return true;
}

auto concurrent() -> bool {
    constexpr uint64_t count = 200000;
    Ring::Data data[2]{};
    Ring::Cursor cursor[2]{};
    std::atomic<bool> failed{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    const auto stopped = [&] {
        if (std::chrono::steady_clock::now() >= deadline)
            failed.store(true, std::memory_order_relaxed);
        return failed.load(std::memory_order_relaxed);
    };
    const auto produce = [&](size_t lane) {
        Ring::Producer producer{data[lane], cursor[lane]};
        for (uint64_t i = 0; i < count && !stopped();) {
            const auto result = producer.push({{i, ~i, i * 31, lane}});
            if (result == Result::Ready) {
                if (++i % 7 == 0) static_cast<void>(producer.publish());
            } else if (result == Result::Full) {
                static_cast<void>(producer.publish());
                std::this_thread::yield();
            } else {
                failed.store(true, std::memory_order_relaxed);
            }
        }
        static_cast<void>(producer.publish());
    };
    const auto consume = [&](size_t lane) {
        Ring::Consumer consumer{data[lane], cursor[lane]};
        for (uint64_t i = 0; i < count && !stopped();) {
            Ring::Entry entry{};
            const auto result = consumer.pop(entry);
            if (result == Result::Ready) {
                if (entry[0] != i || entry[1] != ~i || entry[2] != i * 31
                    || entry[3] != lane) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
                if (++i % 11 == 0) static_cast<void>(consumer.release());
            } else if (result == Result::Empty) {
                static_cast<void>(consumer.release());
                std::this_thread::yield();
            } else {
                failed.store(true, std::memory_order_relaxed);
            }
        }
        static_cast<void>(consumer.release());
    };
    std::thread p0{produce, 0}, p1{produce, 1};
    std::thread c0{consume, 0}, c1{consume, 1};
    p0.join(); p1.join(); c0.join(); c1.join();
    return !failed.load(std::memory_order_relaxed);
}

} // namespace

int main() {
    unsigned failed = 0;
    const auto check = [&](const char* name, bool result) {
        if (!result) {
            std::fprintf(stderr, "FAIL shared-ring: %s\n", name);
            ++failed;
        }
    };
    check("batch publication and backpressure", batching());
    check("untrusted cursor bounds and rollback", invalid_peer());
    check("independent concurrent lanes", concurrent());
    std::printf("shared-ring: %u passed, %u failed\n", 3 - failed, failed);
    return failed == 0 ? 0 : 1;
}
