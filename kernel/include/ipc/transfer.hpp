#pragma once

#include <cap/cspace.hpp>
#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/noncopyable.hpp>
#include <uapi/ipc.h>

namespace kernel::ipc {

enum class TransferKind : u8 {
    Copy,
    Move,
    Delegate,
};

struct TransferSpec final {
    cap::CapHandle source{};
    cap::Rights rights{};
    TransferKind kind{TransferKind::Copy};
};

enum class TransferError : u8 {
    InvalidSpec,
    SourceChanged,
    Capability,
};

// One all-or-nothing transfer into a known CSpace. Preparation reserves every
// destination slot and captures Grant leases, but does not mutate a source
// slot. commit() takes both CSpace locks in address order, revalidates every
// move, and publishes the complete batch in one critical section.
class Transfer final : private libk::noncopyable {
public:
    using Specs = libk::InplaceVector<TransferSpec, MYOS_IPC_MAX_CAPS>;
    using Handles = libk::InplaceVector<cap::CapHandle, MYOS_IPC_MAX_CAPS>;

    Transfer() noexcept = default;
    Transfer(Transfer&&) noexcept = default;
    auto operator=(Transfer&&) noexcept -> Transfer& = default;
    ~Transfer() noexcept = default;

    [[nodiscard]] static auto prepare(
        Transfer& transfer,
        cap::CSpace& source,
        cap::CSpace& destination,
        const Specs& specs) noexcept
        -> libk::Expected<void, cap::CSpaceError>;

    [[nodiscard]] auto commit() noexcept
        -> libk::Expected<Handles, TransferError>;
    [[nodiscard]] auto empty() const noexcept -> bool {
        return entries_.empty();
    }

private:
    void reset() noexcept;

    struct Entry final : private libk::noncopyable {
        Entry(
            cap::CSpace::Reservation&& reserved,
            cap::GrantLease&& admission,
            cap::GrantRef&& grant,
            cap::CapHandle source_handle,
            cap::GrantKey source_key,
            cap::CapView source_view,
            cap::CapView destination_view,
            TransferKind transfer_kind) noexcept
            : slot(libk::move(reserved)),
              lease(libk::move(admission)),
              prepared(libk::move(grant)),
              source(source_handle),
              key(source_key),
              original(source_view),
              view(destination_view),
              kind(transfer_kind) {}

        Entry(Entry&&) noexcept = default;
        auto operator=(Entry&&) noexcept -> Entry& = default;

        cap::CSpace::Reservation slot;
        cap::GrantLease lease{};
        cap::GrantRef prepared{};
        cap::CapHandle source{};
        cap::GrantKey key{};
        cap::CapView original{};
        cap::CapView view{};
        TransferKind kind{TransferKind::Copy};
    };

    cap::CSpace* source_{};
    cap::CSpace* destination_{};
    libk::InplaceVector<Entry, MYOS_IPC_MAX_CAPS> entries_{};
};

} // namespace kernel::ipc
