#include <string_view>
#include <string.h>
#include <sys/protocol.hpp>
#include <sys/channel.hpp>
#include <libk/parse.hpp>
#include <sys/start.hpp>
#include <sys/clock.hpp>
#include <libk/fmt.hpp>

static void prompt(cap_t output, const char* text) noexcept {
    sys::Message message{.operation = static_cast<uint64_t>(console::Operation::Prompt)};
    message.size = ::strlen(text);
    if (message.size > sizeof(message.data)) sys::exit(STATUS_BAD_ARGS);
    ::memcpy(message.data, text, message.size);
    sys::require(sys::send(output, message).status);
}


enum class LineResult { Line, TooLong, End };

class LineReader final {
    cap_t input_;
    sys::stream::Writer output_;
public:
    LineReader(cap_t input, cap_t output) noexcept
        : input_(input), output_(output) {}

    auto read(char (&line)[128]) noexcept -> LineResult {
        size_t used{};
        bool overflow{};
        for (;;) {
            sys::Message message{};
            sys::require(sys::receive(input_, message).status);
            if (message.operation == static_cast<uint64_t>(sys::stream::Frame::End))
                return LineResult::End;
            if (message.operation != static_cast<uint64_t>(sys::stream::Frame::Data))
                sys::exit(STATUS_PEER_FAULT);
            for (size_t i = 0; i < message.size; ++i) {
                const char byte = message.data[i];
                if (byte == '\n') {
                    output_.put('\n');
                    line[used] = '\0';
                    return overflow ? LineResult::TooLong : LineResult::Line;
                }
                if (byte == '\b' || byte == 127) {
                    if (used != 0) { --used; output_.write("\b \b"); }
                } else if (byte >= 32 && byte < 127) {
                    if (used + 1 < sizeof(line)) { line[used++] = byte; output_.put(byte); }
                    else overflow = true;
                }
            }
        }
    }
};

