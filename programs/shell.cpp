#include <string_view>
#include <string>
#include <algorithm>
#include <deque>
#include <vector>
#include <proc.hpp>
#include <string.h>
#include <proto.hpp>
#include <ch.hpp>
#include <libk/parse.hpp>
#include <start.hpp>
#include <util.hpp>
#include <libk/fmt.hpp>

static void prompt(cap_t output, const char* text) noexcept {
    ch_msg message{.operation = static_cast<uint64_t>(con_operation::Prompt)};
    message.size = ::strlen(text);
    if (message.size > sizeof(message.data))
        sys::exit(STATUS_BAD_ARGS);
    ::memcpy(message.data, text, message.size);
    if (const auto rc = ch_send(output, message).status; rc != STATUS_OK)
        sys::exit(rc);
}

enum class LineResult { Line, TooLong, End };

class LineReader final {
    cap_t input_, events_;
    str_writer output_;

  public:
    LineReader(cap_t input, cap_t output, cap_t events) noexcept
        : input_(input), events_(events), output_(output) {}

    template <class Tasks> auto read(char (&line)[128], Tasks& jobs) noexcept -> LineResult {
        // The command reader releases its terminal binding before a foreground
        // application acquires input. Only active readers own readiness state.
        ch_port input{input_, events_};
        size_t used{};
        bool overflow{};
        for (;;) {
            ch_msg message{};
            jobs.poll();
            const auto received = input.try_receive(message);
            if (received.status == STATUS_WOULD_BLOCK) {
                if (const auto rc = input.arm().status; rc != STATUS_OK)
                    sys::exit(rc);
                if (const auto rc = jobs.wait_event().status; rc != STATUS_OK)
                    sys::exit(rc);
                continue;
            }
            if (received.status != STATUS_OK) sys::exit(received.status);
            if (message.operation == static_cast<uint64_t>(str_frame::End))
                return LineResult::End;
            if (message.operation != static_cast<uint64_t>(str_frame::Data))
                sys::exit(STATUS_PEER_FAULT);
            for (size_t i = 0; i < message.size; ++i) {
                const char byte = message.data[i];
                if (byte == '\n') {
                    output_.put('\n');
                    line[used] = '\0';
                    return overflow ? LineResult::TooLong : LineResult::Line;
                }
                if (byte == '\b' || byte == 127) {
                    if (used != 0) {
                        --used;
                        output_.write("\b \b");
                    }
                } else if (byte >= 32 && byte < 127) {
                    if (used + 1 < sizeof(line)) {
                        line[used++] = byte;
                        output_.put(byte);
                    } else
                        overflow = true;
                }
            }
        }
    }
};

static uint64_t latest{};
struct Pipe {
    cap_owner roots[2], ends[2];
    uint64_t producer{}, consumer{};
    auto active() const noexcept -> bool { return bool(roots[0]); }
    auto open(cap_t pool, cap_t cspace) noexcept -> status_t {
        const auto pair = sys::channel_create(pool, 4, CHANNEL_MAX_WORDS, 0, 2);
        if (pair.value)
            roots[0] = cap_owner{pair.value};
        if (pair.value2)
            roots[1] = cap_owner{pair.value2};
        if (pair.status != STATUS_OK)
            return pair.status;
        for (size_t i = 0; i < 2; ++i) {
            const auto minted =
                sys::channel_mint(roots[i].selector(), cspace, 1,
                                  (i == 0 ? RIGHT_SEND : RIGHT_RECEIVE) | RIGHT_DUPLICATE);
            if (minted.status != STATUS_OK)
                return minted.status;
            ends[i] = cap_owner{minted.value};
        }
        return STATUS_OK;
    }
    void close() noexcept {
        if (active())
            if (const auto rc = sys::object_destroy(roots[0].selector()).status; rc != STATUS_OK)
                sys::exit(rc);
        ends[0] = {};
        ends[1] = {};
        roots[0] = {};
        roots[1] = {};
        producer = consumer = 0;
    }
};
static std::deque<Pipe> pipes;
// The parent owns tasks. This table adds only shell stream policy to their IDs.
class Jobs {
    struct Job {
        pr_id task;
        cap_owner output;
        word_t writable{};
        uint64_t sequence{};
        std::optional<ch_msg> end;
        bool consumed{};
        bool discard{};
    };
    rt_env info_;
    pr_tasks tasks_;
    std::deque<Job> jobs_;
    cap_owner files_event_;
    io_client files_;
    rt_clock clock_;

