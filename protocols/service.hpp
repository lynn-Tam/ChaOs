#pragma once
#include <start.hpp>
#include <protocols/msg.hpp>
#include <string.h>

inline constexpr rt_req sv_service{"service.control", 0x53564354, ObjChannel, 7};
inline constexpr rt_req sv_wake{"service.wake", 0x53564357, ObjNotification};
inline constexpr rt_req sv_ready{"ready", 0, ObjNotification};
enum class sv_op : uint64_t { Restart, List, Lookup, Stop, Start, Define, Grant, Remove, Provide };
enum class sv_state : uint64_t { Stopped, Draining, Starting, Running, Failed };

struct sv_query {
    uint32_t protocol;
    uint16_t major, minor;
};
// A trusted component can declare ordinary child services. Root remains their
// sole task owner; each Grant transfers one explicitly named capability.
// Define transfers a bounded lifecycle doorbell Channel sender, not a task reference.
struct sv_define {
    uint64_t bytes, caps, kinds, slots, pages;
    uint16_t name_size, image_size;
    uint32_t flags; // bit 0: wait for the ordinary ready notification
};
struct sv_status { uint64_t desired; };
struct sv_grant {
    uint64_t rights, bytes;
    uint32_t protocol;
    uint16_t kind, major, minor, reserved;
    char name[32];
};
inline auto sv_grant_cap(rpc_client& root, uint64_t instance, rt_req contract, cap_t cap,
                        uint64_t rights, uint64_t bytes = 0) -> status_t {
    const std::string_view name{contract.name};
    if (name.empty() || name.size() >= sizeof(sv_grant::name)) return STATUS_BAD_ARGS;
    sv_grant grant{rights, bytes, contract.protocol, contract.kind, contract.major, contract.minor, 0, {}};
    ::memcpy(grant.name, name.data(), name.size());
    msg_frame request{.operation=static_cast<uint64_t>(sv_op::Grant), .value=instance, .size=sizeof(grant)};
    ::memcpy(request.data, &grant, sizeof(grant));
    msg_packet reply;
    const CapXfer transfer{cap, rights | RIGHT_DUPLICATE, CAP_COPY, 0};
    return root.exchange(request, reply, {&transfer, 1});
}
inline auto sv_lookup(rpc_client& directory, const rt_req& contract, uint64_t deadline = 0)
    -> std::expected<cap_owner, status_t> {
    const std::string_view name{contract.name};
    if (name.empty() || name.size() > sizeof(msg_frame::data) - sizeof(sv_query))
        return std::unexpected(STATUS_BAD_ARGS);
    msg_frame request{.operation = static_cast<uint64_t>(sv_op::Lookup),
                   .size = sizeof(sv_query) + name.size(),
                   .deadline = deadline};
    const sv_query query{contract.protocol, contract.major, contract.minor};
    ::memcpy(request.data, &query, sizeof(query));
    ::memcpy(request.data + sizeof(query), name.data(), name.size());
    msg_packet reply;
    const auto status = directory.exchange(request, reply);
    if (status != STATUS_OK)
        return std::unexpected(status);
    if (reply.count != 1 || request.size || request.value != contract.kind)
        return std::unexpected(STATUS_PEER_FAULT);
    return std::move(reply.capabilities[0]);
}