namespace {
using namespace sys;
uint64_t children[4]{};
uint64_t latest{};
void remember(uint64_t id) {
    for (auto& child : children) if (!child) { child = id; latest = id; return; }
    exit(STATUS_INTERNAL);
}
struct Pipe {
    cap_owner roots[2], ends[2];
    uint64_t producer{}, consumer{};
    auto active() const noexcept -> bool { return bool(roots[0]); }
    auto open(cap_t pool, cap_t cspace) noexcept -> status_t {
        const auto pair = channel_create(pool, 4, CHANNEL_MAX_WORDS, 0, 2);
        if (pair.value) roots[0] = cap_owner{pair.value};
        if (pair.value2) roots[1] = cap_owner{pair.value2};
        if (pair.status != STATUS_OK) return pair.status;
        for (size_t i = 0; i < 2; ++i) {
            const auto minted = channel_mint(roots[i].selector(), cspace, 1,
                (i == 0 ? RIGHT_SEND : RIGHT_RECEIVE) | RIGHT_DUPLICATE);
            if (minted.status != STATUS_OK) return minted.status;
            ends[i] = cap_owner{minted.value};
        }
        return STATUS_OK;
    }
    void close() noexcept {
        if (active()) require(object_destroy(roots[0].selector()).status);
        ends[0] = {}; ends[1] = {}; roots[0] = {}; roots[1] = {}; producer = consumer = 0;
    }
};
Pipe pipes[4];
void forget(uint64_t id) {
    for (auto& child : children) if (child == id) child = 0;
    for (auto& pipe : pipes) if (pipe.active()) {
        if (pipe.producer == id) pipe.producer = 0;
        if (pipe.consumer == id) {
            pipe.consumer = 0;
            require(channel_close(pipe.roots[0].selector()).status);
        }
        if (!pipe.producer && !pipe.consumer) pipe.close();
    }
    if (latest == id) { latest = 0; for (auto child : children) if (child) latest = child; }
}
bool storage_available{};

void error(stream::Writer& console, status_t status) {
    (void)libk::fmt::format_to<"error: {}\n">(console, status);
}

// Parse one bounded command line into the existing argv wire form. A pipe is
// syntax only outside quotes; escaped bytes and empty quoted words are data.
auto arguments(char* text, boot::args& output, std::array<size_t, 5>& cuts, size_t& stages,
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
            if (ok) { ::memcpy(target, word, used); target[used] = 0; target_present = true; }
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
                if (stages == 4 || redirect || output.size() == cuts[stages - 1]) return false;
                cuts[stages++] = output.size();
            } else if (ch == '>') {
                if (redirect || output.size() == cuts[stages - 1]) return false;
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

void command(char* line, sys::Connection& process, stream::Writer& console,
    cap_t control, cap_t pool, cap_t cspace, cap_t input, cap_t output) {
    while (*line == ' ') ++line;
    char name[128]{};
    char* argument = line;
    while (*argument != '\0' && *argument != ' ' && *argument != '|' && *argument != '>') ++argument;
    const size_t name_size = argument - line;
    ::memcpy(name, line, name_size);
    line = name;
    while (*argument == ' ') ++argument;
    if (*line == '\0') return;
    if ((std::string_view{line} == "help")) {
        if (*argument != '\0') { console.write("invalid command line\n"); return; }
        console.write("help | ls [DIR] | cat FILE | mkfs [32_HEX_ID] | edit FILE | run/spawn PROGRAM [ARGS] | jobs | wait [ID] [MS] | stop [ID] | restart SERVICE\n");
        if (storage_available) console.write("PROGRAM > FILE (replace) | PROGRAM >> FILE (append)\n");
        if (storage_available) console.write("touch FILE | write/append FILE TEXT | mkdir/rm PATH | mv OLD NEW | cp SOURCE DEST | stat FILE | fs device/volid; / is data, /boot is read-only\n");
        return;
    }
    if ((std::string_view{line} == "restart")) {
        sys::Message request{};
        request.size = ::strlen(argument);
        if (request.size == 0 || request.size >= sizeof(request.data)) {
            console.write("invalid service\n");
            return;
        }
        ::memcpy(request.data, argument, request.size);
        const auto pair = channel_create(pool, 1, CHANNEL_MAX_WORDS, 0, 1);
        if (pair.status != STATUS_OK) {
            error(console, pair.status);
            return;
        }
        cap_owner inbox_root{pair.value}, outbox_root{pair.value2};
        const auto reader = channel_mint(inbox_root.selector(), cspace, 1, RIGHT_RECEIVE);
        const auto sender = channel_mint(outbox_root.selector(), cspace, 1,
            RIGHT_SEND | RIGHT_DUPLICATE);
        cap_owner inbox, outbox;
        if (reader.status == STATUS_OK) inbox = cap_owner{reader.value};
        if (sender.status == STATUS_OK) outbox = cap_owner{sender.value};
        auto status = reader.status != STATUS_OK ? reader.status : sender.status;
        if (status == STATUS_OK) {
            status = sys::send_cap(control, request, outbox.selector(), RIGHT_SEND).status;
            if (status == STATUS_OK) {
                console.write("service restart requested\n");
                sys::Message reply{};
                status = sys::receive(inbox.selector(), reply).status;
                if (status == STATUS_OK) status = reply.status;
            }
        }
        const auto destroyed = object_destroy(inbox_root.selector()).status;
        if (destroyed != STATUS_OK) status = destroyed;
        if (status != STATUS_OK && status != STATUS_BUSY) error(console, status);
        return;
    }
    if ((std::string_view{line} == "jobs")) {
        if (*argument != '\0') { console.write("invalid command line\n"); return; }
        for (auto child : children) if (child) (void)libk::fmt::format_to<"task: {}\n">(console, child);
        return;
    }
    const bool file_command = storage_available && (
        (std::string_view{line} == "stat") || (std::string_view{line} == "touch")
        || (std::string_view{line} == "write")
        || (std::string_view{line} == "append") || (std::string_view{line} == "mkdir")
        || (std::string_view{line} == "rm") || (std::string_view{line} == "mv")
        || (std::string_view{line} == "cp"));
    const bool explicit_run = (std::string_view{line} == "run");
    const bool spawn = (std::string_view{line} == "spawn");
    const bool wait = (std::string_view{line} == "wait");
    const bool stop = (std::string_view{line} == "stop");
    const bool run = !spawn && !wait && !stop;
    sys::Message request{};
    request.operation = static_cast<uint64_t>(run || spawn ? proc::op::Spawn
        : wait ? proc::op::Wait : proc::op::Stop);
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
            sys::require(clock.open());
            const auto deadline = duration ? clock.after_ms(*duration) : std::nullopt;
            if (!deadline) { console.write("invalid timeout\n"); return; }
            request.size = sizeof(uint64_t);
            ::memcpy(request.data, &*deadline, request.size);
        }
    }
    if (run || spawn) {
        boot::args words;
        if (file_command) {
            (void)words.append("fs", 2);
            if ((std::string_view{line} == "cp")) (void)words.append("copy", 4);
            else (void)words.append(line, ::strlen(line));
        } else if (!explicit_run && !spawn)
            (void)words.append(line, ::strlen(line));
        std::array<size_t, 5> cuts{};
        size_t stages = 1;
        char target[128]{};
        bool append_output{};
        if (!arguments(argument, words, cuts, stages, target, append_output)
            || words.size() == cuts[stages - 1]) {
            console.write("invalid command line\n"); return;
        }
        if (*target) {
            if (stages == 4) { error(console, STATUS_BUSY); return; }
            cuts[stages++] = words.size();
            if (!words.append("put", 3) || (append_output && !words.append("-a", 2))
                || !words.append(target, ::strlen(target))) {
                console.write("argument too long\n"); return;
            }
        }
        cuts[stages] = words.size();
        if (std::count(std::begin(children), std::end(children), 0) < ptrdiff_t(stages)) {
            error(console, STATUS_BUSY); return;
        }
        std::array<Pipe*, 4> links{};
        const size_t first = run ? 1 : 0;
        status_t status = STATUS_OK;
        for (size_t i = first; i < stages; ++i) {
            for (auto& p : pipes) if (!p.active()) { links[i] = &p; break; }
            if (!links[i]) { status = STATUS_BUSY; break; }
            status = links[i]->open(pool, cspace);
            if (status != STATUS_OK) break;
        }
        // A background job starts at EOF rather than consuming shell input.
        if (status == STATUS_OK && !run)
            status = sys::send(links[0]->ends[0].selector(), {
                .operation = static_cast<uint64_t>(stream::Frame::End)}).status;
        std::array<uint64_t, 4> ids{};
        for (size_t i = 0; i < stages && status == STATUS_OK; ++i) {
            sys::Message launch{.operation = static_cast<uint64_t>(proc::op::Spawn),
                .id = 7 | (i + 1 < stages ? proc::end_stdout : 0),
                .size = cuts[i + 1] - cuts[i]};
            if (launch.size > sizeof(launch.data)) { status = STATUS_BAD_ARGS; break; }
            ::memcpy(launch.data, words.data() + cuts[i], launch.size);
            const std::array<CapXfer, 3> stdio{{
                {i == 0 && run ? input : links[i]->ends[1].selector(), RIGHT_RECEIVE | RIGHT_DUPLICATE, CAP_COPY, 0},
                {i + 1 == stages ? output : links[i + 1]->ends[0].selector(), RIGHT_SEND | RIGHT_DUPLICATE, CAP_COPY, 0},
                {output, RIGHT_SEND | RIGHT_DUPLICATE, CAP_COPY, 0},
            }};
            status = process.send(launch, stdio).status;
            if (status == STATUS_OK) status = process.receive(request).status;
            if (status == STATUS_OK) status = request.status;
            if (status != STATUS_OK) break;
            ids[i] = request.id;
            remember(ids[i]);
            if (links[i]) links[i]->consumer = ids[i];
            if (i + 1 < stages) links[i + 1]->producer = ids[i];
        }
        if (status != STATUS_OK) {
            // Revoke edges first so a partial pipeline cannot remain blocked.
            for (auto* p : links) if (p && p->active()) require(channel_close(p->roots[0].selector()).status);
            for (auto id : ids) if (id) {
                require(process.send({.operation = static_cast<uint64_t>(proc::op::Stop), .id = id}).status);
                sys::Message reply;
                require(process.receive(reply).status);
                forget(id);
            }
            for (auto* p : links) if (p) p->close();
            error(console, status); return;
        }
        if (!run) {
            for (auto id : ids) if (id) (void)libk::fmt::format_to<"task: {}\n">(console, id);
            return;
        }
        // Reap consumers first: an early consumer exit closes the edge and
        // wakes a backpressured producer. proc publishes ordered End on exit.
        for (size_t i = stages; i-- > 0;) {
            require(process.send({.operation = static_cast<uint64_t>(proc::op::Wait), .id = ids[i]}).status);
            sys::Message reply;
            require(process.receive(reply).status);
            forget(ids[i]);
            if (reply.status != STATUS_OK) error(console, reply.status);
        }
        return;
    }
    require(process.send(request).status);
    sys::Message reply;
    require(process.receive(reply).status);
    if (wait || stop) {
        if (reply.status != STATUS_TIMED_OUT) forget(request.id);
    }
    if (reply.status != STATUS_OK
        && !(stop && reply.status == STATUS_CANCELED)) error(console, reply.status);
}
}

extern "C" [[noreturn]] void user_main(const void* address, word_t size, const char* arg_data, size_t arg_size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    boot::args args;
    if (!args.decode(arg_data, arg_size)) exit(STATUS_BAD_ARGS);
    const auto output = sys::capability(info, boot::ConsoleOutput);
    stream::Writer console{output};
    const auto input = sys::capability(info, boot::ConsoleInput);
    sys::Connection process{
        sys::capability(info, boot::Process),
        sys::capability(info, BOOT_EVENTS)};
    storage_available = args.count() == 1 && (std::string_view{args.argument(0)} == "storage");
    console.write("myos native shell\n");
    prompt(output, "myos> ");
    LineReader reader{input, output};
    char line[128]{};
    for (;;) {
        const auto result = reader.read(line);
        if (result == LineResult::TooLong) console.write("line too long\n");
        else if (result == LineResult::Line)
            command(line, process, console,
                sys::capability(info, boot::ServiceControl),
                sys::capability(info, BOOT_POOL),
                sys::capability(info, BOOT_CSPACE), input, output);
        prompt(output, "myos> ");
    }
}
