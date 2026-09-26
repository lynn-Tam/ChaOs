#include <user/lib/stream.hpp>
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

void storage_error(stream::Writer& console, myos_status_t status) {
    (void)libk::fmt::format_to<"storage error: {}\n">(console, status);
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
        console.write("help | ls | cat FILE | run hello | spawn hello | jobs | wait [ID] [MS] | stop [ID] | restart SERVICE\n");
        if (storage_available)
            console.write("wdevice | mkfs | wls [DIR] | wcat FILE | wstat FILE | write FILE TEXT | append FILE TEXT | save BOOTFILE FILE | mkdir DIR | rm FILE | mv OLD NEW\n");
        return;
    }
    if (service::equal(line, "wdevice") || service::equal(line, "mkfs") || service::equal(line, "wls")
        || service::equal(line, "wcat") || service::equal(line, "wstat")
        || service::equal(line, "write") || service::equal(line, "append")
        || service::equal(line, "save") || service::equal(line, "mkdir")
        || service::equal(line, "rm") || service::equal(line, "mv")) {
        if (!storage_available) { console.write("no writable volume\n"); return; }
        myos_status_t status = MYOS_STATUS_OK;
        if (service::equal(line, "wdevice")) {
            if (*argument != '\0') status = MYOS_STATUS_BAD_ARGS;
            else {
                uint8_t id[20]{};
                status = store_client.device_id(id);
                if (status == MYOS_STATUS_OK) {
                    constexpr char digits[] = "0123456789abcdef";
                    char hex[sizeof(id) * 2];
                    for (size_t i = 0; i < sizeof(id); ++i) {
                        hex[i * 2] = digits[id[i] >> 4];
                        hex[i * 2 + 1] = digits[id[i] & 15];
                    }
                    console.write(hex, sizeof(hex));
                    console.put('\n');
                    return;
                }
            }
        } else if (service::equal(line, "mkfs")) {
            if (*argument != '\0') status = MYOS_STATUS_BAD_ARGS;
            else status = store_client.format();
        } else if (service::equal(line, "wls")) {
            io::ControlMessage entry{};
            do {
                status = store_client.list(entry, argument);
                if (status != MYOS_STATUS_OK) break;
                if (entry.size != 0) { console.write(entry.data, entry.size); console.put('\n'); }
            } while (entry.value != 0);
        } else if (service::equal(line, "mkdir")) status = store_client.mkdir(argument);
        else if (service::equal(line, "rm")) status = store_client.remove(argument);
        else if (service::equal(line, "mv")) {
            char* next = argument;
            while (*next != '\0' && *next != ' ') ++next;
            if (*next == '\0') status = MYOS_STATUS_BAD_ARGS;
            else {
                *next++ = '\0';
                while (*next == ' ') ++next;
                status = store_client.rename(argument, next);
            }
        } else if (service::equal(line, "wcat") || service::equal(line, "wstat")) {
            store::File file{};
            status = store_client.open(argument, store::Read, file);
            if (status == MYOS_STATUS_OK) {
                if (service::equal(line, "wstat"))
                    (void)libk::fmt::format_to<"{} bytes\n">(console, file.size);
                else status = store_client.read(file,
                    [&](uint64_t, const uint8_t* data, size_t count) {
                        console.write(reinterpret_cast<const char*>(data), count);
                    });
                const auto closed = store_client.close(file);
                if (status == MYOS_STATUS_OK) status = closed;
            }
        } else {
            char* next = argument;
            while (*next != '\0' && *next != ' ') ++next;
            if (*next == '\0') status = MYOS_STATUS_BAD_ARGS;
            else {
                *next++ = '\0';
                while (*next == ' ') ++next;
                if (service::equal(line, "write") || service::equal(line, "append")) {
                    store::File file{};
                    const bool append = service::equal(line, "append");
                    status = store_client.open(argument, append
                        ? store::Read | store::Write | store::Create
                        : store::Write | store::Create | store::Truncate, file);
                    if (status == MYOS_STATUS_OK) {
                        status = store_client.write(file, append ? file.size : 0,
                            reinterpret_cast<const uint8_t*>(next), service::length(next));
                        if (status == MYOS_STATUS_OK) status = store_client.sync(file);
                        const auto closed = store_client.close(file);
                        if (status == MYOS_STATUS_OK) status = closed;
                    }
                } else {
                    files::File source{};
                    status = filesystem.open(argument, service::length(argument), source);
                    if (status == MYOS_STATUS_OK) {
                        store::File target{};
                        status = store_client.open(next,
                            store::Write | store::Create | store::Truncate, target);
                        if (status == MYOS_STATUS_OK) {
                            myos_status_t copied = MYOS_STATUS_OK;
                            status = filesystem.read(source,
                                [&](uint64_t offset, const uint8_t* data, size_t count) {
                                    if (copied == MYOS_STATUS_OK)
                                        copied = store_client.write(target, offset, data, count);
                                });
                            if (status == MYOS_STATUS_OK) status = copied;
                            if (status == MYOS_STATUS_OK) status = store_client.sync(target);
                            const auto closed = store_client.close(target);
                            if (status == MYOS_STATUS_OK) status = closed;
                        }
                        const auto closed = filesystem.close(source);
                        if (status == MYOS_STATUS_OK) status = closed;
                    }
                }
            }
        }
        if (status != MYOS_STATUS_OK) storage_error(console, status);
        else console.write("ok\n");
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
            (void)libk::fmt::format_to<"service restart: {}\n">(console, pair.status);
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
        if (status != MYOS_STATUS_OK && status != MYOS_STATUS_BUSY)
            (void)libk::fmt::format_to<"service restart: {}\n">(console, status);
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
        size_t split{};
        while (*argument != 0) {
            const char* first = argument;
            while (*argument != 0 && *argument != ' ') ++argument;
            if (argument - first == 1 && *first == '|') {
                if (split || arguments.count() == 0) { console.write("invalid pipeline\n"); return; }
                split = arguments.data().size;
                while (*argument == ' ') ++argument;
                continue;
            }
            if (!arguments.append(first, argument - first)) { console.write("argument too long\n"); return; }
            while (*argument == ' ') ++argument;
        }
        request.size = arguments.data().size;
        if (split != 0) {
            if (split == request.size) { console.write("invalid pipeline\n"); return; }
            request.operation = static_cast<uint64_t>(service::Process::Pipeline);
            request.id = split;
        }
        if (request.size > sizeof(request.data)) { console.write("argument too long\n"); return; }
        service::copy(request.data, arguments.data().bytes, request.size);
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
                if (reply.status != MYOS_STATUS_OK)
                    (void)libk::fmt::format_to<"producer: {}\n">(console, reply.status);
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
    static_cast<void>(libk::fmt::format_to<"exit: {}\n">(console, reply.status));
}
}

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    stream::Writer console{service::capability(info, myos::bootstrap::imports::ConsoleOutput)};
    const auto input = service::capability(info, myos::bootstrap::imports::ConsoleInput);
    service::Connection process{
        service::capability(info, myos::bootstrap::imports::Process),
        service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION)};
    service::require(filesystem.connect(info));
    storage_available = info.selector(bootstrap::imports::Store) != 0;
    if (storage_available) service::require(store_client.connect(info));
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
                else command(line, process, console,
                    service::capability(info, bootstrap::imports::ServiceControl),
                    service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL),
                    service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE));
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
