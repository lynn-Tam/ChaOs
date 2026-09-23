#pragma once

#include <cap/cspace.hpp>
#include <libk/intrusive_list.hpp>
#include <object/object_ref.hpp>
#include <operation/completion.hpp>
#include <uapi/channel.h>

namespace kernel::ipc {
class Channel;
class Buffer;
using ChannelSide = cap::ChannelSide;

struct ChannelSend final {
    u64 transaction{};
    u64 tag{};
    usize word_count{};
    u64 words[MYOS_CHANNEL_MAX_WORDS]{};
    usize cap_count{};
    myos_cap_transfer caps[MYOS_CHANNEL_MAX_CAPS]{};
};

struct ChannelRecv final {
    u64 transaction{};
    u64 tag{};
    u64 sender_badge{};
    u64 sequence{};
    usize word_count{};
    u64 words[MYOS_CHANNEL_MAX_WORDS]{};
    usize cap_count{};
    usize receive_limit{MYOS_CHANNEL_MAX_CAPS};
    cap::CapHandle caps[MYOS_CHANNEL_MAX_CAPS]{};
};

// Resident in the continuation: Channel indexes this node but never allocates it.
struct ChannelWait final : private libk::noncopyable_nonmovable {
    enum class Kind : u8 {
        Send,
        Receive,
    };
    enum class State : u8 {
        Attaching,
        Awaiting,
        Armed,
        Ready,
        Done,
    };

    explicit ChannelWait(Channel& owner) noexcept;
    ~ChannelWait() noexcept;

    [[nodiscard]] auto complete() const noexcept -> bool;
    [[nodiscard]] auto read() noexcept -> kernel::operation::Result;
    void release() noexcept;
    [[nodiscard]] auto cancel() noexcept -> bool;
    void resume(arch::TrapContext& trap) noexcept;

    Channel* owner{};
    Kind kind{Kind::Send};
    State state{State::Attaching};
    libk::IntrusiveListHook hook{};
    // Admission, Completion, authority attachment, and lock-external
    // notifications each own one reference. Protected by Channel::lock_.
    usize references{1};
    bool admitted{};
    bool authority_detaching{};
    ChannelSide side{ChannelSide::A};
    cap::CSpace* cspace{};
    cap::CapHandle authority{};
    Buffer* buffer{};
    ChannelSend send{};
    usize receive_limit{};
    kernel::operation::Result result{};
    object::ObjectRef channel_ref{};
    cap::GrantAttachment grant_attachment;
    kernel::operation::Completion completion;
};

} // namespace kernel::ipc
