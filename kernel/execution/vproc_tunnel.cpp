#include <execution/vproc.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <ipc/tunnel.hpp>
#include <libk/limits.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel {

auto Vproc::attach_tunnel_source(ipc::TunnelLink& link) noexcept -> bool {
    kernel::sync::IrqLockGuard guard{tunnel_lock_};
    if (tunnel_admission_closed_ || link.hook.is_linked()) {
        return false;
    }
    outgoing_tunnels_.push_back(link);
    return true;
}

auto Vproc::attach_tunnel_target(
    ipc::TunnelLink& link,
    usize slot,
    usize tag) noexcept -> libk::optional<u64> {
    if (slot >= MYOS_VPROC_MAX_INGRESS) {
        return libk::nullopt;
    }
    kernel::sync::IrqLockGuard guard{tunnel_lock_};
    IngressSlot& ingress = ingresses_[slot];
    if (tunnel_admission_closed_ || ingress.link != nullptr
        || ingress.binding_generation == libk::numeric_limits<u64>::max()) {
        return libk::nullopt;
    }
    ++ingress.binding_generation;
    KASSERT(ingress.binding_generation != 0);
    ingress.link = &link;
    ingress.delivery_generation = 0;
    ingress.tag = tag;
    return ingress.binding_generation;
}

void Vproc::detach_tunnel_source(ipc::TunnelLink& link) noexcept {
    kernel::sync::IrqLockGuard guard{tunnel_lock_};
    if (link.hook.is_linked()) {
        outgoing_tunnels_.erase(link);
    }
}

void Vproc::detach_tunnel_target(
    ipc::TunnelLink& link,
    usize slot,
    u64 binding_generation) noexcept {
    if (slot >= MYOS_VPROC_MAX_INGRESS) {
        return;
    }
    kernel::sync::IrqLockGuard tunnel_guard{tunnel_lock_};
    IngressSlot& ingress = ingresses_[slot];
    if (ingress.link != &link
        || ingress.binding_generation != binding_generation) {
        return;
    }
    kernel::sync::IrqLockGuard operation_guard{operation_lock_};
    ingress.link = nullptr;
    ingress.delivery_generation = 0;
    ingress.tag = 0;
    ingress_mask_ &= ~(u64{1} << slot);
    if (runtime_.events != nullptr) {
        __atomic_fetch_and(
            &runtime_.events->ingress_mask,
            ~(u64{1} << slot),
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->ingress_generation[slot],
            0,
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->ingress_tag[slot], 0, __ATOMIC_RELEASE);
    }
}

void Vproc::publish_tunnel(
    ipc::TunnelLink& link,
    usize slot,
    u64 binding_generation,
    u64 delivery_generation,
    usize tag,
    CpuRegistry& cpus) noexcept {
    if (slot >= MYOS_VPROC_MAX_INGRESS) {
        return;
    }
    {
        kernel::sync::IrqLockGuard tunnel_guard{tunnel_lock_};
        IngressSlot& ingress = ingresses_[slot];
        if (ingress.link != &link
            || ingress.binding_generation != binding_generation
            || ingress.tag != tag) {
            return;
        }
        kernel::sync::IrqLockGuard operation_guard{operation_lock_};
        ingress.delivery_generation = delivery_generation;
        const u64 bit = u64{1} << slot;
        if ((ingress_mask_ & bit) == 0) {
            ++pending_sequence_;
            KASSERT(pending_sequence_ != 0);
        }
        ingress_mask_ |= bit;
        __atomic_store_n(
            &runtime_.events->ingress_generation[slot],
            delivery_generation,
            __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->ingress_tag[slot], tag, __ATOMIC_RELEASE);
        __atomic_fetch_or(
            &runtime_.events->ingress_mask, bit, __ATOMIC_RELEASE);
        __atomic_store_n(
            &runtime_.events->pending_sequence,
            pending_sequence_,
            __ATOMIC_RELEASE);
    }
    // Detach clears this projection if close wins after publication.  The
    // scheduler may then observe a harmless activation with no pending event;
    // it must not call back into Tunnel while holding the Vproc relation lock.
    // The retained request actively creates a trap boundary on a remote
    // running hart. Failure delays delivery; Tunnel remains canonical.
    static_cast<void>(sched::activate(cpus, *this));
}

void Vproc::clear_tunnel(
    ipc::TunnelLink& link,
    usize slot,
    u64 binding_generation,
    u64 delivery_generation) noexcept {
    if (slot >= MYOS_VPROC_MAX_INGRESS) {
        return;
    }
    kernel::sync::IrqLockGuard tunnel_guard{tunnel_lock_};
    IngressSlot& ingress = ingresses_[slot];
    if (ingress.link != &link
        || ingress.binding_generation != binding_generation
        || ingress.delivery_generation > delivery_generation) {
        return;
    }
    kernel::sync::IrqLockGuard operation_guard{operation_lock_};
    ingress.delivery_generation = 0;
    const u64 bit = u64{1} << slot;
    ingress_mask_ &= ~bit;
    __atomic_fetch_and(
        &runtime_.events->ingress_mask, ~bit, __ATOMIC_RELEASE);
    __atomic_store_n(
        &runtime_.events->ingress_generation[slot], 0, __ATOMIC_RELEASE);
}

void Vproc::close_tunnels() noexcept {
    for (;;) {
        ipc::Tunnel* tunnel{};
        {
            kernel::sync::IrqLockGuard guard{tunnel_lock_};
            if (!outgoing_tunnels_.empty()) {
                tunnel = outgoing_tunnels_.front().tunnel;
            } else {
                for (IngressSlot& ingress : ingresses_) {
                    if (ingress.link != nullptr) {
                        tunnel = ingress.link->tunnel;
                        break;
                    }
                }
            }
            if (tunnel == nullptr) {
                return;
            }
            tunnel->retain_relation();
        }
        tunnel->peer_stopped(*this);
        tunnel->release_relation();
    }
}

} // namespace kernel
