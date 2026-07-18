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
inline constexpr myos_word_t VprocCookie = 0xc001'cafe;
inline constexpr myos_word_t TunnelIngressSlot = 1;
inline constexpr myos_word_t TunnelTag = 0x5455'4e4e'454c'0001;
inline constexpr myos_word_t TunnelSourceReady = 0x5455'4e53'5244'5900;
inline constexpr myos_word_t TunnelInvoked = 0x5455'4e49'4e56'4b00;
inline constexpr myos_word_t TunnelDelivered = 0x5455'4e44'4f4e'4500;

} // namespace myos::proof
