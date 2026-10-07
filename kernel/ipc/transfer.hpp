#pragma once

#include <utility>


#include <cap/cspace.hpp>
#include <base/types.hpp>
#include <expected>
#include <libk/inplace_vector.hpp>
#include <libk/noncopyable.hpp>
#include <uapi/ipc.h>

namespace ipc {

enum class TransferKind : u8 {
    Copy,
    Move,
    Delegate,
};

struct TransferSpec final {
    cap::Handle source{};
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
    using Specs = libk::InplaceVector<TransferSpec, IPC_MAX_CAPS>;
    using Handles = libk::InplaceVector<cap::Handle, IPC_MAX_CAPS>;

    Transfer() noexcept = default;
    Transfer(Transfer&&) noexcept = default;
    auto operator=(Transfer&&) noexcept -> Transfer& = default;
    ~Transfer() noexcept = default;

    [[nodiscard]] static auto prepare(
        Transfer& transfer,
        cap::CSpace& source,
        cap::CSpace& destination,
        const Specs& specs) noexcept
        -> std::expected<void, cap::CSpaceError>;

    [[nodiscard]] auto commit() noexcept
        -> std::expected<Handles, TransferError>;
    [[nodiscard]] auto handles() const noexcept -> Handles;
    void abort() noexcept { reset(); }
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
            cap::Handle source_handle,
            cap::GrantKey source_key,
            cap::View source_view,
            cap::View destination_view,
            TransferKind transfer_kind) noexcept
            : slot(std::move(reserved)),
              lease(std::move(admission)),
              prepared(std::move(grant)),
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
        cap::Handle source{};
        cap::GrantKey key{};
        cap::View original{};
        cap::View view{};
        TransferKind kind{TransferKind::Copy};
    };

    cap::CSpace* source_{};
    cap::CSpace* destination_{};
    libk::InplaceVector<Entry, IPC_MAX_CAPS> entries_{};
};

} // namespace ipc
