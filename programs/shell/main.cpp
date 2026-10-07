#include <servers/process_server/protocol.hpp>
#include <servers/runtime/service.hpp>
#include <libk/parse.hpp>
#include <sys/channel.hpp>
#include <programs/shell/terminal.hpp>
#include <sys/start.hpp>
#include <sys/clock.hpp>
#include <libk/fmt.hpp>

namespace {
using namespace sys;
uint64_t children[4]{};
uint64_t latest{};
void remember(uint64_t id) {
    for (auto& child : children) if (!child) { child = id; latest = id; return; }
    exit(STATUS_INTERNAL);
}
void forget(uint64_t id) {
    for (auto& child : children) if (child == id) child = 0;
    if (latest == id) { latest = 0; for (auto child : children) if (child) latest = child; }
}
bool storage_available{};

void error(stream::Writer& console, status_t status) {
    (void)libk::fmt::format_to<"error: {}\n">(console, status);
}

// Parse one bounded command line into the existing argv wire form. A pipe is
// syntax only outside quotes; escaped bytes and empty quoted words are data.
auto arguments(char* text, boot::Args& output, size_t& split,
    char (&target)[128], bool& append_output) noexcept -> bool {
    char word[128]{};
    size_t used{};
    bool present{};
    bool redirect{}, target_present{};
    char quote{};
    auto append = [&]() noexcept -> bool {
        if (!present) return true;
        bool ok{};
        if (redirect) {
            ok = !target_present && used != 0;
            if (ok) { service::copy(target, word, used); target[used] = 0; target_present = true; }
        } else ok = output.append(word, used);
        used = 0;
        present = false;
        return ok;
    };
    for (;;) {
        const char ch = *text++;
        if (ch == '\0') return quote == 0 && append() && (!redirect || target_present);
        if (ch == '\\' && *text != '\0') {
            if (used == sizeof(word)) return false;
            word[used++] = *text++;
            present = true;
        } else if (ch == '\'' || ch == '"') {
            if (quote == 0) { quote = ch; present = true; }
            else if (quote == ch) quote = 0;
            else { if (used == sizeof(word)) return false; word[used++] = ch; }
        } else if (quote == 0 && (ch == ' ' || ch == '|' || ch == '>')) {
            if (!append()) return false;
            if (ch == '|') {
                if (split != 0 || redirect || output.count() == 0) return false;
                split = output.data().size;
            } else if (ch == '>') {
                if (split != 0 || redirect || output.count() == 0) return false;
                redirect = true;
                if (*text == '>') { append_output = true; ++text; }
            }
        } else {
            if (used == sizeof(word)) return false;
            word[used++] = ch;
            present = true;
        }
    }
}

void command(char* line, service::Connection& process, stream::Writer& console,
    cap_t control, cap_t pool, cap_t cspace) {
    while (*line == ' ') ++line;
    char name[128]{};
    char* argument = line;
    while (*argument != '\0' && *argument != ' ' && *argument != '|' && *argument != '>') ++argument;
    const size_t name_size = argument - line;
    service::copy(name, line, name_size);
    line = name;
    while (*argument == ' ') ++argument;
    if (*line == '\0') return;
    if (service::equal(line, "help")) {
        if (*argument != '\0') { console.write("invalid command line\n"); return; }
        console.write("help | ls [DIR] | cat FILE | mkfs [32_HEX_ID] | edit FILE | run/spawn PROGRAM [ARGS] | jobs | wait [ID] [MS] | stop [ID] | restart SERVICE\n");
        if (storage_available) console.write("PROGRAM > FILE (replace) | PROGRAM >> FILE (append)\n");
        if (storage_available) console.write("touch FILE | write/append FILE TEXT | mkdir/rm PATH | mv OLD NEW | cp SOURCE DEST | stat FILE | fs device/volid; / is data, /boot is read-only\n");
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
        const auto pair = channel_create(pool, 1, CHANNEL_MAX_WORDS, 0, 1);
        if (pair.status != STATUS_OK) {
            error(console, pair.status);
            return;
        }
        cap::OwnedCap inbox_root{{pair.value, 0}}, outbox_root{{pair.value2, 0}};
        const auto reader = channel_mint(inbox_root.selector(), cspace, 1, RIGHT_RECEIVE);
        const auto sender = channel_mint(outbox_root.selector(), cspace, 1,
            RIGHT_SEND | RIGHT_DUPLICATE);
        cap::OwnedCap inbox, outbox;
        if (reader.status == STATUS_OK) inbox = cap::OwnedCap{{reader.value, 0}};
        if (sender.status == STATUS_OK) outbox = cap::OwnedCap{{sender.value, 0}};
        auto status = reader.status != STATUS_OK ? reader.status : sender.status;
        if (status == STATUS_OK) {
            status = service::send_cap(control, request, outbox.selector(), RIGHT_SEND).status;
            if (status == STATUS_OK) {
                console.write("service restart requested\n");
                service::Message reply{};
                status = service::receive(inbox.selector(), reply).status;
                if (status == STATUS_OK) status = reply.status;
            }
        }
        const auto destroyed = object_destroy(inbox_root.selector()).status;
        if (destroyed != STATUS_OK) status = destroyed;
        if (status != STATUS_OK && status != STATUS_BUSY) error(console, status);
        return;
    }
    if (service::equal(line, "jobs")) {
        if (*argument != '\0') { console.write("invalid command line\n"); return; }
        for (auto child : children) if (child) (void)libk::fmt::format_to<"task: {}\n">(console, child);
        return;
    }
    const bool file_command = storage_available && (
        service::equal(line, "stat") || service::equal(line, "touch")
        || service::equal(line, "write")
        || service::equal(line, "append") || service::equal(line, "mkdir")
        || service::equal(line, "rm") || service::equal(line, "mv")
        || service::equal(line, "cp"));
    const bool explicit_run = service::equal(line, "run");
    const bool spawn = service::equal(line, "spawn");
    const bool wait = service::equal(line, "wait");
    const bool stop = service::equal(line, "stop");
    const bool run = !spawn && !wait && !stop;
    service::Message request{};
    request.operation = static_cast<uint64_t>(run ? process::op::ForegroundSpawn
        : spawn ? process::op::Spawn
        : wait ? process::op::Wait : process::op::Stop);
    request.id = latest;
    if ((wait || stop) && *argument != 0) {
        char* end = argument;
        while (*end && *end != ' ') ++end;
        if (*end) *end++ = 0;
        const auto parsed = libk::parse<uint64_t>(argument);
        if (!parsed) { console.write("invalid task\n"); return; }
        request.id = *parsed;
        while (*end == ' ') ++end;
        if (wait && *end) {
            const auto duration = libk::parse<uint64_t>(end);
            Clock clock;
            service::require(clock.open());
            const auto deadline = duration ? clock.after_ms(*duration) : std::nullopt;
            if (!deadline) { console.write("invalid timeout\n"); return; }
            request.size = sizeof(uint64_t);
            service::copy(request.data, &*deadline, request.size);
        }
    }
    if (run || spawn) {
        boot::Args words;
        if (file_command) {
            (void)words.append("fs", 2);
            if (service::equal(line, "cp")) (void)words.append("copy", 4);
            else (void)words.append(line, service::length(line));
        } else if (!explicit_run && !spawn)
            (void)words.append(line, service::length(line));
        size_t split{};
        char target[128]{};
        bool append_output{};
        if (!arguments(argument, words, split, target, append_output) || words.count() == 0) {
            console.write("invalid command line\n"); return;
        }
        if (*target != '\0') {
            split = words.data().size;
            if (!words.append("put", 3)
                || (append_output && !words.append("-a", 2))
                || !words.append(target, service::length(target))) {
                console.write("argument too long\n"); return;
            }
        }
        request.size = words.data().size;
        if (split != 0) {
            if (split == request.size) { console.write("invalid pipeline\n"); return; }
            request.operation = static_cast<uint64_t>(run
                ? process::op::ForegroundPipeline : process::op::Pipeline);
            request.id = split;
        }
        if (request.size > sizeof(request.data)) { console.write("argument too long\n"); return; }
        service::copy(request.data, words.data().bytes, request.size);
    }
    service::require(process.send(request).status);
    service::Message reply{};
    service::require(process.receive(reply).status);
    if ((run || spawn) && reply.status == STATUS_OK) {
        const auto child = reply.id;
        uint64_t consumer{};
        if (reply.operation == static_cast<uint64_t>(process::op::Pipeline)
            || reply.operation == static_cast<uint64_t>(process::op::ForegroundPipeline)) {
            if (reply.size != sizeof(consumer)) exit(STATUS_PEER_FAULT);
            service::copy(&consumer, reply.data, sizeof(consumer));
        }
        if (run) {
            service::Message wait_request{.operation = static_cast<uint64_t>(process::op::Wait),
                .id = consumer ? consumer : child};
            service::require(process.send(wait_request).status);
            service::require(process.receive(reply).status);
            const auto status = reply.status;
            if (consumer) {
                wait_request.id = child;
                service::require(process.send(wait_request).status);
                service::require(process.receive(reply).status);
                if (reply.status != STATUS_OK) error(console, reply.status);
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
        if (reply.status != STATUS_TIMED_OUT) forget(request.id);
    }
    if (reply.status != STATUS_OK
        && !(stop && reply.status == STATUS_CANCELED)) error(console, reply.status);
}
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size, const char* arg_data, size_t arg_size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    boot::Args args;
    if (!args.decode(arg_data, arg_size)) exit(STATUS_BAD_ARGS);
    const auto output = service::capability(info, boot::ConsoleOutput);
    stream::Writer console{output};
    const auto input = service::capability(info, boot::ConsoleInput);
    service::Connection process{
        service::capability(info, boot::Process),
        service::capability(info, BOOT_EVENTS)};
    storage_available = args.count() == 1 && service::equal(args.argument(0), "storage");
    console.write("myos native shell\n");
    sys::console::prompt(output, "myos> ");
    terminal::LineReader reader{input, output};
    char line[128]{};
    for (;;) {
        const auto result = reader.read(line);
        if (result == terminal::LineResult::TooLong) console.write("line too long\n");
        else if (result == terminal::LineResult::Line)
            command(line, process, console,
                service::capability(info, boot::ServiceControl),
                service::capability(info, BOOT_POOL),
                service::capability(info, BOOT_CSPACE));
        sys::console::prompt(output, "myos> ");
    }
}