  public:
    void open(const rt_env& info) {
        info_ = info;
        if (const auto rc = clock_.open(); rc != STATUS_OK)
            sys::exit(rc);
        tasks_.open(info.pool, info.selector(rt_domain), info.vspace, info.cpus);
        const auto event = sys::notification_create(info.pool, 1);
        if (event.status != STATUS_OK) sys::exit(event.status);
        files_event_ = cap_owner{event.value};
        uint64_t value{};
        if (const auto rc = files_.connect(info.selector(fs_service), info.pool,
                                           info.cspace, files_event_.selector(),
                                           info.vspace, 0, value);
            rc != STATUS_OK)
            sys::exit(rc);
    }
    auto spawn(std::span<const std::string_view> argv, std::span<const cap_t, 3> stdio, bool end) -> pr_spawn {
        const std::string_view name{argv[0]};
        // Bare commands use the boot mount's package suffix; paths are explicit.
        std::string path{name};
        if (!name.ends_with(".pkg") && !name.ends_with(".PKG"))
            path += ".PKG";
        fs_file file;
        auto s = fs_open(files_, path.data(), path.size(), file);
        if (s != STATUS_OK)
            return {{}, s};
        auto memory = fs_backing(files_, file, VM_READ | VM_EXECUTE);
        const auto closed = fs_close(files_, file);
        if (!memory)
            return {{}, memory.error()};
        if (closed != STATUS_OK)
            return {{}, closed};
        pr_pkg pkg;
        s = pkg.open(info_.vspace, memory->memory.selector(),
                     {0, pr_window::round_size(memory->size)}, memory->size,
                     VM_READ | VM_EXECUTE | VM_MAP_PRIVATE);
        if (s != STATUS_OK)
            return {{}, s};
        const auto binding = [](rt_req role, cap_t cap, word_t rights) {
            return pr_cap{.contract = role, .cap = cap, .limit = rights};
        };
        const pr_cap caps[]{
            binding(rt_stdin, stdio[0], RIGHT_RECEIVE),
            binding(rt_stdout, stdio[1], RIGHT_SEND),
            binding(rt_stderr, stdio[2], RIGHT_SEND),
            binding(vfs_service, info_.selector(vfs_service), RIGHT_SEND),
            binding(vfs_read, info_.selector(vfs_read), RIGHT_SEND),
        };
        cap_owner output;
        word_t writable{};
        if (end) {
            const auto copy = sys::cap_duplicate(stdio[1], info_.cspace, RIGHT_SEND);
            s = copy.status;
            output = cap_owner{copy.value};
            if (s == STATUS_OK) {
                const auto bound = sys::channel_bind(output.selector(), info_.events,
                                                     CHANNEL_WRITABLE);
                s = bound.status;
                writable = bound.value;
            }
            if (s != STATUS_OK)
                return {{}, s};
        }
        const auto child =
            tasks_.spawn(pkg, {},
                         {.argv = argv,
                          .caps = caps,
                          .resources = {.bytes = 4 * 1024 * 1024,
                                        .caps = 128,
                                        .kinds = pr_base_kinds | OBJ_BIT(ObjChannel)},
                          .events = info_.events});
        if (child.id.valid())
            jobs_.push_back(Job{
                child.id, std::move(output), writable, 0, {}, false, child.status != STATUS_OK});
        else if (output)
            if (const auto rc = sys::channel_unbind(output.selector(), writable).status;
                rc != STATUS_OK)
                sys::exit(rc);
        if (child.status == STATUS_OK)
            latest = child.id.key();
        return child;
    }
    void poll() {
        for (auto i = jobs_.begin(); i != jobs_.end();) {
            auto& job = *i;
            if (!job.consumed) {
                const auto s = tasks_.poll(job.task);
                if (s == STATUS_OK) {
                    // A dead consumer has no reader, even while its exit result
                    // remains unconsumed. Release backpressured writers now.
                    for (auto& pipe : pipes)
                        if (pipe.consumer == job.task.key()) {
                            pipe.consumer = 0;
                            if (const auto rc = sys::channel_close(pipe.roots[0].selector()).status;
                                rc != STATUS_OK)
                                sys::exit(rc);
                        }
                    if (job.output && !job.end)
                        job.end = ch_msg{.operation = static_cast<uint64_t>(str_frame::End),
                                         .status = tasks_.result(job.task)->status};
                    if (job.discard) {
                        if (const auto rc = tasks_.collect(job.task).status; rc != STATUS_OK)
                            sys::exit(rc);
                        job.consumed = true;
                    }
                } else if (s != STATUS_WOULD_BLOCK && !pr_retryable(s))
                    if (s != STATUS_OK) sys::exit(s);
            }
            if (job.output && job.end) {
                const auto sent = ch_send(job.output.selector(), *job.end, false);
                if (sent.status == STATUS_WOULD_BLOCK) {
                    const auto armed =
                        sys::channel_arm(job.output.selector(), job.writable, job.sequence);
                    if (armed.status != STATUS_OK) sys::exit(armed.status);
                    job.sequence = armed.value;
                } else {
                    if (sent.status != STATUS_CLOSED && sent.status != STATUS_PEER_CLOSED)
                        if (sent.status != STATUS_OK) sys::exit(sent.status);
                    if (const auto rc =
                            sys::channel_unbind(job.output.selector(), job.writable).status;
                        rc != STATUS_OK)
                        sys::exit(rc);
                    if (const auto rc = job.output.close(); rc != STATUS_OK)
                        sys::exit(rc);
                }
            }
            if (job.consumed && !job.output) {
                i = jobs_.erase(i);
            } else
                ++i;
        }
    }
    auto wait_event(uint64_t deadline = 0) -> sys::SysResult {
        // Synchronous group close can await a kernel worker without emitting an
        // exit notification. Wake briefly only while such retirement is pending.
        auto wake_at = deadline;
        if (std::any_of(jobs_.begin(), jobs_.end(), [&](const auto& job) {
                return !job.consumed && tasks_.closing_needs_poll(job.task);
            })) {
            const auto next = clock_.after_ms(1);
            if (!next)
                return {.status = STATUS_BAD_ARGS};
            if (!wake_at || *next < wake_at)
                wake_at = *next;
        }
        const auto wake = sys::notification_wait(info_.events, wake_at);
        return wake.status == STATUS_TIMED_OUT && wake_at != deadline ? sys::SysResult{} : wake;
    }
    auto wait(uint64_t id, uint64_t deadline = 0) -> sys::SysResult {
        for (;;) {
            poll();
            auto i = std::find_if(jobs_.begin(), jobs_.end(),
                                  [=](const auto& job) { return job.task.key() == id; });
            if (i == jobs_.end() || i->discard || i->consumed)
                return {.status = STATUS_INVALID_CAP};
            if (tasks_.result(i->task)) {
                const auto result = tasks_.collect(i->task);
                i->consumed = true;
                for (auto& pipe : pipes)
                    if (pipe.active()) {
                        if (pipe.producer == id)
                            pipe.producer = 0;
                        if (pipe.consumer == id) {
                            pipe.consumer = 0;
                            if (const auto rc = sys::channel_close(pipe.roots[0].selector()).status;
                                rc != STATUS_OK)
                                sys::exit(rc);
                        }
                    }
                std::erase_if(pipes, [](auto& p) {
                    if (p.producer || p.consumer)
                        return false;
                    p.close();
                    return true;
                });
                if (latest == id) {
                    latest = 0;
                    for (const auto& job : jobs_)
                        if (!job.consumed && !job.discard)
                            latest = job.task.key();
                }
                return result;
            }
            const auto wake = wait_event(deadline);
            if (wake.status != STATUS_OK)
                return wake;
        }
    }
    auto stop(uint64_t id) -> sys::SysResult {
        auto i = std::find_if(jobs_.begin(), jobs_.end(),
                              [=](const auto& job) { return job.task.key() == id; });
        if (i == jobs_.end() || i->consumed || i->discard)
            return {.status = STATUS_INVALID_CAP};
        const auto s = tasks_.request_stop(i->task);
        return s == STATUS_OK ? wait(id) : sys::SysResult{.status = s};
    }
    void list(str_writer& console) const {
        for (const auto& job : jobs_)
            if (!job.discard && !job.consumed)
                (void) libk::fmt::format_to<"task: {}\n">(console, job.task.key());
    }
};
static Jobs jobs;

