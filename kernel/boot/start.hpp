#pragma once

#include <boot/info.hpp>
#include <uapi/start.h>
#include <cap/cspace.hpp>
#include <cpu/cpu.hpp>
#include <io/space.hpp>
#include <ipc/channel.hpp>
#include <ipc/notification.hpp>
#include <ipc/port.hpp>
#include <mm/pager.hpp>
#include <object/pool.hpp>
#include <sched/sched.hpp>

// Cold ownership only. Runtime code borrows the concrete entities it needs;
// none of these resources are destroyed while CPUs can still execute.
struct Boot {
    Boot(mm::Pmm& pmm, mm::KSpace& vm, u64 frequency) noexcept
        : pmm(pmm), vm(vm), clock(frequency), objects(pmm, work),
          grants(pmm, work), cpus(pmm) {}

    mm::Pmm& pmm;
    mm::KSpace& vm;
    time::Clock clock;
    WorkQueue work;
    object::Objects objects;
    cap::Graph grants;
    Cpus cpus;
    object::ref<sched::ScDomain> domain;
    object::ref<resource::Quota> root;
    object::ref<object::Scope> scope;
    object::ref<mm::Phys> physical;

};

enum class BootErr : u8 {
    InvalidModule, InvalidBundle, Ownership, OutOfMemory,
    InvalidState, MappingFailed, CapabilityFailed, SchedulingFailed,
};
auto boot_root(Boot&, Cpu&, const BootInfo&, mm::BootPages&&, mm::BootPages&&) noexcept
    -> std::expected<object::ref<Thread>, BootErr>;
