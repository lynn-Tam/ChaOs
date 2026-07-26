#pragma once

#include <libk/sync/atomic.hpp>
#include <uapi/types.h>

//Confirmatory experiment.
// Exit condition: remove this shared-memory proof protocol when Endpoint-based
// service tests replace the Stage E construction and activation proof.
namespace myos::proof {

inline constexpr myos_word_t PageSize = 4096;
inline constexpr myos_word_t SharedAddress = 0x2000'0000;
inline constexpr myos_word_t ControlAddress = 0x2200'0000;
inline constexpr myos_word_t EventAddress = 0x2200'1000;
inline constexpr myos_word_t StackAddress = 0x2100'0000;
inline constexpr myos_word_t StackStride = 0x0001'0000;
inline constexpr myos_word_t EndpointIpcAddress = 0x2300'0000;
inline constexpr myos_word_t VprocRuntimeStride = 0x0000'2000;
inline constexpr myos_word_t TargetVproc = 0;
inline constexpr myos_word_t SourceVproc = 1;
inline constexpr myos_word_t VprocCount = 2;

inline constexpr myos_word_t NotificationSlot = 8;
inline constexpr myos_word_t VprocNotificationSlot = 9;
inline constexpr myos_word_t VprocKeySlot = 10;
inline constexpr myos_word_t VprocStateSlot = 11;
inline constexpr myos_word_t PoolSlot = 12;
inline constexpr myos_word_t CSpaceSlot = 13;
inline constexpr myos_word_t TunnelSourceStateSlot = 14;
inline constexpr myos_word_t TunnelTargetStateSlot = 15;
inline constexpr myos_word_t TunnelSourceSequenceSlot = 16;
inline constexpr myos_word_t TunnelTargetSequenceSlot = 17;
inline constexpr myos_word_t TunnelHeartbeatSlot = 18;
inline constexpr myos_word_t TunnelAdminSlot = 19;
inline constexpr myos_word_t TunnelConnectSlot = 20;
inline constexpr myos_word_t TunnelTxSlot = 21;
inline constexpr myos_word_t ParkProbeSlot = 22;
inline constexpr myos_word_t ParkObservedSlot = 23;
inline constexpr myos_word_t ParkResultSlot = 24;
inline constexpr myos_word_t ParkWakeSlot = 25;
inline constexpr myos_word_t TunnelDeliveryCountSlot = 26;
inline constexpr myos_word_t EndpointSlot = 27;
inline constexpr myos_word_t EndpointResultSlot = 28;
inline constexpr myos_word_t ChannelSenderSlot = 29;
inline constexpr myos_word_t ChannelSenderAltSlot = 30;
inline constexpr myos_word_t ChannelReceiverSlot = 31;
inline constexpr myos_word_t ChannelNotifyRSlot = 32;
inline constexpr myos_word_t ChannelNotifySSlot = 33;
inline constexpr myos_word_t ChannelRelationRSlot = 34;
inline constexpr myos_word_t ChannelRelationSSlot = 35;
inline constexpr myos_word_t ChannelReadySlot = 36;
inline constexpr myos_word_t ChannelFirstSentSlot = 37;
inline constexpr myos_word_t ChannelBlockedSlot = 38;
inline constexpr myos_word_t ChannelFirstReceivedSlot = 39;
inline constexpr myos_word_t ChannelSecondSentSlot = 40;
inline constexpr myos_word_t ChannelVprocReadySlot = 41;
inline constexpr myos_word_t ChannelVprocSentSlot = 42;
inline constexpr myos_word_t ChannelVprocDoneSlot = 43;
inline constexpr myos_word_t ChannelClosedSlot = 44;
inline constexpr myos_word_t ChannelCompleteSlot = 45;
inline constexpr myos_word_t ChannelFailureSlot = 46;
inline constexpr myos_word_t ChannelVprocGoSlot = 47;
inline constexpr myos_word_t ChannelVprocBindSlot = 48;
inline constexpr myos_word_t ChannelDrainSentSlot = 49;
inline constexpr myos_word_t ProgressEpochSlot = 50;
inline constexpr myos_word_t ChannelDrainReceivedSlot = 51;
inline constexpr myos_word_t ProgressCellBase = 56;
inline constexpr myos_word_t ProgressCellStride = 5;
inline constexpr myos_word_t ProgressActorCount = 5;
inline constexpr myos_word_t SharedWords =
    ProgressCellBase + ProgressCellStride * ProgressActorCount;

inline constexpr myos_word_t ArmDescriptorOffset = 1024;
inline constexpr myos_word_t ArmDescriptorStride = 128;
static_assert(SharedWords * sizeof(myos_word_t) <= ArmDescriptorOffset);
static_assert(
    ArmDescriptorOffset + VprocCount * ArmDescriptorStride <= PageSize);

// These are proof-local semantic checkpoints, not a second Channel/Vproc
// state machine. Each actor writes only its own cell; the coordinator reads
// cells to explain a stall without steering the protocol.
enum class ProgressActor : myos_word_t {
    Coordinator = 0,
    Sender = 1,
    Receiver = 2,
    TargetVproc = 3,
    SourceVproc = 4,
};

enum class ProgressStage : myos_word_t {
    Boot = 1,
    ChannelBound = 2,
    FirstSend = 3,
    FirstReceive = 4,
    QueueFull = 5,
    BlockingSend = 6,
    SecondReceive = 7,
    BindRequested = 8,
    BindCommitted = 9,
    ReadyPublished = 10,
    GoPublished = 11,
    VprocSend = 12,
    VprocDone = 13,
    Complete = 14,
    Failed = 15,
};

enum class ProgressWait : myos_word_t {
    None = 0,
    Notification = 1,
    Children = 2,
    ChannelReady = 3,
    FirstReceive = 4,
    Space = 5,
    ChannelBind = 6,
    VprocReady = 7,
    ChannelGo = 8,
    VprocEvent = 9,
    Tunnel = 10,
};

struct ProgressSnapshot final {
    myos_word_t generation{};
    myos_word_t sequence{};
    myos_word_t stage{};
    myos_word_t wait{};
    myos_word_t detail{};
};

inline constexpr myos_word_t NotificationBadge = 1U << 5;
inline constexpr myos_word_t VprocBadge = 1U << 6;
inline constexpr myos_word_t ChannelNotifyRBadge = 1U << 7;
inline constexpr myos_word_t ChannelNotifySBadge = 1U << 8;
inline constexpr myos_word_t ChildReady = 0x5052'4f4f'4652'554e;
inline constexpr myos_word_t VprocMagic = 0x5650'524f'4352'554e;
inline constexpr myos_word_t SourceVprocMagic = 0x5455'4e53'4f55'5243;
inline constexpr myos_word_t VprocReady = 0x5650'5245'4144'5900;
inline constexpr myos_word_t VprocComplete = 0x5650'444f'4e45'0000;
inline constexpr myos_word_t VprocNotificationIngress = 0;
inline constexpr myos_word_t VprocNotificationTag = 0x4e4f'5449'4659'0001;
inline constexpr myos_word_t TunnelIngressSlot = 1;
inline constexpr myos_word_t ChannelVprocIngress = 2;
inline constexpr myos_word_t TunnelTag = 0x5455'4e4e'454c'0001;
inline constexpr myos_word_t ChannelVprocTag = 0x4348'4e4c'5244'0001;
inline constexpr myos_word_t TunnelSourceReady = 0x5455'4e53'5244'5900;
inline constexpr myos_word_t TunnelFirstReady = 0x5455'4e46'5253'5400;
inline constexpr myos_word_t TunnelFirstInvoked = 0x5455'4e46'4952'4500;
inline constexpr myos_word_t TunnelSecondReady = 0x5455'4e53'434e'4400;
inline constexpr myos_word_t TunnelSecondInvoked = 0x5455'4e53'4947'4e00;
inline constexpr myos_word_t ParkRejected = 0x5041'524b'5245'4a00;
inline constexpr myos_word_t ParkCommitted = 0x5041'524b'574f'4b00;
inline constexpr myos_word_t EndpointMagic = 0x454e'4450'4f49'4e54;
inline constexpr myos_word_t EndpointAbortMagic = 0x454e'4441'424f'5254;
inline constexpr myos_word_t EndpointTimeoutMagic = 0x454e'4454'494d'454f;
inline constexpr myos_word_t EndpointFaultMagic = 0x454e'4446'4155'4c54;
inline constexpr myos_word_t EndpointAbortDetail = 0x4142'4f52'5400'0042;
inline constexpr myos_word_t EndpointBadge = 0x45;
inline constexpr myos_word_t EndpointResult = 0x4550'4341'4c4c'4f4b;
inline constexpr myos_word_t EndpointTransfer = 0x4550'4341'5053'4f4b;
inline constexpr myos_word_t ChannelSenderMagic = 0x4348'5345'4e44'0001;
inline constexpr myos_word_t ChannelReceiverMagic = 0x4348'5245'4356'0001;
inline constexpr myos_word_t ChannelReady = 0x4348'5244'5900'0001;
inline constexpr myos_word_t ChannelFirstSent = 0x4348'4653'454e'5401;
inline constexpr myos_word_t ChannelBlocked = 0x4348'424c'4f43'4b01;
inline constexpr myos_word_t ChannelFirstReceived = 0x4348'4652'4543'5601;
inline constexpr myos_word_t ChannelSecondSent = 0x4348'5353'454e'5401;
inline constexpr myos_word_t ChannelVprocReady = 0x4348'5652'4541'4401;
inline constexpr myos_word_t ChannelVprocSent = 0x4348'5653'454e'5401;
inline constexpr myos_word_t ChannelVprocDone = 0x4348'5644'4f4e'4501;
inline constexpr myos_word_t ChannelClosed = 0x4348'434c'4f53'4501;
inline constexpr myos_word_t ChannelComplete = 0x4348'434f'4d50'4c01;
inline constexpr myos_word_t ChannelFailure = 0x4348'4641'494c'0001;
inline constexpr myos_word_t ChannelVprocIpcAddress = 0x2400'0000;

// Both proof ELFs share this page across harts. Release publication and
// acquire observation are part of the protocol; volatile is not synchronization
// on RISC-V. Heartbeat is deliberately only a relaxed liveness observation.
class Shared final {
public:
    Shared() noexcept = default;
    explicit Shared(myos_word_t address) noexcept
        : words_(reinterpret_cast<myos_word_t*>(address)) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return words_ != nullptr;
    }

