#include <vm.hpp>
#include <proc.hpp>
#include <lib/fs/fs.hpp>

#include <lib/mgr/mgr.hpp>
#include <lib/term/term.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/parse.hpp>
#include <libk/fmt.hpp>
#include <bit>
#include <deque>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

struct sh_stage {
    std::unique_ptr<proc_task> task;
    std::optional<status_t> result;
    bool end{};
    cap_owner end_sender;
    ev_watch end_ready;
};
enum class sh_state { Running, Suspending, Suspended, Terminating, Done };
struct sh_job {
    uint64_t id;
    std::string command;
    std::vector<sh_stage> stages;
    std::vector<std::unique_ptr<ch_pair>> pipes;
    ch_pair input, output;
    ubus_object terminal;
    cap_owner input_sender, terminal_sender, terminal_connector;
    ev_watch terminal_ready;
    std::optional<ubus_msg> focus_event;
    ev_watch input_ready;
    ubus_rx output_rx;
    std::deque<str_chunk> pending_input;
    sh_state state{sh_state::Running};
    bool fullscreen{}, output_empty{}, background{}, announced{}, read_only{};
    uint64_t focus{};
};

class sh_session {
    const rt_env& env_;
    ubus_rx input_;
    ubus_conn bus_, publisher_;
    ubus_object management_{"myos.manager"};
    mgr_wait watches_;
    std::deque<std::unique_ptr<sh_job>> jobs_;
    uint64_t next_{}, foreground_{}, focus_{};
    struct output { str_chunk frame; uint64_t owner{}, focus{}; };
    std::deque<output> output_;
    ev_watch output_ready_;
    std::string line_;
    std::string cwd_{"/"};
    std::vector<status_t> result_;
    std::optional<std::string> command_;
    bool carriage_{}, overflow_{}, screen_{}, reading_{}, prompt_visible_{}, line_start_{true};
    auto job(uint64_t id) noexcept -> sh_job* {
        for (auto& job : jobs_) if (job->id == id) return job.get();
        return nullptr;
    }
    auto collect(sh_job& job) -> status_t {
        result_.clear();
        for (const auto& stage : job.stages) result_.push_back(stage.result.value_or(STATUS_CANCELED));
        const auto status = result_.back();
        const auto id = job.id;
        std::erase_if(jobs_, [=](const auto& job) { return job->id == id; });
        return status;
    }
    void enqueue(std::string_view text, uint64_t owner = 0, uint64_t focus = 0) {
        while (!text.empty()) {
            if (output_.empty() || output_.back().owner != owner || output_.back().focus != focus
                || output_.back().frame.size == sizeof(str_chunk::data)) {
                if (output_.size() == 128) return; // Echo/status overflow; child streams stop draining at 64.
                output_.push_back({{}, owner, focus});
            }
            auto& frame = output_.back().frame;
            const auto count = std::min(text.size(), sizeof(frame.data) - frame.size);
            ::memcpy(frame.data + frame.size, text.data(), count);
            frame.size += count; text.remove_prefix(count);
        }
    }
    void focus(sh_job& job, bool gained) {
        // Supersede pending screen/control text; ordinary job bytes keep order.
        std::erase_if(output_, [](const auto& output) { return output.focus || !output.owner; });
        job.focus = ++focus_;
        // Old queued events can be replaced; old in-flight frames still carry
        // their old focus sequence and are rejected by the output broker.
        ubus_msg event;
        event.data.put("focus", job.focus).put("focused", gained).put("rows", uint64_t{tm_default_size.rows})
            .put("columns", uint64_t{tm_default_size.columns});
        job.focus_event.emplace(std::move(event));
        if (gained) { enqueue("\x1b[?1049h\x1b[?25h"); screen_ = true; }
        else if (screen_) { enqueue("\x1b[?1049l\x1b[?25h"); screen_ = false; }
    }
    void leave(sh_job& job) {
        if (foreground_ != job.id) return;
        if (job.fullscreen) focus(job, false);
        foreground_ = 0;
    }
    void input(char byte) {
        if (byte == '\n' && carriage_) { carriage_ = false; return; }
        carriage_ = byte == '\r';
        if (carriage_) byte = '\n';
        auto* target = job(foreground_);
        if (byte == 3 || byte == 26) {
            if (target) {
                leave(*target);
                if (byte == 3) terminate(*target);
                else {
                    target->state = sh_state::Suspending;
                    target->background = true;
                    for (auto& stage : target->stages)
                        if (stage.task) (void)stage.task->execution(false);
                }
            } else { line_.clear(); overflow_ = false; enqueue("^C\n"); line_start_ = true; prompt_visible_ = false; prompt(); }
            return;
        }
        if (target) {
            if (target->pending_input.size() == 32) { enqueue("\a"); return; }
            if (byte == 4 && !target->fullscreen)
                target->pending_input.push_back({});
            else {
                str_chunk frame{.size = 1}; frame.data[0] = byte;
                target->pending_input.push_back(frame);
            }
            return;
        }
        if (command_) return;
        if (byte == '\n') {
            enqueue("\n"); line_start_ = true; prompt_visible_ = false;
            if (overflow_) { enqueue("line too long\n"); line_.clear(); overflow_ = false; prompt(); }
            else { command_ = std::move(line_); line_.clear(); }
        } else if (byte == '\b' || byte == 127) {
            if (!line_.empty()) { line_.pop_back(); enqueue("\b \b"); }
        } else if (byte >= 32 && byte < 127) {
            if (line_.size() == 127) overflow_ = true;
            else { line_.push_back(byte); enqueue({&byte, 1}); }
        }
    }
    auto launch(sh_stage& stage, sh_job& job, size_t index, std::span<const std::string_view> argv) -> status_t {
        std::string path{argv[0]};
        if (path.find('/') == path.npos) {
            path = "/boot/" + path;
            if (!path.ends_with(".elf")) path += ".elf";
        } else path = fs_path(path, cwd_);
        ubus_conn files;
        auto status = files.connect(bus_, "exec", "myos.files", 1, 1);
        if (status != STATUS_OK) return status;
        auto image = fs_map(files, path, VM_READ | VM_EXECUTE);
        if (!image) return image.error();
        auto mapped = vm_area::map(rt_vm(), std::move(image->memory), 0,
            libk::checked_align_up(image->size, 4096).value_or(0), VM_READ);
        if (!mapped) return mapped.error();
        const auto directory = env_.selector(job.read_only ? mgr_read_bus : ubus_directory);
        auto jobs = management_.connector(job.read_only ? word_t{mgr_observe} : word_t{mgr_observe | mgr_control});
        if (!jobs) return jobs.error();
        std::vector<proc_cap> caps{
            {.key = rt_stdin, .cap = index ? job.pipes[index - 1]->ends[1].selector() : job.input.ends[1].selector(), .limit = word_t{RIGHT_RECEIVE}},
            {.key = rt_stdout, .cap = index + 1 < job.stages.size() ? job.pipes[index]->ends[0].selector() : job.output.ends[0].selector(), .limit = word_t{RIGHT_SEND}, .badge = 1},
            {.key = rt_stderr, .cap = job.output.ends[0].selector(), .limit = word_t{RIGHT_SEND}, .badge = 1},
            {.key = ubus_directory, .cap = directory, .limit = word_t{RIGHT_SEND | RIGHT_DUPLICATE}},
            {.key = mgr_jobs, .cap = jobs->selector(), .limit = word_t{RIGHT_SEND | RIGHT_DUPLICATE}}
        };
        if (job.stages.size() == 1)
            caps.push_back({.key = tm_service, .cap = job.terminal_connector.selector(), .limit = word_t{RIGHT_SEND | RIGHT_DUPLICATE}});
        const auto cpu = static_cast<uint32_t>((job.id + index) % env_.cpus);
        const std::string pwd = "PWD=" + cwd_;
        const std::array<std::string_view, 1> environment{pwd};
        stage.task = std::make_unique<proc_task>();
        const proc_auth auth{env_.alloc(), static_cast<uint32_t>(env_.cpus), env_.domain};
        const proc_quota quota{.bytes=4 * 1024 * 1024, .caps=160,
            .kinds=proc_base_kinds | OBJ_BIT(ObjChannel), .slots=160, .pages=24,
            .pool_perms=RIGHT_CREATE | RIGHT_INSPECT};
        status = stage.task->open(auth, quota);
        if (status != STATUS_OK) return status;
        auto publication = mgr_view(publisher_, "job" + std::to_string(job.id) + "-" + std::to_string(index),
            ubus_all, stage.task->scope(), 4);
        if (!publication) return publication.error();
        caps.push_back({.key=ubus_publisher, .cap=publication->selector(), .limit=word_t{RIGHT_SEND | RIGHT_DUPLICATE}});
        status = proc_spawn(*stage.task, auth,
            {.bytes={reinterpret_cast<const std::byte*>(mapped->address), image->size},
             .memory=mapped->memory.selector(), .offset=image->offset, .perms=VM_READ | VM_EXECUTE},
            {.argv=argv, .envp=environment, .caps=caps, .resources=quota, .cpu=cpu, .events=env_.events});
        if (status == STATUS_OK) status = stage.task->start();
        return status;
    }
public:
    explicit sh_session(const rt_env& env) : env_(env) {
        if (const auto status = input_.open(env.selector(rt_stdin), env.events); status != STATUS_OK) sys::exit(status);
        if (const auto status = bus_.bind(env.selector(ubus_directory)); status != STATUS_OK) sys::exit(status);
        if (const auto status = publisher_.bind(env.selector(ubus_publisher)); status != STATUS_OK) sys::exit(status);
        for (const auto name : {"List", "Inspect", "Changed", "Action", "CloseCap", "Revoke"})
            if (const auto status = mgr_method(management_, name, [this, name](auto& q, auto& r) { return manage(name, q, r); });
                status != STATUS_OK) sys::exit(status);
        if (const auto status = management_.open(); status != STATUS_OK) sys::exit(status);
    }
    void prompt() {
        if (!line_start_ && !prompt_visible_) enqueue("\n");
        enqueue(cwd_); enqueue(" > "); enqueue(line_); prompt_visible_ = true;
    }
    auto cwd() const noexcept -> const std::string& { return cwd_; }
    auto cd(std::string_view path) -> status_t {
        if (path.empty()) return STATUS_BAD_ARGS;
        ubus_conn files;
        auto status = files.connect(bus_, "read", "myos.files", 1, 1);
        if (status != STATUS_OK) return status;
        const auto target = fs_path(path, cwd_);
        ubus_data page;
        status = fs_list(files, target, page);
        if (status == STATUS_OK) cwd_ = target;
        return status;
    }
    void terminate(sh_job& job) {
        job.state = sh_state::Terminating;
        leave(job);
        for (auto& stage : job.stages) if (stage.task) (void)stage.task->stop();
    }
    auto manage(std::string_view method, ubus_request& request, ubus_msg& response) -> status_t {
        if (method == "List") {
            ubus_data items{ubus_type::Array};
            constexpr std::array<std::string_view, 5> states{"running", "suspending", "suspended", "draining", "done"};
            for (const auto& job : jobs_) for (size_t i = 0; i < job->stages.size(); ++i) {
                auto row = mgr_row(job->stages[i].task.get(), (job->id << 8) | i, job->id, job->command,
                    states[static_cast<size_t>(job->state)], (request.facet() & mgr_control) && job->id != foreground_);
                row.put("job", job->id).put("member", i + 1); items.push(row);
            }
            response.data.put("items", items).put("epoch", next_); return STATUS_OK;
        }
        const auto id = *request.data["id"].as<uint64_t>();
        auto* target = job(id >> 8);
        const auto index = id & 255;
        if (!target || index >= target->stages.size()) return STATUS_NOT_FOUND;
        if (request.data["gen"].as<uint64_t>() != target->id) return STATUS_STALE;
        auto& stage = target->stages[index];
        if (!stage.task) return STATUS_CLOSED;
        if (method == "Changed") return watches_.accept(stage.task->scope(), stage.task->cspace(), request, response);
        if (method == "Inspect") {
            if (request.data["facet"].as<std::string_view>() == "dependencies") {
                ubus_data items{ubus_type::Array};
                for (const auto peer : {index ? index - 1 : UINT64_MAX, index + 1 < target->stages.size() ? index + 1 : UINT64_MAX}) if (peer != UINT64_MAX) {
                    ubus_data edge; edge.put("id", (target->id << 8) | peer).put("gen", target->id)
                        .put("name", peer < index ? "stdin pipe" : "stdout pipe"); items.push(edge);
                }
                response.data.put("items", items); return STATUS_OK;
            }
            return mgr_inspect(stage.task->scope(), stage.task->cspace(), request.data.view(), response);
        }
        if (target->id == foreground_) return STATUS_DENIED;
        if (method == "CloseCap") return sys::cap_close(*request.data["selector"].as<cap_t>(), stage.task->cspace()).status;
        if (method == "Revoke") return mgr_revoke(stage.task->cspace(), request.data.view(), response);
        if (method != "Action") return STATUS_INVALID_OP;
        const auto action = *request.data["action"].as<std::string_view>();
        if (action == "stop") { terminate(*target); return STATUS_OK; }
        if (action != "pause" && action != "resume") return STATUS_BAD_ARGS;
        const bool enabled = action == "resume";
        if (!enabled) leave(*target);
        target->state = enabled ? sh_state::Running : sh_state::Suspending;
        for (auto& member : target->stages) if (member.task)
            if (const auto status = member.task->execution(enabled).status; status != STATUS_OK) return status;
        return STATUS_OK;
    }
    void pump(bool sleep = true) {
        bool polling{}, progress{};
        for (unsigned n = 0; n < 16; ++n) {
            const auto status = management_.poll();
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) sys::exit(status);
            progress = true;
        }
        watches_.poll();
        auto deadline = watches_.deadline();
        if (const auto status = management_.arm(); status != STATUS_OK) sys::exit(status);
        for (auto& entry : jobs_) {
            auto& job = *entry;
            bool suspended = true, done = true;
            for (size_t i = 0; i < job.stages.size(); ++i) {
                auto& stage = job.stages[i];
                if (stage.task) {
                    if (job.state == sh_state::Suspending) {
                        const auto state = stage.task->info();
                        suspended &= state && state->requested == state->applied && !(state->flags & THREAD_APPLIED);
                    }
                    const auto status = stage.task->poll();
                    if (status == STATUS_OK) {
                        progress = true;
                        const auto result = stage.task->collect();
                        if (!stage.result) stage.result = static_cast<status_t>(result.value);
                        stage.task.reset(); stage.end = i + 1 < job.stages.size();
                        if (i) (void)sys::channel_close(job.pipes[i - 1]->ends[0].selector());
                    } else if (status != STATUS_WOULD_BLOCK && !proc_retryable(status)) sys::exit(status);
                    else polling |= stage.task->polling();
                }
                if (stage.end) {
                    const auto status = str_send(stage.end_sender.selector(), {.status = *stage.result}, false);
                    if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
                        const auto armed = stage.end_ready ? stage.end_ready.arm().status
                            : stage.end_ready.open(env_.events, stage.end_sender.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
                        if (armed != STATUS_OK) sys::exit(armed);
                    } else {
                        stage.end = false; progress = true;
                        if (const auto closed = stage.end_ready.close(); closed != STATUS_OK) sys::exit(closed);
                    }
                }
                done &= !stage.task && !stage.end;
            }
            if (job.state == sh_state::Suspending && suspended) { job.state = sh_state::Suspended; progress = true; }
            for (unsigned i = 0; i < 16; ++i) {
                if (foreground_ == job.id && output_.size() >= 64) break;
                const auto status = job.terminal.poll();
                if (status == STATUS_WOULD_BLOCK) break;
                if (status != STATUS_OK) break;
                progress = true;
            }
            if (job.focus_event && job.terminal_sender) {
                const auto status = ubus_send(job.terminal_sender.selector(), *job.focus_event, false);
                if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
                    const auto armed = job.terminal_ready ? job.terminal_ready.arm().status
                        : job.terminal_ready.open(env_.events, job.terminal_sender.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
                    if (armed != STATUS_OK) sys::exit(armed);
                } else {
                    job.focus_event.reset();
                    if (const auto closed = job.terminal_ready.close(); closed != STATUS_OK) sys::exit(closed);
                    if (status != STATUS_OK) job.terminal_sender = {};
                    progress = true;
                }
            }
            if (done && job.fullscreen && foreground_ == job.id) leave(job);
            if (!screen_ && output_.size() < 64) {
                str_chunk frame;
                const auto status = str_receive(job.output_rx, frame);
                job.output_empty = status == STATUS_WOULD_BLOCK || status == STATUS_CLOSED || status == STATUS_PEER_CLOSED;
                progress |= status == STATUS_OK;
                if (status == STATUS_OK && frame.size) {
                    const bool background = !foreground_;
                    if (background && prompt_visible_) { enqueue("\r\x1b[2K"); prompt_visible_ = false; }
                    output_.push_back({frame, job.id, 0});
                    line_start_ = frame.size && (frame.data[frame.size - 1] == '\n' || frame.data[frame.size - 1] == '\r');
                    if (background && line_start_ && reading_) prompt();
                }
            }
            while (!job.pending_input.empty()) {
                const auto status = str_send(job.input_sender.selector(), job.pending_input.front(), false);
                if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
                    const auto armed = job.input_ready ? job.input_ready.arm().status
                        : job.input_ready.open(env_.events, job.input_sender.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
                    if (armed != STATUS_OK) sys::exit(armed);
                    break;
                }
                job.pending_input.pop_front(); progress = true;
            }
            if (job.pending_input.empty())
                if (const auto status = job.input_ready.close(); status != STATUS_OK) sys::exit(status);
            if (done && job.output_empty && job.state != sh_state::Done) {
                leave(job); job.state = sh_state::Done; progress = true;
            }
            if (job.state == sh_state::Done && job.background && !job.announced && reading_ && !foreground_) {
                libk::fmt::fixed_buffer<96> notice;
                (void)libk::fmt::format_to<"\r\x1b[2K[{}] Done {}\n">(notice, job.id, job.stages.back().result.value_or(STATUS_CANCELED));
                enqueue(notice.view()); line_start_ = true; prompt_visible_ = false; prompt(); job.announced = true;
            }
        }
        for (unsigned i = 0; i < 16; ++i) {
            str_chunk frame;
            const auto status = str_receive(input_, frame);
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) sys::exit(status);
            progress = true;
            for (size_t i = 0; i < frame.size; ++i) input(frame.data[i]);
        }
        while (!output_.empty()) {
            auto& next = output_.front();
            if (next.focus && (next.owner != foreground_ || !job(next.owner) || next.focus != job(next.owner)->focus)) { output_.pop_front(); continue; }
            const auto status = str_send(env_.selector(rt_stdout), next.frame, false);
            if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
                const auto armed = output_ready_ ? output_ready_.arm().status
                    : output_ready_.open(env_.events, env_.selector(rt_stdout), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
                if (armed != STATUS_OK) sys::exit(armed);
                break;
            }
            if (status != STATUS_OK) sys::exit(status);
            output_.pop_front(); progress = true;
        }
        if (output_.empty())
            if (const auto status = output_ready_.close(); status != STATUS_OK) sys::exit(status);
        (void)input_.arm();
        for (auto& entry : jobs_) { (void)entry->terminal.arm(); (void)entry->output_rx.arm(); }
        if (sleep && !command_ && !progress) {
            // Successful executions use Exit/Scope/Account notifications.
            if (polling) {
                const auto next = rt_after_ms(1).value_or(0);
                if (next && (!deadline || next < deadline)) deadline = next;
            }
            (void)ev_wait(env_.events, deadline);
        }
    }
    auto read() -> std::string {
        reading_ = true;
        while (!command_) pump();
        reading_ = false;
        auto command = std::move(*command_); command_.reset();
        return command;
    }
    auto spawn(const std::vector<std::string>& words, const std::vector<size_t>& cuts,
               bool background, std::string_view command, bool read_only = false) -> status_t {
        auto entry = std::make_unique<sh_job>();
        auto& job = *entry;
        job.id = ++next_; job.command = command; job.background = background; job.read_only = read_only;
        job.stages.resize(cuts.size() - 1);
        auto status = job.input.open(env_.alloc(), 8, 0);
        if (status == STATUS_OK) status = job.output.open(env_.alloc(), 8, 0);
        if (status == STATUS_OK) status = job.terminal.open(8);
        if (status == STATUS_OK) {
            const auto input = sys::channel_mint(job.input.ends[0].selector(), env_.cspace, 1, RIGHT_SEND);
            job.input_sender = cap_owner{input.value}; status = input.status;
        }
        if (status == STATUS_OK) status = job.terminal.add("Fullscreen", {{"events", ubus_type::Cap}}, {},
            [this, &job](auto& request, auto&) {
                auto endpoint = request.take(request.data["events"]);
                const auto state = sys::source_info(endpoint.selector(), SOURCE_WRITE);
                if (state.status != STATUS_OK || (state.value & SOURCE_TERMINAL)) return STATUS_BAD_ARGS;
                if (const auto status = job.terminal_ready.close(); status != STATUS_OK) return status;
                job.terminal_sender = std::move(endpoint); job.fullscreen = true;
                if (foreground_ == job.id) focus(job, true);
                return STATUS_OK;
            });
        if (status == STATUS_OK) status = job.terminal.add("Frame", {{"focus", ubus_type::U64}, {"data", ubus_type::Bytes, false, sizeof(str_chunk::data)}}, {},
            [this, &job](auto& request, auto&) {
                if (foreground_ != job.id || request.data["focus"].template as<uint64_t>() != job.focus) return STATUS_STALE;
                if (output_.size() >= 64) return STATUS_BUSY;
                str_chunk frame;
                const auto bytes = *request.data["data"].template as<std::span<const std::byte>>();
                frame.size = bytes.size(); ::memcpy(frame.data, bytes.data(), frame.size);
                output_.push_back({frame, job.id, job.focus}); return STATUS_OK;
            }, 0, true);
        if (status == STATUS_OK) {
            auto connector = job.terminal.connector();
            if (!connector) status = connector.error(); else job.terminal_connector = std::move(*connector);
        }
        if (status == STATUS_OK) status = job.output_rx.open(job.output.ends[1].selector(), env_.events);
        for (size_t i = 1; i < job.stages.size() && status == STATUS_OK; ++i) {
            auto& pipe = job.pipes.emplace_back(std::make_unique<ch_pair>());
            status = pipe->open(env_.alloc(), 4, 0);
            if (status == STATUS_OK) {
                const auto end = sys::channel_mint(pipe->ends[0].selector(), env_.cspace, 1, RIGHT_SEND);
                job.stages[i - 1].end_sender = cap_owner{end.value}; status = end.status;
            }
        }
        if (status != STATUS_OK) return status;
        jobs_.push_back(std::move(entry));
        if (!background) foreground_ = job.id;
        for (size_t i = 0; i < job.stages.size() && status == STATUS_OK; ++i) {
            std::vector<std::string_view> argv(words.begin() + cuts[i], words.begin() + cuts[i + 1]);
            status = launch(job.stages[i], job, i, argv);
            if (status != STATUS_OK) job.stages[i].result = status;
        }
        if (status != STATUS_OK) {
            terminate(job);
            while (job.state != sh_state::Done) pump();
            (void)collect(job);
            return status;
        }
        if (background) {
            libk::fmt::fixed_buffer<192> started;
            (void)libk::fmt::format_to<"[{}] Running {}\n">(started, job.id, std::string_view{job.command});
            enqueue(started.view());
        }
        if (!background) {
            while (foreground_) pump();
            if (job.state == sh_state::Terminating || std::ranges::all_of(job.stages, [](const auto& stage) { return !stage.task; }))
                while (job.state != sh_state::Done) pump();
            if (job.state == sh_state::Done) return collect(job);
        }
        return STATUS_OK;
    }
    auto latest(bool active) const noexcept -> uint64_t {
        for (const auto& job : jobs_ | std::views::reverse)
            if (!active || (job->state != sh_state::Done && job->state != sh_state::Terminating)) return job->id;
        return 0;
    }
    auto control(std::string_view operation, uint64_t id, uint64_t deadline = 0) -> status_t {
        auto* target = job(id);
        if (!target) return STATUS_NOT_FOUND;
        if (operation == "kill") terminate(*target);
        else if (operation == "fg" || operation == "bg") {
            if (target->state == sh_state::Done || target->state == sh_state::Terminating) return STATUS_CLOSED;
            if (operation == "fg") { foreground_ = id; if (target->fullscreen) focus(*target, true); }
            for (auto& stage : target->stages) if (stage.task) (void)stage.task->execution(true);
            target->state = sh_state::Running;
            if (operation == "bg") { target->background = true; return STATUS_OK; }
        }
        if (operation == "fg") {
            while (foreground_) pump();
            if (target->state == sh_state::Terminating || std::ranges::all_of(target->stages, [](const auto& stage) { return !stage.task; }))
                while (target->state != sh_state::Done) pump();
            return target->state == sh_state::Done ? collect(*target) : STATUS_OK;
        }
        while (target->state != sh_state::Done) {
            if (deadline && sys::clock_now().value >= deadline) return STATUS_TIMED_OUT;
            pump();
        }
        const auto result = collect(*target);
        return operation == "kill" ? STATUS_OK : result;
    }
    void status(str_writer& console) const {
        for (size_t i = 0; i < result_.size(); ++i)
            (void)libk::fmt::format_to<"stage {}: {}\n">(console, i + 1, result_[i]);
    }
    void list(str_writer& console) const {
        constexpr const char* names[]{"Running", "Suspending", "Suspended", "Terminating", "Done"};
        for (const auto& job : jobs_)
            (void)libk::fmt::format_to<"[{}] {} {}\n">(console, job->id, names[static_cast<size_t>(job->state)], std::string_view{job->command});
    }
};

