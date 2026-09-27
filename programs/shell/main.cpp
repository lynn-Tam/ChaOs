#include <user/lib/stream.hpp>
#include <user/lib/console.hpp>
#include <user/lib/imports.hpp>
#include <user/lib/clock.hpp>
#include <libk/fmt.hpp>
#include <user/lib/file_client.hpp>
#include <user/lib/store_client.hpp>

namespace {
using namespace myos;
uint64_t children[4]{};
uint64_t latest{};
void remember(uint64_t id) {
    for (auto& child : children) if (!child) { child = id; latest = id; return; }
    exit(MYOS_STATUS_INTERNAL);
}
void forget(uint64_t id) {
    for (auto& child : children) if (child == id) child = 0;
    if (latest == id) { latest = 0; for (auto child : children) if (child) latest = child; }
}
files::Client filesystem;
store::Client store_client;
bool storage_available{};

void error(stream::Writer& console, myos_status_t status) {
    (void)libk::fmt::format_to<"error: {}\n">(console, status);
}

class Editor final {
    store::File draft_{};
    char target_[sizeof(io::ControlMessage::data) + 1]{};
    char temporary_[24]{};
    uint64_t offset_{};
    bool active_{};
    bool draft_present_{};
public:
    auto active() const noexcept -> bool { return active_; }
    auto draft_present() const noexcept -> bool { return draft_present_; }

    auto open(const char* path) noexcept -> myos_status_t {
        const size_t length = service::length(path);
        if (length == 0 || length > sizeof(io::ControlMessage::data)) return MYOS_STATUS_BAD_ARGS;
        service::copy(target_, path, length + 1);
        const auto now = clock_now();
        if (now.status != MYOS_STATUS_OK) return now.status;
        uint64_t candidate = now.value;
        constexpr char digits[] = "0123456789abcdef";
        for (;;) {
            service::copy(temporary_, "/.edit-", 7);
            for (size_t i = 0; i < 16; ++i)
                temporary_[7 + i] = digits[(candidate >> ((15 - i) * 4)) & 15];
            temporary_[23] = '\0';
            const auto status = store_client.open(temporary_,
                store::Write | store::Create | store::Exclusive, draft_);
            if (status == MYOS_STATUS_BUSY && candidate != UINT64_MAX) { ++candidate; continue; }
            if (status != MYOS_STATUS_OK) return status;
            offset_ = 0;
            active_ = true;
            draft_present_ = true;
            return MYOS_STATUS_OK;
        }
    }

    auto line(const char* text) noexcept -> myos_status_t {
        const size_t length = service::length(text);
        const auto status = store_client.write(draft_, offset_,
            reinterpret_cast<const uint8_t*>(text), length);
        if (status != MYOS_STATUS_OK) return status;
        offset_ += length;
        const uint8_t newline = '\n';
        const auto ended = store_client.write(draft_, offset_, &newline, 1);
        if (ended == MYOS_STATUS_OK) ++offset_;
        return ended;
    }

    auto finish() noexcept -> myos_status_t {
        active_ = false;
        auto status = store_client.sync(draft_);
        const auto closed = store_client.close(draft_);
        if (status == MYOS_STATUS_OK) status = closed;
        if (status == MYOS_STATUS_OK) {
            status = store_client.rename(temporary_, target_);
            if (status == MYOS_STATUS_OK) draft_present_ = false;
        }
        return status;
    }

    auto cancel() noexcept -> myos_status_t {
        active_ = false;
        auto status = store_client.close(draft_);
        const auto removed = store_client.remove(temporary_);
        if (removed == MYOS_STATUS_OK) draft_present_ = false;
        if (status == MYOS_STATUS_OK) status = removed;
        return status;
    }