    void bind(myos_word_t address) noexcept {
        words_ = reinterpret_cast<myos_word_t*>(address);
    }

    [[nodiscard]] auto load(myos_word_t slot) const noexcept -> myos_word_t {
        return libk::AtomicRef{words_[slot]}
            .load<libk::MemoryOrder::Acquire>();
    }

    void store(myos_word_t slot, myos_word_t value) const noexcept {
        libk::AtomicRef{words_[slot]}
            .store<libk::MemoryOrder::Release>(value);
    }

    void progress(
        ProgressActor actor,
        ProgressStage stage,
        ProgressWait wait = ProgressWait::None,
        myos_word_t detail = 0,
        myos_word_t generation = 1) const noexcept {
        const myos_word_t index = static_cast<myos_word_t>(actor);
        if (index >= ProgressActorCount) {
            return;
        }
        const myos_word_t base = ProgressCellBase
            + index * ProgressCellStride;
        auto sequence = libk::AtomicRef{words_[base]}
            .load<libk::MemoryOrder::Relaxed>();
        if ((sequence & 1U) != 0
            || sequence > static_cast<myos_word_t>(-1) - 2) {
            return;
        }
        libk::AtomicRef{words_[base]}
            .store<libk::MemoryOrder::Release>(sequence + 1);
        libk::AtomicRef{words_[base + 1]}
            .store<libk::MemoryOrder::Relaxed>(generation);
        libk::AtomicRef{words_[base + 2]}
            .store<libk::MemoryOrder::Relaxed>(
                static_cast<myos_word_t>(stage));
        libk::AtomicRef{words_[base + 3]}
            .store<libk::MemoryOrder::Relaxed>(
                static_cast<myos_word_t>(wait));
        libk::AtomicRef{words_[base + 4]}
            .store<libk::MemoryOrder::Relaxed>(detail);
        libk::AtomicRef{words_[base]}
            .store<libk::MemoryOrder::Release>(sequence + 2);
        (void)libk::AtomicRef{words_[ProgressEpochSlot]}
            .fetch_add<libk::MemoryOrder::Release>(1);
    }