static void error(str_writer& console, status_t status) {
    (void)libk::fmt::format_to<"error: {}\n">(console, status);
}
// Parse one bounded command line into the existing argv wire form. A pipe is
// syntax only outside quotes; escaped bytes and empty quoted words are data.
static auto arguments(char* text, std::vector<std::string>& output, std::vector<size_t>& cuts,
                      char (&target)[128], bool& append_output, bool& background) noexcept -> bool {
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
        if (ch == '&' && quote == 0) {
            while (*text == ' ') ++text;
            background = true;
            return *text == 0 && append() && (!redirect || target_present);
        }
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

static void command(sh_session& session, std::string text, const rt_env& info) {
    str_writer console{info.selector(rt_stdout)};
    const auto split = text.find(' ');
    const auto name = std::string_view{text}.substr(0, split);
    const auto tail = split == text.npos ? std::string_view{} : std::string_view{text}.substr(split + 1);
    std::string argument_copy{tail};
    char* argument = argument_copy.data();
    const std::string name_copy{name};
    const auto line = name_copy.c_str();
    if (name.empty()) return;
    if (name == "help") {
        console.write("PROGRAM [ARGS] [&] | PIPELINE | jobs | status | fg/bg/kill/wait [ID]\n"
            "cd [DIR] | pwd | ls | cat | edit | tasks | touch | mkdir [-p] | rm [-rf] | mv | cp | stat\n"
            "PROGRAM > FILE | PROGRAM >> FILE | svc [start/stop/restart SERVICE] | mkfs\n"
            "Ctrl+C terminates the foreground job; Ctrl+Z suspends it.\n");
        return;
    }
    if (std::string_view{line} == "svc") {
        ubus_conn bus;
        auto status = bus.bind(info.selector(ubus_directory));
        if (status != STATUS_OK) { error(console, status); return; }
        const auto split = tail.find(' ');
        const auto verb = tail.substr(0, split);
        const auto target = split == tail.npos ? std::string_view{} : tail.substr(split + 1);
        for (const auto role : {"system", "session", "devices"}) {
            ubus_conn manager;
            status = manager.connect(bus, role, "myos.manager", 1, 1);
            if (status != STATUS_OK) { error(console, status); continue; }
            ubus_msg response;
            status = manager.invoke("List", {}, response);
            if (status != STATUS_OK) { error(console, status); continue; }
            for (const auto service : response.data["items"].values()) {
                const auto service_name = service["name"].as<std::string_view>().value_or("?");
                if (tail.empty()) {
                    (void)libk::fmt::format_to<"{} {}\n">(console, service_name, service["state"].as<std::string_view>().value_or("?"));
                } else if (target == service_name) {
                    if (verb != "start" && verb != "stop" && verb != "restart") { error(console, STATUS_BAD_ARGS); return; }
                    ubus_msg request, result;
                    request.data.put("id", service["id"]).put("gen", service["gen"]).put("action", verb);
                    status = manager.invoke("Action", std::move(request), result);
                    if (status == STATUS_OK) console.write("service change requested\n"); else error(console, status);
                    return;
                }
            }
        }
        if (!tail.empty()) error(console, STATUS_NOT_FOUND);
        return;
    }
    if (std::string_view{line} == "mkfs") {
        uint8_t id[fs_volume_id_size]{};
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
        ubus_conn directory, admin;
        auto status = directory.bind(info.selector(ubus_directory));
        if (status == STATUS_OK) status = admin.connect(directory, "store.admin", "myos.files", 1, 1);
        if (status == STATUS_OK) status = fs_format(admin, text.empty() ? std::span<const uint8_t>{} : std::span<const uint8_t>{id});
        if (status != STATUS_OK)
            error(console, status);
        return;
    }
    if (name == "jobs") { session.list(console); return; }
    if (name == "cd" || name == "pwd") {
        std::vector<std::string> words;
        std::vector<size_t> cuts{0};
        char target[128]{};
        bool append{}, background{};
        if (!arguments(text.data(), words, cuts, target, append, background) || cuts.size() != 1
            || *target || background || words.size() > (name == "cd" ? 2U : 1U)) {
            error(console, STATUS_BAD_ARGS); return;
        }
        if (name == "pwd") { console.write(session.cwd().c_str()); console.write("\n"); }
        else if (const auto status = session.cd(words.size() == 1 ? "/" : std::string_view{words[1]}); status != STATUS_OK)
            error(console, status);
        return;
    }
    if (name == "status") { session.status(console); return; }
    if (name == "fg" || name == "bg" || name == "kill" || name == "wait") {
        uint64_t id = session.latest(name == "fg" || name == "bg"), deadline{};
        if (!tail.empty()) {
            const auto split = tail.find(' ');
            const auto number = libk::parse<uint64_t>(tail.substr(0, split));
            if (!number) { error(console, STATUS_BAD_ARGS); return; }
            id = *number;
            if (name == "wait" && split != tail.npos) {
                const auto duration = libk::parse<uint64_t>(tail.substr(split + 1));
                const auto time = duration ? rt_after_ms(*duration) : std::nullopt;
                if (!time) { error(console, STATUS_BAD_ARGS); return; }
                deadline = *time;
            }
        }
        if (const auto status = session.control(name, id, deadline); status != STATUS_OK) error(console, status);
        return;
    }
    const bool read_only = name == "view";
    if (read_only) text = std::string{tail};
    bool background{};
    std::vector<std::string> words;
    const bool file_command = name == "stat" || name == "create" || name == "write" || name == "append";
    if (file_command) words.emplace_back("fs");
    std::vector<size_t> cuts{0};
    char target[128]{};
    bool append{};
    if (!arguments(text.data(), words, cuts, target, append, background) || words.size() == cuts.back()) {
        error(console, STATUS_BAD_ARGS); return;
    }
    if (*target) {
        cuts.push_back(words.size()); words.insert(words.end(), {"fs", "save", target});
        if (append) words.emplace_back("-a");
    }
    cuts.push_back(words.size());
    if (const auto status = session.spawn(words, cuts, background, text, read_only); status != STATUS_OK) error(console, status);
}

int main() {
    const auto& info = rt_startup();
    sh_session session{info};
    str_writer{info.selector(rt_stdout)}.write("myos native shell\n");
    session.prompt();
    for (;;) {
        command(session, session.read(), info);
        session.prompt();
    }
}
