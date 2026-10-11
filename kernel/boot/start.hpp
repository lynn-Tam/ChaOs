#pragma once

#include <boot/info.hpp>
#include <uapi/start.h>
#include <cap/cspace.hpp>
#include <cpu/cpu.hpp>
#include <io/space.hpp>
#include <ipc/channel.hpp>
#include <ipc/endpoint.hpp>
#include <ipc/notification.hpp>
#include <mm/pager.hpp>
#include <object/pool.hpp>
#include <sched/domain.hpp>

// Cold ownership only. Runtime code borrows the concrete entities it needs;
// none of these resources are destroyed while CPUs can still execute.
struct Boot {
    Boot(mm::Pmm& pmm, mm::KSpace& vm, u64 frequency) noexcept
        : pmm(pmm), vm(vm), clock(frequency), objects(pmm, work),
          grants(pmm, work), cpus(pmm), worker{cpus} {}

    mm::Pmm& pmm;
    mm::KSpace& vm;
    time::Clock clock;
    WorkQueue work;
    object::Objects objects;
    cap::Graph grants;
    Cpus cpus;
    object::ref<sched::Domain> domain;
    object::ref<object::group> root;
    struct Worker {
        Cpus& cpus;
        object::ref<sched::Sc> sc{};
        object::ref<Thread> thread{};
        void operator()() noexcept { libk_assert(sc && sched::wake(cpus, sc.get())); }
    } worker;
};

enum class BootErr : u8 {
    InvalidModule, InvalidBundle, Ownership, OutOfMemory,
    InvalidState, MappingFailed, CapabilityFailed, SchedulingFailed,
};
auto boot_root(Boot&, Cpu&, const BootInfo&, mm::BootPages&&, mm::BootPages&&, HwStart) noexcept
    -> std::expected<void, BootErr>;