    [[nodiscard]] auto progress_read(
        ProgressActor actor,
        ProgressSnapshot& snapshot) const noexcept -> bool {
        const myos_word_t index = static_cast<myos_word_t>(actor);
        if (index >= ProgressActorCount) {
            return false;
        }
        const myos_word_t base = ProgressCellBase
            + index * ProgressCellStride;
        for (myos_word_t attempt = 0; attempt < 3; ++attempt) {
            const auto first = libk::AtomicRef{words_[base]}
                .load<libk::MemoryOrder::Acquire>();
            if ((first & 1U) != 0) {
                continue;
            }
            snapshot.generation = libk::AtomicRef{words_[base + 1]}
                .load<libk::MemoryOrder::Acquire>();
            snapshot.stage = libk::AtomicRef{words_[base + 2]}
                .load<libk::MemoryOrder::Acquire>();
            snapshot.wait = libk::AtomicRef{words_[base + 3]}
                .load<libk::MemoryOrder::Acquire>();
            snapshot.detail = libk::AtomicRef{words_[base + 4]}
                .load<libk::MemoryOrder::Acquire>();
            const auto second = libk::AtomicRef{words_[base]}
                .load<libk::MemoryOrder::Acquire>();
            if (first == second && (second & 1U) == 0) {
                snapshot.sequence = second;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto add_relaxed(
        myos_word_t slot,
        myos_word_t value = 1) const noexcept -> myos_word_t {
        return libk::AtomicRef{words_[slot]}
            .fetch_add<libk::MemoryOrder::Relaxed>(value) + value;
    }

    [[nodiscard]] auto add_release(
        myos_word_t slot,
        myos_word_t value = 1) const noexcept -> myos_word_t {
        return libk::AtomicRef{words_[slot]}
            .fetch_add<libk::MemoryOrder::Release>(value) + value;
    }

private:
    myos_word_t* words_{};
};

} // namespace myos::proof