static bool storage_available{};
static cap_t info_admin{};

static void error(str_writer& console, status_t status) {
    (void) libk::fmt::format_to<"error: {}\n">(console, status);
}

// Parse one bounded command line into the existing argv wire form. A pipe is
// syntax only outside quotes; escaped bytes and empty quoted words are data.
static auto arguments(char* text, std::vector<std::string>& output, std::vector<size_t>& cuts,
                      char (&target)[128], bool& append_output) noexcept -> bool {
    char word[128]{};
    size_t used{};
    bool present{};
    bool redirect{}, target_present{};
    char quote{};
    auto append = [&]() noexcept -> bool {
        if (!present)
            return true;
        bool ok{};
        if (redirect) {
            ok = !target_present && used != 0;
            if (ok) {
                ::memcpy(target, word, used);
                target[used] = 0;
                target_present = true;
            }
        } else {
            output.emplace_back(word, used);
            ok = true;
        }
        used = 0;
        present = false;
        return ok;
    };
    for (;;) {
        const char ch = *text++;
        if (ch == '\0')
            return quote == 0 && append() && (!redirect || target_present);
        if (ch == '\\' && *text != '\0') {
            if (used == sizeof(word))
                return false;
            word[used++] = *text++;
            present = true;
        } else if (ch == '\'' || ch == '"') {
            if (quote == 0) {
                quote = ch;
                present = true;
            } else if (quote == ch)
                quote = 0;
            else {
                if (used == sizeof(word))
                    return false;
                word[used++] = ch;
            }
        } else if (quote == 0 && (ch == ' ' || ch == '|' || ch == '>')) {
            if (!append())
                return false;
            if (ch == '|') {
                if (redirect || output.size() == cuts.back())
                    return false;
                cuts.push_back(output.size());
            } else if (ch == '>') {
                if (redirect || output.size() == cuts.back())
                    return false;
                redirect = true;
                if (*text == '>') {
                    append_output = true;
                    ++text;
                }
            }
        } else {
            if (used == sizeof(word))
                return false;
            word[used++] = ch;
            present = true;
        }
    }
}

