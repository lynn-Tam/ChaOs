#pragma once

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
inline constexpr myos_word_t SharedWords = 32;

inline constexpr myos_word_t ArmDescriptorOffset = 512;
inline constexpr myos_word_t ArmDescriptorStride = 128;

inline constexpr myos_word_t NotificationBadge = 1U << 5;
inline constexpr myos_word_t VprocBadge = 1U << 6;
inline constexpr myos_word_t ChildReady = 0x5052'4f4f'4652'554e;
inline constexpr myos_word_t VprocMagic = 0x5650'524f'4352'554e;
inline constexpr myos_word_t SourceVprocMagic = 0x5455'4e53'4f55'5243;
inline constexpr myos_word_t VprocReady = 0x5650'5245'4144'5900;
inline constexpr myos_word_t VprocComplete = 0x5650'444f'4e45'0000;
inline constexpr myos_word_t VprocNotificationIngress = 0;
inline constexpr myos_word_t VprocNotificationTag = 0x4e4f'5449'4659'0001;
inline constexpr myos_word_t TunnelIngressSlot = 1;
inline constexpr myos_word_t TunnelTag = 0x5455'4e4e'454c'0001;
inline constexpr myos_word_t TunnelSourceReady = 0x5455'4e53'5244'5900;
inline constexpr myos_word_t TunnelFirstReady = 0x5455'4e46'5253'5400;
inline constexpr myos_word_t TunnelFirstInvoked = 0x5455'4e46'4952'4500;
inline constexpr myos_word_t TunnelSecondReady = 0x5455'4e53'434e'4400;
inline constexpr myos_word_t TunnelSecondInvoked = 0x5455'4e53'4947'4e00;
inline constexpr myos_word_t ParkRejected = 0x5041'524b'5245'4a00;
inline constexpr myos_word_t ParkCommitted = 0x5041'524b'574f'4b00;
inline constexpr myos_word_t EndpointMagic = 0x454e'4450'4f49'4e54;
inline constexpr myos_word_t EndpointBadge = 0x45;
inline constexpr myos_word_t EndpointResult = 0x4550'4341'4c4c'4f4b;
inline constexpr myos_word_t EndpointTransfer = 0x4550'4341'5053'4f4b;

} // namespace myos::proof
