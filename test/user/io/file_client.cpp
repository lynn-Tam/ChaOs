#include <util.hpp>
#include <string.h>
#include <utility>
#include <ch.hpp>
#include <start.hpp>
#include <proto.hpp>

static io_client sessions[2];
constexpr uint64_t FileSize = 16397;
static void check(bool condition) {
    if (!condition)
        sys::exit(STATUS_INTERNAL);
}

int main() {
    const auto& info = rt_startup();
    const auto events = info.events;
    const auto vspace = info.vspace;
    const auto directory = info.selector(fs_service);
    const auto pool = info.pool;
    // No descendant exists: completion precedes arming the blocking revoke.
    const auto disposable = sys::memory_create(pool, 4096, VM_READ | VM_WRITE);
    if (disposable.status != STATUS_OK) sys::exit(disposable.status);
    cap_owner temporary{disposable.value};
    if (const auto rc = sys::cap_revoke(temporary.selector(), false).status; rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::object_destroy(temporary.selector()).status; rc != STATUS_OK)
        sys::exit(rc);
    temporary = {};
    uint64_t handles[2]{};
    for (size_t i = 0; i < 2; ++i) {
        uint64_t value{};
        if (const auto rc = sessions[i].connect(directory, pool, info.cspace, events,
                                                vspace, 0x70000000 + i * 0x100000, value);
            rc != STATUS_OK)
            sys::exit(rc);
        ch_msg list{.operation = static_cast<uint64_t>(fs_control::List)};
        if (const auto rc = sessions[i].exchange(list); rc != STATUS_OK)
            sys::exit(rc);
        check(list.size != 0 && list.value == 0);
        ch_msg opened{.operation = static_cast<uint64_t>(fs_control::Open), .size = 9};
        ::memcpy(opened.data, "/data.bin", 9);
        if (const auto rc = sessions[i].exchange(opened); rc != STATUS_OK)
            sys::exit(rc);
        check(opened.size == 8 && fs_file_size(opened) == FileSize);
        handles[i] = opened.value;
    }
    vm_area mappings[2];
    uint64_t identity{};
    constexpr size_t mapped_size = (FileSize + 4095) & ~size_t{4095};
    for (size_t i = 0; i < 2; ++i) {
        ch_msg request{
            .operation = static_cast<uint64_t>(fs_control::Map), .value = handles[i], .size = 1};
        request.data[0] = VM_READ | VM_EXECUTE;
        ch_packet packet;
        check(sessions[i].exchange(request, packet) == STATUS_DENIED && packet.count == 0);
        request = {
            .operation = static_cast<uint64_t>(fs_control::Map), .value = handles[i], .size = 1};
        request.data[0] = VM_READ;
        if (const auto rc = sessions[i].exchange(request, packet); rc != STATUS_OK)
            sys::exit(rc);
        check(packet.count == 1 && request.size == 8 && fs_file_size(request) == FileSize);
        if (i == 0)
            identity = request.value;
        else
            check(identity != 0 && identity == request.value);
        // The response conveys Map-only content authority; it cannot initialize
        // or modify the server's canonical backing object.
        check(sys::memory_write(packet.capabilities[0].selector(), 0, 0, 1).status ==
              STATUS_BAD_RIGHTS);
        check(sys::mem_trim(packet.capabilities[0].selector(), 0, 1).status == STATUS_BAD_RIGHTS);
        check(sys::mem_writeback(packet.capabilities[0].selector(), 0).status == STATUS_BAD_RIGHTS);
        auto mapped = vm_area::map(vspace, std::move(packet.capabilities[0]),
                                   0x75000000 + i * 0x100000, mapped_size, VM_READ);
        check(static_cast<bool>(mapped));
        mappings[i] = std::move(*mapped);
        const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mappings[i].address);
        for (size_t n = 0; n < mapped_size; ++n)
            check(bytes[n] == (n < FileSize ? n % 251 : 0));
    }
    // Files exports one immutable source to every client. A private mapping
    // initially reads that source, then owns only the pages it changes.
    ch_msg private_request{
        .operation = static_cast<uint64_t>(fs_control::Map), .value = handles[0], .size = 1};
    private_request.data[0] = VM_READ;
    ch_packet private_packet;
    if (const auto rc = sessions[0].exchange(private_request, private_packet); rc != STATUS_OK)
        sys::exit(rc);
    check(private_packet.count == 1 && private_request.value == identity);
    auto private_result = vm_area::map_private(vspace, std::move(private_packet.capabilities[0]),
                                               0x76000000, mapped_size);
    check(static_cast<bool>(private_result));
    auto private_mapping = std::move(*private_result);
    auto* private_bytes = reinterpret_cast<volatile uint8_t*>(private_mapping.address);
    check(private_bytes[0] == 0 && private_bytes[4096] == 4096 % 251);
    private_bytes[0] = 97;
    private_bytes[4096] = 98;
    private_bytes[FileSize] = 99;
    check(private_bytes[0] == 97 && private_bytes[4096] == 98 && private_bytes[FileSize] == 99);
    for (const auto& shared : mappings) {
        const auto* bytes = reinterpret_cast<const volatile uint8_t*>(shared.address);
        check(bytes[0] == 0 && bytes[4096] == 4096 % 251 && bytes[FileSize] == 0);
    }
    if (const auto rc = private_mapping.close(); rc != STATUS_OK)
        sys::exit(rc);
    for (size_t pass = 0; pass < 8; ++pass) {
        for (size_t page = 0; page < mapped_size; page += 4096) {
            const auto* bytes =
                reinterpret_cast<const volatile uint8_t*>(mappings[pass % 2].address);
            check(bytes[page] == page % 251);
        }
    }
    for (size_t batch = 0; batch < 16; ++batch) {
        uint64_t ids[2][io_queue_depth]{}, offsets[2][io_queue_depth]{};
        for (size_t i = 0; i < 2; ++i) {
            for (size_t slot = 0; slot < io_queue_depth; ++slot) {
                const uint64_t offset = (batch * 1031 + slot * 509 + i * 13) % (FileSize + 512);
                io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                               .object = handles[i],
                               .offset = offset,
                               .buffer_offset = slot * io_buffer_size,
                               .length = io_buffer_size};
                check(sessions[i].queue().submit(request) == libk::ring_status::ready);
                ids[i][slot] = request.id;
                offsets[i][slot] = offset;
            }
            if (const auto rc = sessions[i].flush(); rc != STATUS_OK)
                sys::exit(rc);
        }
        size_t remaining = 2 * io_queue_depth;
        while (remaining != 0) {
            for (size_t i = 0; i < 2; ++i) {
                for (;;) {
                    io_done completion;
                    const auto result = sessions[i].queue().take(completion);
                    if (result == libk::ring_status::empty)
                        break;
                    check(result == libk::ring_status::ready);
                    size_t slot{};
                    while (slot < io_queue_depth && ids[i][slot] != completion.id)
                        ++slot;
                    check(slot < io_queue_depth && completion.status == STATUS_OK &&
                          completion.flags == 0);
                    const auto offset = offsets[i][slot];
                    const auto available = offset >= FileSize ? 0 : FileSize - offset;
                    const auto expected = available < io_buffer_size ? available : io_buffer_size;
                    check(completion.bytes == expected);
                    for (size_t n = 0; n < expected; ++n)
                        check(sessions[i].payload()[slot * io_buffer_size + n] ==
                              (offset + n) % 251);
                    ids[i][slot] = 0;
                    --remaining;
                }
                if (const auto rc = sessions[i].flush(); rc != STATUS_OK)
                    sys::exit(rc);
                if (const auto rc = sessions[i].arm(); rc != STATUS_OK)
                    sys::exit(rc);
            }
            if (remaining != 0)
                if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
                    sys::exit(rc);
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        ch_msg closed{.operation = static_cast<uint64_t>(fs_control::Close), .value = handles[i]};
        if (const auto rc = sessions[i].exchange(closed); rc != STATUS_OK)
            sys::exit(rc);
        ch_msg opened{.operation = static_cast<uint64_t>(fs_control::Open), .size = 8};
        ::memcpy(opened.data, "DATA.BIN", 8);
        if (const auto rc = sessions[i].exchange(opened); rc != STATUS_OK)
            sys::exit(rc);
        check(opened.value != handles[i]);
        closed.value = handles[i];
        check(sessions[i].exchange(closed) == STATUS_NOT_FOUND);
        // Close while reads remain published; acknowledgement ends all admitted
        // downstream access without requiring the caller to consume every CQ.
        for (size_t slot = 0; slot < io_queue_depth; ++slot) {
            io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                           .object = opened.value,
                           .offset = 13,
                           .buffer_offset = slot * io_buffer_size,
                           .length = io_buffer_size};
            check(sessions[i].queue().submit(request) == libk::ring_status::ready);
        }
        if (const auto rc = sessions[i].flush(); rc != STATUS_OK)
            sys::exit(rc);
        if (const auto rc = sessions[i].close(); rc != STATUS_OK)
            sys::exit(rc);
    }
    // File/session close ends admission, while an already granted immutable
    // content capability and its mapping have their own lifetime.
    for (size_t i = 0; i < 2; ++i) {
        const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mappings[i].address);
        check(bytes[FileSize - 1] == (FileSize - 1) % 251);
        if (const auto rc = mappings[i].close(); rc != STATUS_OK)
            sys::exit(rc);
    }
    // Reuse the same slots and virtual ranges. Old handles and payload bytes
    // must not cross sessions; teardown must keep returning resources to the pool.
    for (size_t round = 0; round < 64; ++round) {
        for (size_t i = 0; i < 2; ++i) {
            uint64_t value{};
            if (const auto rc =
                    sessions[i].connect(directory, pool, info.cspace, events, vspace,
                                        0x70000000 + i * 0x100000, value);
                rc != STATUS_OK)
                sys::exit(rc);
            for (size_t byte = 0; byte < io_payload_size; ++byte)
                check(sessions[i].payload()[byte] == 0);
            ch_msg stale{.operation = static_cast<uint64_t>(fs_control::Map),
                         .value = handles[i],
                         .size = 1};
            stale.data[0] = VM_READ;
            check(sessions[i].exchange(stale) == STATUS_NOT_FOUND);
            ch_msg opened{.operation = static_cast<uint64_t>(fs_control::Open), .size = 8};
            ::memcpy(opened.data, "DATA.BIN", 8);
            if (const auto rc = sessions[i].exchange(opened); rc != STATUS_OK)
                sys::exit(rc);
            handles[i] = opened.value;
            io_cmd request{.operation = static_cast<uint64_t>(io_operation::Read),
                           .object = opened.value,
                           .offset = 13,
                           .length = io_buffer_size};
            check(sessions[i].queue().submit(request) == libk::ring_status::ready);
            if (const auto rc = sessions[i].flush(); rc != STATUS_OK)
                sys::exit(rc);
            io_done completion;
            for (;;) {
                const auto taken = sessions[i].queue().take(completion);
                if (taken == libk::ring_status::ready)
                    break;
                check(taken == libk::ring_status::empty);
                if (const auto rc = sessions[i].arm(); rc != STATUS_OK)
                    sys::exit(rc);
                if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
                    sys::exit(rc);
            }
            check(completion.status == STATUS_OK && completion.bytes == io_buffer_size);
            check(sessions[i].payload()[0] == 13);
        }
        for (auto& session : sessions)
            if (const auto rc = session.close(); rc != STATUS_OK)
                sys::exit(rc);
    }
    sys::exit();
}