static void command(char* line, str_writer& console, cap_t control, cap_t pool, cap_t cspace,
                    cap_t input, cap_t output, cap_t events) {
    while (*line == ' ')
        ++line;
    char name[128]{};
    char* argument = line;
    while (*argument != '\0' && *argument != ' ' && *argument != '|' && *argument != '>')
        ++argument;
    const size_t name_size = argument - line;
    ::memcpy(name, line, name_size);
    line = name;
    while (*argument == ' ')
        ++argument;
    if (*line == '\0')
        return;
    if ((std::string_view{line} == "help")) {
        if (*argument != '\0') {
            console.write("invalid command line\n");
            return;
        }
        console.write("help | ls [DIR] | cat FILE | mkfs [32_HEX_ID] | edit FILE | run/spawn "
                      "PROGRAM [ARGS] | jobs | wait [ID] [MS] | stop [ID] | svc [restart SERVICE]\n");
        if (storage_available)
            console.write("PROGRAM > FILE (replace) | PROGRAM >> FILE (append)\n");
        if (storage_available)
            console.write(
                "touch FILE | write/append FILE TEXT | mkdir/rm PATH | mv OLD NEW | cp SOURCE DEST "
                "| stat FILE | fs device/volid; / is data, /boot is read-only\n");
        return;
    }
    if (std::string_view{line} == "svc") {
        ch_rpc rpc;
        const auto opened = rpc.open(control, pool, cspace, events);
        if (opened != STATUS_OK) { error(console, opened); return; }
        if (*argument == '\0') {
            for (uint64_t index = 0;; ++index) {
                ch_msg request{.operation = static_cast<uint64_t>(sv_op::List), .value = index};
                const auto status = rpc.exchange(request);
                if (status == STATUS_NOT_FOUND) break;
                if (status != STATUS_OK) { error(console, status); break; }
                console.write(request.data, request.size);
                console.write(request.value & sv_recovering ? " recovering\n" : " running\n");
            }
        } else {
            const std::string_view text{argument};
            if (!text.starts_with("restart ") || text.size() == 8 || text.size() - 8 > sizeof(ch_msg::data)) {
                console.write("usage: svc [restart SERVICE]\n");
                return;
            }
            ch_msg request{.operation = static_cast<uint64_t>(sv_op::Restart), .size = text.size() - 8};
            ::memcpy(request.data, text.data() + 8, request.size);
            console.write("service restart requested\n");
            const auto status = rpc.exchange(request);
            if (status != STATUS_OK) error(console, status);
        }
        return;
    }
    if (std::string_view{line} == "mkfs") {
        uint8_t id[st_volume_id_size]{};
        const std::string_view text{argument};
        if (!text.empty()) {
            if (text.size() != 2 * sizeof(id)) {
                error(console, STATUS_BAD_ARGS);
                return;
            }
            for (size_t i = 0; i < sizeof(id); ++i) {
                const auto byte = libk::parse<uint8_t>(text.substr(2 * i, 2), 16);
                if (!byte) {
                    error(console, STATUS_BAD_ARGS);
                    return;
                }
                id[i] = *byte;
            }
        }
        ch_rpc admin;
        auto status = admin.open(info_admin, pool, cspace, events);
        if (status == STATUS_OK)
            status = st_format(admin, text.empty() ? nullptr : id);
        if (status != STATUS_OK)
            error(console, status);
        return;
    }
    if ((std::string_view{line} == "jobs")) {
        if (*argument != '\0') {
            console.write("invalid command line\n");
            return;
        }
        jobs.list(console);
        return;
    }
    const bool file_command =
        storage_available &&
        ((std::string_view{line} == "stat") || (std::string_view{line} == "touch") ||
         (std::string_view{line} == "write") || (std::string_view{line} == "append") ||
         (std::string_view{line} == "mkdir") || (std::string_view{line} == "rm") ||
         (std::string_view{line} == "mv") || (std::string_view{line} == "cp"));
    const bool explicit_run = (std::string_view{line} == "run");
    const bool spawn = (std::string_view{line} == "spawn");
    const bool wait = (std::string_view{line} == "wait");
    const bool stop = (std::string_view{line} == "stop");
    const bool run = !spawn && !wait && !stop;
    ch_msg request{};
    request.id = latest;
    if ((wait || stop) && *argument != 0) {
        char* end = argument;
        while (*end && *end != ' ')
            ++end;
        if (*end)
            *end++ = 0;
        const auto parsed = libk::parse<uint64_t>(argument);
        if (!parsed) {
            console.write("invalid task\n");
            return;
        }
        request.id = *parsed;
        while (*end == ' ')
            ++end;
        if (wait && *end) {
            const auto duration = libk::parse<uint64_t>(end);
            rt_clock clock;
            if (const auto rc = clock.open(); rc != STATUS_OK)
                sys::exit(rc);
            const auto deadline = duration ? clock.after_ms(*duration) : std::nullopt;
            if (!deadline) {
                console.write("invalid timeout\n");
                return;
            }
            request.size = sizeof(uint64_t);
            ::memcpy(request.data, &*deadline, request.size);
        }
    }
    if (run || spawn) {
        std::vector<std::string> words;
        if (file_command) {
            words.emplace_back("fs");
            if ((std::string_view{line} == "cp"))
                words.emplace_back("copy");
            else
                words.emplace_back(line);
        } else if (!explicit_run && !spawn)
            words.emplace_back(line);
        std::vector<size_t> cuts{0};
        char target[128]{};
        bool append_output{};
        if (!arguments(argument, words, cuts, target, append_output) ||
            words.size() == cuts.back()) {
            console.write("invalid command line\n");
            return;
        }
        if (*target) {
            cuts.push_back(words.size());
            words.emplace_back("put");
            if (append_output) words.emplace_back("-a");
            words.emplace_back(target);
        }
        cuts.push_back(words.size());
        const auto stages = cuts.size() - 1;
        std::vector<Pipe*> links(stages);
        const size_t first = run ? 1 : 0;
        status_t status = STATUS_OK;
        for (size_t i = first; i < stages; ++i) {
            links[i] = &pipes.emplace_back();
            status = links[i]->open(pool, cspace);
            if (status != STATUS_OK)
                break;
        }
        // A background job starts at EOF rather than consuming shell input.
        if (status == STATUS_OK && !run)
            status = ch_send(links[0]->ends[0].selector(),
                             {.operation = static_cast<uint64_t>(str_frame::End)})
                         .status;
        std::vector<uint64_t> ids(stages);
        for (size_t i = 0; i < stages && status == STATUS_OK; ++i) {
            std::vector<std::string_view> launch;
            for (size_t j = cuts[i]; j < cuts[i + 1]; ++j) launch.push_back(words[j]);
            const std::array<cap_t, 3> stdio{
                i == 0 && run ? input : links[i]->ends[1].selector(),
                i + 1 == stages ? output : links[i + 1]->ends[0].selector(), output};
            const auto child = jobs.spawn(launch, stdio, i + 1 < stages);
            status = child.status;
            if (status != STATUS_OK)
                break;
            ids[i] = child.id.key();
            if (links[i])
                links[i]->consumer = ids[i];
            if (i + 1 < stages)
                links[i + 1]->producer = ids[i];
        }
        if (status != STATUS_OK) {
            // Revoke edges first so a partial pipeline cannot remain blocked.
            for (auto* p : links)
                if (p && p->active())
                    if (const auto rc = sys::channel_close(p->roots[0].selector()).status;
                        rc != STATUS_OK)
                        sys::exit(rc);
            for (auto id : ids)
                if (id)
                    if (const auto rc = jobs.stop(id).status; rc != STATUS_OK)
                        sys::exit(rc);
            std::erase_if(pipes, [](auto& p) {
                if (p.producer || p.consumer)
                    return false;
                p.close();
                return true;
            });
            error(console, status);
            return;
        }
        if (!run) {
            for (auto id : ids)
                if (id)
                    (void) libk::fmt::format_to<"task: {}\n">(console, id);
            return;
        }
        // Reap consumers first: an early consumer exit closes the edge and
        // wakes a backpressured producer. The parent sends End after the producer has retired.
        for (size_t i = stages; i-- > 0;) {
            const auto reply = jobs.wait(ids[i]);
            if (reply.status != STATUS_OK) sys::exit(reply.status);
            if (static_cast<status_t>(reply.value) != STATUS_OK)
                error(console, reply.value);
        }
        return;
    }
    uint64_t deadline{};
    if (request.size)
        ::memcpy(&deadline, request.data, sizeof(deadline));
    const auto reply = stop ? jobs.stop(request.id) : jobs.wait(request.id, deadline);
    const auto status =
        reply.status == STATUS_OK ? static_cast<status_t>(reply.value) : reply.status;
    if (status != STATUS_OK && !(stop && status == STATUS_CANCELED))
        error(console, status);
}

int main(int argc, char** argv) {

    const auto& info = rt_startup();
    const auto output = info.selector(con_output);
    str_writer console{output};
    const auto input = info.selector(con_input);
    jobs.open(info);
    info_admin = info.selector(st_admin);
    storage_available = argc == 2 && (std::string_view{argv[1]} == "storage");
    console.write("myos native shell\n");
    prompt(output, "myos> ");
    LineReader reader{input, output, info.events};
    char line[128]{};
    for (;;) {
        const auto result = reader.read(line, jobs);
        if (result == LineResult::TooLong)
            console.write("line too long\n");
        else if (result == LineResult::Line)
            command(line, console, info.selector(sv_service), info.pool,
                    info.cspace, input, output, info.events);
        prompt(output, "myos> ");
    }
}
