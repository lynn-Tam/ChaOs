#include <user/lib/imports.hpp>
#include <user/lib/clock.hpp>
#include <libk/fmt.hpp>
#include <user/lib/file_client.hpp>

namespace {
using namespace myos;
uint64_t children[4]{};
uint64_t latest{};
void forget(uint64_t id) {
    for (auto& child : children) if (child == id) child = 0;
    if (latest == id) { latest = 0; for (auto child : children) if (child) latest = child; }
}
files::Client filesystem;

void command(char* line, service::Connection& process, service::Console& console) {
    while (*line == ' ') ++line;
    char* argument = line;
    while (*argument != '\0' && *argument != ' ') ++argument;
    if (*argument != '\0') *argument++ = '\0';
    while (*argument == ' ') ++argument;
    if (*line == '\0') return;
    if (service::equal(line, "help")) {
        console.write("help | ls | cat FILE | run hello | spawn hello | jobs | wait [ID] [MS] | stop [ID]\n");
        return;
    }
    if (service::equal(line, "ls")) {
        io::ControlMessage reply;
        do {
            const auto status = filesystem.list(reply);
            if (status != MYOS_STATUS_OK) {
                (void)libk::fmt::format_to<"file error: {}\n">(console, status);
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
    const bool run = cat || service::equal(line, "run");
    const bool spawn = service::equal(line, "spawn");
    const bool wait = service::equal(line, "wait");
    const bool stop = service::equal(line, "stop");
    if (!run && !spawn && !wait && !stop) {
        console.write("unknown command\n");
        return;
    }
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
        bootstrap::Arguments arguments;
        if (cat) (void)arguments.append("cat", 3);
        while (*argument != 0) {
            const char* first = argument;
            while (*argument != 0 && *argument != ' ') ++argument;
            if (!arguments.append(first, argument - first)) { console.write("argument too long\n"); return; }
            while (*argument == ' ') ++argument;
        }
        request.size = arguments.data().size;
        if (request.size > sizeof(request.data)) { console.write("argument too long\n"); return; }
        service::copy(request.data, arguments.data().bytes, request.size);
    }
    service::require(process.send(request).status);
    service::Message reply{};
    service::require(process.receive(reply).status);
    if ((run || spawn) && reply.status == MYOS_STATUS_OK) {
        const auto child = reply.id;
        latest = child;
        if (run) {
            request = {};
            request.operation = static_cast<uint64_t>(service::Process::Wait);
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
            service::require(process.send(request).status);
            service::require(process.receive(reply).status);
            forget(child);
        } else {
            for (auto& slot : children) if (slot == 0) { slot = child; break; }
            static_cast<void>(libk::fmt::format_to<"task: {}\n">(console, child));
            return;
        }
    } else if (wait || stop) {
        if (reply.status != MYOS_STATUS_TIMED_OUT) forget(request.id);
    }
    static_cast<void>(libk::fmt::format_to<"exit: {}\n">(console, reply.status));
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    service::Console console{service::capability(info, myos::bootstrap::imports::ConsoleOutput)};
    const auto input = service::capability(info, myos::bootstrap::imports::ConsoleInput);
    service::Connection process{
        service::capability(info, myos::bootstrap::imports::Process),
        service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION)};
    service::require(filesystem.connect(info));
    console.write("myos native shell\nmyos> ");
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
                else command(line, process, console);
                used = 0;
                overflow = false;
                console.write("myos> ");
            } else if (byte == '\b' || byte == 127) {
                if (used != 0) { --used; console.write("\b \b"); }
            } else if (byte >= 32 && byte < 127) {
                if (used + 1 < sizeof(line)) { line[used++] = byte; console.put(byte); }
                else overflow = true;
            }
        }
    }
}
