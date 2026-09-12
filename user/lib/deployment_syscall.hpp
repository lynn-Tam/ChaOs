#pragma once

#include <user/lib/deployment.hpp>
#include <user/lib/capability_syscall.hpp>

namespace myos::cap {

template<size_t LocalCapacity, size_t RemoteCapacity>
using DeploymentCaps = BasicDeploymentCaps<
    LocalCapacity, RemoteCapacity, SyscallBackend>;

template<size_t LocalCapacity, size_t RemoteCapacity>
using TaskSpace = deploy::TaskSpace<
    LocalCapacity, RemoteCapacity, SyscallBackend>;

using MappedBundle = deploy::MappedBundle<SyscallBackend>;
using ScratchWindow = deploy::ScratchWindow<SyscallBackend>;

} // namespace myos::cap