    auto draft() const noexcept -> const char* { return temporary_; }
};
Editor editor;

// Parse one bounded command line into the existing argv wire form. A pipe is
// syntax only outside quotes; escaped bytes and empty quoted words are data.
auto arguments(char* text, bootstrap::Arguments& output, size_t& split) noexcept -> bool {
    char word[128]{};
    size_t used{};
    bool present{};
    char quote{};
    auto append = [&]() noexcept -> bool {
        if (!present) return true;
        const bool ok = output.append(word, used);
        used = 0;
        present = false;
        return ok;
    };
    for (;;) {
        const char ch = *text++;
        if (ch == '\0') return quote == 0 && append();
        if (ch == '\\' && *text != '\0') {
            if (used == sizeof(word)) return false;
            word[used++] = *text++;
            present = true;
        } else if (ch == '\'' || ch == '"') {
            if (quote == 0) { quote = ch; present = true; }
            else if (quote == ch) quote = 0;
            else { if (used == sizeof(word)) return false; word[used++] = ch; }
        } else if (quote == 0 && (ch == ' ' || ch == '|')) {
            if (!append()) return false;
            if (ch == '|') {
                if (split != 0 || output.count() == 0) return false;
                split = output.data().size;
            }
        } else {
            if (used == sizeof(word)) return false;
            word[used++] = ch;
            present = true;
        }
    }
}

void command(char* line, service::Connection& process, stream::Writer& console,
    myos_cap_t control, myos_cap_t pool, myos_cap_t cspace) {
    while (*line == ' ') ++line;
    char* argument = line;
    while (*argument != '\0' && *argument != ' ') ++argument;
    if (*argument != '\0') *argument++ = '\0';
    while (*argument == ' ') ++argument;
    if (*line == '\0') return;
    if (service::equal(line, "help")) {
        console.write("help | ls (boot) | cat FILE (boot) | fs COMMAND (data) | mkfs [32_HEX_ID] | edit FILE | run/spawn PROGRAM [ARGS] | jobs | wait [ID] [MS] | stop [ID] | restart SERVICE\n");
        if (storage_available) console.write("fs: ls [DIR], cat/stat/touch FILE, write/append FILE TEXT, mkdir/rm PATH, mv OLD NEW, copy BOOTFILE FILE, device, volid\n");
        return;
    }
    if (service::equal(line, "mkfs")) {
        if (!storage_available) { error(console, MYOS_STATUS_NOT_FOUND); return; }
        myos_status_t status{};
        if (*argument == '\0') status = store_client.format();
        else {
            uint8_t id[store::VolumeIdSize]{};
            status = store::parse_volume_id(argument, service::length(argument), id)
                ? store_client.format(id) : MYOS_STATUS_BAD_ARGS;
        }
        if (status != MYOS_STATUS_OK) error(console, status);
        return;
    }
    if (service::equal(line, "edit")) {
        if (!storage_available) { error(console, MYOS_STATUS_NOT_FOUND); return; }
        bootstrap::Arguments path;
        size_t split{};
        if (!arguments(argument, path, split) || split != 0 || path.count() != 1) {
            error(console, MYOS_STATUS_BAD_ARGS); return;
        }
        const auto status = editor.open(path.argument(0));
        if (status != MYOS_STATUS_OK) error(console, status);
        else console.write("Enter replacement text; '.' saves, ':q' cancels.\n");
        return;
    }
    if (service::equal(line, "restart")) {
        service::Message request{};
        request.size = service::length(argument);
        if (request.size == 0 || request.size >= sizeof(request.data)) {
            console.write("invalid service\n");
            return;
        }
        service::copy(request.data, argument, request.size);
        const auto pair = channel_create(pool, 1, MYOS_CHANNEL_MAX_WORDS, 0, 1);
        if (pair.status != MYOS_STATUS_OK) {
            error(console, pair.status);
            return;
        }
        cap::OwnedCap inbox_root{{pair.value, 0}}, outbox_root{{pair.value2, 0}};
        const auto reader = channel_mint(inbox_root.selector(), cspace, 1, MYOS_RIGHT_RECEIVE);
        const auto sender = channel_mint(outbox_root.selector(), cspace, 1,
            MYOS_RIGHT_SEND | MYOS_RIGHT_DUPLICATE);
        cap::OwnedCap inbox, outbox;
        if (reader.status == MYOS_STATUS_OK) inbox = cap::OwnedCap{{reader.value, 0}};
        if (sender.status == MYOS_STATUS_OK) outbox = cap::OwnedCap{{sender.value, 0}};
        auto status = reader.status != MYOS_STATUS_OK ? reader.status : sender.status;
        if (status == MYOS_STATUS_OK) {
            status = service::send_cap(control, request, outbox.selector(), MYOS_RIGHT_SEND).status;
            if (status == MYOS_STATUS_OK) {
                console.write("service restart requested\n");
                service::Message reply{};
                status = service::receive(inbox.selector(), reply).status;
                if (status == MYOS_STATUS_OK) status = reply.status;
            }
        }
        const auto destroyed = object_destroy(inbox_root.selector()).status;
        if (destroyed != MYOS_STATUS_OK) status = destroyed;
        if (status != MYOS_STATUS_OK && status != MYOS_STATUS_BUSY) error(console, status);
        return;
    }
    if (service::equal(line, "ls")) {
        io::ControlMessage reply;
        do {
            const auto status = filesystem.list(reply);
            if (status != MYOS_STATUS_OK) {
                error(console, status);
                return;
            }
            console.write(reply.data, reply.size);
        } while (reply.value != 0);
        return;
    }
    if (service::equal(line, "jobs")) {
        for (auto child : children) if (child) (void)libk::fmt::format_to<"task: {}\n">(console, child);
        return;
    }
    const bool cat = service::equal(line, "cat");
    const bool explicit_run = service::equal(line, "run");
    const bool spawn = service::equal(line, "spawn");
    const bool wait = service::equal(line, "wait");
    const bool stop = service::equal(line, "stop");
    const bool run = !spawn && !wait && !stop;
    service::Message request{};
    request.operation = static_cast<uint64_t>(run || spawn ? service::Process::Spawn
        : wait ? service::Process::Wait : service::Process::Stop);
    request.id = latest;
    if ((wait || stop) && *argument != 0) {
        char* end = argument;
        while (*end && *end != ' ') ++end;
        if (*end) *end++ = 0;
        const auto parsed = decimal(argument);
        if (!parsed) { console.write("invalid task\n"); return; }
        request.id = *parsed;
        while (*end == ' ') ++end;
        if (wait && *end) {
            const auto duration = decimal(end);
            Clock clock;
            service::require(clock.open());
            const auto deadline = duration ? clock.after_ms(*duration) : libk::nullopt;
            if (!deadline) { console.write("invalid timeout\n"); return; }
            request.size = sizeof(uint64_t);
            service::copy(request.data, &*deadline, request.size);
        }
    }
    if (run || spawn) {
        bootstrap::Arguments words;
        if (cat) (void)words.append("cat", 3);
        else if (!explicit_run && !spawn)
            (void)words.append(line, service::length(line));
        size_t split{};
        if (!arguments(argument, words, split) || words.count() == 0) {
            console.write("invalid command line\n"); return;
        }
        request.size = words.data().size;
        if (split != 0) {
            if (split == request.size) { console.write("invalid pipeline\n"); return; }
            request.operation = static_cast<uint64_t>(service::Process::Pipeline);
            request.id = split;
        }
        if (request.size > sizeof(request.data)) { console.write("argument too long\n"); return; }
        service::copy(request.data, words.data().bytes, request.size);
    }
    service::require(process.send(request).status);
    service::Message reply{};
    service::require(process.receive(reply).status);
    if ((run || spawn) && reply.status == MYOS_STATUS_OK) {
        const auto child = reply.id;
        uint64_t consumer{};
        if (reply.operation == static_cast<uint64_t>(service::Process::Pipeline)) {
            if (reply.size != sizeof(consumer)) exit(MYOS_STATUS_PEER_FAULT);
            service::copy(&consumer, reply.data, sizeof(consumer));
        }
        if (run) {
            service::Message wait_request{.operation = static_cast<uint64_t>(service::Process::Wait),
                .id = consumer ? consumer : child};
            service::require(process.send(wait_request).status);
            service::require(process.receive(reply).status);
            const auto status = reply.status;
            if (consumer) {
                wait_request.id = child;
                service::require(process.send(wait_request).status);
                service::require(process.receive(reply).status);
                if (reply.status != MYOS_STATUS_OK) error(console, reply.status);
                reply.status = status;
            }
        } else {
            remember(child);
            (void)libk::fmt::format_to<"task: {}\n">(console, child);
            if (consumer) {
                remember(consumer);
                (void)libk::fmt::format_to<"task: {}\n">(console, consumer);
            }
            return;
        }
    } else if (wait || stop) {
        if (reply.status != MYOS_STATUS_TIMED_OUT) forget(request.id);
    }
    if (reply.status != MYOS_STATUS_OK
        && !(stop && reply.status == MYOS_STATUS_CANCELED)) error(console, reply.status);
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto output = service::capability(info, myos::bootstrap::imports::ConsoleOutput);
    stream::Writer console{output};
    const auto input = service::capability(info, myos::bootstrap::imports::ConsoleInput);
    service::Connection process{
        service::capability(info, myos::bootstrap::imports::Process),
        service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION)};
    service::require(filesystem.connect(info));
    storage_available = info.selector(bootstrap::imports::Store) != 0;
    if (storage_available) service::require(store_client.connect(info));
    console.write("myos native shell\n");
    myos::console::prompt(output, "myos> ");
    char line[128]{};
    size_t used = 0;
    bool overflow = false;
    bool carriage_return = false;
    for (;;) {
        service::Message message{};
        service::require(service::receive(input, message).status);
        for (size_t i = 0; i < message.size; ++i) {
            const char byte = message.data[i];
            if (byte == '\n' && carriage_return) { carriage_return = false; continue; }
            carriage_return = byte == '\r';
            if (byte == '\r' || byte == '\n') {
                console.put('\n');
                line[used] = '\0';
                if (overflow) console.write("line too long\n");
                else if (editor.active()) {
                    myos_status_t status = MYOS_STATUS_OK;
                    if (service::equal(line, ".")) status = editor.finish();
                    else if (service::equal(line, ":q")) status = editor.cancel();
                    else status = editor.line(line);
                    if (status != MYOS_STATUS_OK) {
                        error(console, status);
                        if (editor.active()) (void)editor.cancel();
                        if (editor.draft_present()) {
                            console.write("draft: ");
                            console.write(editor.draft());
                            console.put('\n');
                        }
                    }
                } else command(line, process, console,
                    service::capability(info, bootstrap::imports::ServiceControl),
                    service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
                    service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE));
                used = 0;
                overflow = false;
                myos::console::prompt(output, editor.active() ? "edit> " : "myos> ");
            } else if (byte == '\b' || byte == 127) {
                if (used != 0) { --used; console.write("\b \b"); }
            } else if (byte >= 32 && byte < 127) {
                if (used + 1 < sizeof(line)) { line[used++] = byte; console.put(byte); }
                else overflow = true;
            }
        }
    }
}
