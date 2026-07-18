#pragma once

#include <cap/grant.hpp>
#include <cap/authority.hpp>
#include <libk/noncopyable.hpp>
#include <libk/utility.hpp>
#include <object/object_ref.hpp>

namespace kernel::cap {

class CSpace;

template<typename T>
class Resolved final : private libk::noncopyable {
public:
    Resolved(Resolved&&) noexcept = default;
    auto operator=(Resolved&&) noexcept -> Resolved& = default;

    [[nodiscard]] auto object() noexcept -> T& { return pin_.get(); }
    [[nodiscard]] auto object() const noexcept -> const T& { return pin_.get(); }
    [[nodiscard]] auto operator->() noexcept -> T* { return &pin_.get(); }
    [[nodiscard]] auto operator->() const noexcept -> const T* {
        return &pin_.get();
    }
    [[nodiscard]] auto rights() const noexcept -> Rights {
        return authority_.rights;
    }
    [[nodiscard]] auto authority() const noexcept -> EffectiveAuthority {
        return authority_;
    }
    [[nodiscard]] auto grant() const noexcept -> GrantKey {
        return lease_.key();
    }
    [[nodiscard]] auto reference() const noexcept
        -> libk::Expected<object::ObjectRef, object::ObjectError> {
        return lease_.clone_target();
    }
    [[nodiscard]] auto attach(GrantAttachment& attachment) const noexcept
        -> libk::Expected<void, GrantError> {
        return lease_.attach(attachment);
    }
    [[nodiscard]] auto derive_region(
        kernel::resource::Reservation&& charge,
        object::ObjectRef&& target,
        GrantCeiling ceiling,
        RegionDerivation proof) const noexcept
        -> libk::Expected<GrantRef, GrantError> {
        return lease_.derive_region(
            libk::move(charge), libk::move(target), ceiling, proof);
    }
    [[nodiscard]] auto derive_tunnel_tx(
        kernel::resource::Reservation&& charge,
        object::ObjectRef&& target,
        GrantCeiling ceiling,
        TunnelConnectProof proof) const noexcept
        -> libk::Expected<GrantRef, GrantError> {
        return lease_.derive_tunnel_tx(
            libk::move(charge), libk::move(target), ceiling, proof);
    }
    [[nodiscard]] auto source() const noexcept -> CSpace& { return *source_; }

private:
    friend class CSpace;
    Resolved(
        CSpace& source,
        object::ObjectPin<T>&& pin,
        GrantLease&& lease,
        EffectiveAuthority authority) noexcept
        : source_(&source),
          lease_(libk::move(lease)),
          pin_(libk::move(pin)),
          authority_(libk::move(authority)) {}

    CSpace* source_{};
    GrantLease lease_;
    object::ObjectPin<T> pin_;
    EffectiveAuthority authority_{};
};

} // namespace kernel::cap
