#include <libk/fmt.hpp>
#include <user/lib/file_client.hpp>

namespace {
using namespace myos;
uint64_t child = 0;
files::Client filesystem;

void command(char* line, service::Connection& process, service::Console& console) {
    while (*line == ' ') ++line;
    char* argument = line;
    while (*argument != '\0' && *argument != ' ') ++argument;
    if (*argument != '\0') *argument++ = '\0';
    while (*argument == ' ') ++argument;
    if (*line == '\0') return;
    if (service::equal(line, "help")) {
        console.write("help | ls | cat FILE | run hello | spawn hello | wait | stop\n");
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
    if (service::equal(line, "cat")) {
        files::File file;
        auto status = filesystem.open(argument, service::length(argument), file);
        if (status == MYOS_STATUS_OK) {
            status = filesystem.read(file, [&](uint64_t, const uint8_t* data, size_t bytes) {
                console.write(reinterpret_cast<const char*>(data), bytes);
            });
            const auto closed = filesystem.close(file);
            if (status == MYOS_STATUS_OK) status = closed;
        }
        if (status != MYOS_STATUS_OK) (void)libk::fmt::format_to<"file error: {}\n">(console, status);
        return;
    }
    const bool run = service::equal(line, "run");
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
    request.id = child;
    request.size = service::length(argument);
    if (request.size >= sizeof(request.data)) { console.write("argument too long\n"); return; }
    service::copy(request.data, argument, request.size);
    service::require(process.send(request).status);
    service::Message reply{};
    service::require(process.receive(reply).status);
    if ((run || spawn) && reply.status == MYOS_STATUS_OK) {
        child = reply.id;
        if (run) {
            request = {};
            request.operation = static_cast<uint64_t>(service::Process::Wait);
            request.id = child;
            service::require(process.send(request).status);
            service::require(process.receive(reply).status);
            child = 0;
        } else {
            static_cast<void>(libk::fmt::format_to<"task: {}\n">(console, child));
            return;
        }
    } else if (wait || stop) {
        child = 0;
    }
    static_cast<void>(libk::fmt::format_to<"exit: {}\n">(console, reply.status));
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    service::Console console{service::capability(info, MYOS_BOOTSTRAP_CAP_CONSOLE_OUTPUT)};
    const auto input = service::capability(info, MYOS_BOOTSTRAP_CAP_CONSOLE_INPUT);
    service::Connection process{
        service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_CHANNEL),
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
