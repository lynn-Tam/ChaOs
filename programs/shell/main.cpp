#include <user/lib/stream.hpp>
#include <user/lib/console.hpp>
#include <user/lib/imports.hpp>
#include <user/lib/clock.hpp>
#include <libk/fmt.hpp>
#include <user/lib/terminal.hpp>

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
bool storage_available{};

void error(stream::Writer& console, myos_status_t status) {
    (void)libk::fmt::format_to<"error: {}\n">(console, status);
}

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
        console.write("help | ls [DIR] | cat FILE | mkfs [32_HEX_ID] | edit FILE | run/spawn PROGRAM [ARGS] | jobs | wait [ID] [MS] | stop [ID] | restart SERVICE\n");
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
    if (service::equal(line, "jobs")) {
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
    request.operation = static_cast<uint64_t>(run ? service::Process::ForegroundSpawn
        : spawn ? service::Process::Spawn
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
        if (file_command) {
            (void)words.append("fs", 2);
            if (service::equal(line, "cp")) (void)words.append("copy", 4);
            else (void)words.append(line, service::length(line));
        } else if (!explicit_run && !spawn)
            (void)words.append(line, service::length(line));
        size_t split{};
        if (!arguments(argument, words, split) || words.count() == 0) {
            console.write("invalid command line\n"); return;
        }
        request.size = words.data().size;
        if (split != 0) {
            if (split == request.size) { console.write("invalid pipeline\n"); return; }
            request.operation = static_cast<uint64_t>(run
                ? service::Process::ForegroundPipeline : service::Process::Pipeline);
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
        if (reply.operation == static_cast<uint64_t>(service::Process::Pipeline)
            || reply.operation == static_cast<uint64_t>(service::Process::ForegroundPipeline)) {
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
    storage_available = info.argument_count() == 1 && service::equal(info.argument(0), "storage");
    console.write("myos native shell\n");
    myos::console::prompt(output, "myos> ");
    terminal::LineReader reader{input, output};
    char line[128]{};
    for (;;) {
        const auto result = reader.read(line);
        if (result == terminal::LineResult::TooLong) console.write("line too long\n");
        else if (result == terminal::LineResult::Line)
            command(line, process, console,
                service::capability(info, bootstrap::imports::ServiceControl),
                service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
                service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE));
        myos::console::prompt(output, "myos> ");
    }
}
