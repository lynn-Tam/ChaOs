#include <exec.hpp>
#include <protocols/file.hpp>
#include <protocols/service.hpp>
#include <protocols/manage.hpp>
#include <protocols/terminal.hpp>
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
    std::unique_ptr<pr_task> task;
    std::optional<status_t> result;
    bool end{};
    cap_owner end_sender;
};
enum class sh_state { Running, Suspending, Suspended, Terminating, Done };
struct sh_job {
    uint64_t id;
    std::string command;
    std::vector<sh_stage> stages;
    std::vector<std::unique_ptr<ch_pair>> pipes;
    ch_pair input, output, terminal;
    cap_owner input_sender, terminal_sender;
    msg_rx output_rx, terminal_rx;
    std::deque<msg_frame> pending_input;
    sh_state state{sh_state::Running};
    bool fullscreen{}, output_empty{}, background{}, announced{}, read_only{};
    uint64_t focus{};
};

class sh_session {
    const rt_env& env_;
    msg_rx input_;
    rpc_client services_;
    ch_pair management_;
    msg_rx management_rx_;
    std::deque<std::unique_ptr<sh_job>> jobs_;
    uint64_t next_{}, foreground_{}, focus_{};
    cap_owner device_control_;
    struct output { msg_frame frame; uint64_t owner{}, focus{}; };
    std::deque<output> output_;
    std::string line_;
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
                || output_.back().frame.size == sizeof(msg_frame::data)) {
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
        msg_frame event;
        while (msg_recv(job.terminal.ends[1].selector(), event, false).status == STATUS_OK) {}
        event = {.operation = static_cast<uint64_t>(gained ? tm_op::FocusGained : tm_op::FocusLost),
                 .value = job.focus};
        event.size = sizeof(tm_default_size);
        ::memcpy(event.data, &tm_default_size, event.size);
        (void)msg_send(job.terminal_sender.selector(), event, false);
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
            } else { line_.clear(); overflow_ = false; enqueue("^C\nmyos> "); }
            return;
        }
        if (target) {
            if (target->pending_input.size() == 32) { enqueue("\a"); return; }
            if (byte == 4 && !target->fullscreen)
                target->pending_input.push_back({.operation = static_cast<uint64_t>(str_frame::End)});
            else {
                msg_frame frame{.size = 1}; frame.data[0] = byte;
                target->pending_input.push_back(frame);
            }
            return;
        }
        if (command_) return;
        if (byte == '\n') {
            enqueue("\n"); line_start_ = true; prompt_visible_ = false;
            if (overflow_) { enqueue("line too long\nmyos> "); line_.clear(); overflow_ = false; }
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
        }
        auto directory = sv_lookup(services_, vfs_exec);
        if (!directory) return directory.error();
        rpc_client files;
        auto status = files.open(directory->selector(), env_.pool, env_.cspace, env_.events);
        if (status != STATUS_OK) return status;
        auto image = fl_map(files, path.c_str(), VM_READ | VM_EXECUTE);
        if (!image) return image.error();
        auto mapped = vm_area::map(env_.vspace, std::move(image->memory), 0,
            libk::checked_align_up(image->size, 4096).value_or(0), VM_READ);
        if (!mapped) return mapped.error();
        auto writable = sv_lookup(services_, vfs_service);
        auto readable = sv_lookup(services_, vfs_read);
        if (!writable || !readable) return !writable ? writable.error() : readable.error();
        std::vector<pr_cap> caps{
            {.contract = rt_stdin, .cap = index ? job.pipes[index - 1]->ends[1].selector() : job.input.ends[1].selector(), .limit = word_t{RIGHT_RECEIVE}},
            {.contract = rt_stdout, .cap = index + 1 < job.stages.size() ? job.pipes[index]->ends[0].selector() : job.output.ends[0].selector(), .limit = word_t{RIGHT_SEND}, .badge = 1},
            {.contract = rt_stderr, .cap = job.output.ends[0].selector(), .limit = word_t{RIGHT_SEND}, .badge = 1},
            {.contract = vfs_service, .cap = writable->selector(), .limit = word_t{RIGHT_SEND}},
            {.contract = vfs_read, .cap = readable->selector(), .limit = word_t{RIGHT_SEND}},
        };
        caps.push_back({.contract = mg_jobs, .cap = management_.ends[1].selector(),
            .limit = word_t{RIGHT_SEND}, .badge = job.read_only ? mg_observe : mg_control});
        if (const auto control = env_.selector(job.read_only ? mg_tasks_view : mg_tasks))
            caps.push_back({.contract = mg_tasks, .cap = control, .limit = word_t{RIGHT_SEND}});
        if (const auto observer = env_.selector(mg_tasks_view))
            caps.push_back({.contract = mg_tasks_view, .cap = observer, .limit = word_t{RIGHT_SEND}});
        if (auto devices = sv_lookup(services_, job.read_only ? mg_devices_view : mg_devices)) {
            caps.push_back({.contract = mg_devices, .cap = devices->selector(), .limit = word_t{RIGHT_SEND}});
            // pr_exec copies this endpoint while its slot is alive.
            device_control_ = std::move(*devices);
        }
        if (job.stages.size() == 1)
            caps.push_back({.contract = tm_service, .cap = job.terminal.ends[1].selector(), .limit = word_t{RIGHT_SEND | RIGHT_RECEIVE}, .badge = 1});
        cap_t resource{};
        ScInfo scheduling{};
        for (uint64_t i = 0;; ++i) {
            const auto name = "cpu." + std::to_string(i);
            const auto cap = env_.selector({name.c_str(), 0, ObjSc});
            if (!cap) break;
            if (sys::sc_info(cap).status != STATUS_OK) continue;
            ::memcpy(&scheduling, reinterpret_cast<const void*>(env_.ipc), sizeof(scheduling));
            if (!scheduling.bound) { resource = cap; break; }
        }
        if (!resource) return STATUS_BUSY;
        auto cpu = static_cast<uint32_t>((job.id + index) % env_.cpus);
        if (!(scheduling.cpus & (uint64_t{1} << cpu))) cpu = std::countr_zero(scheduling.cpus);
        stage.task = std::make_unique<pr_task>();
        status = pr_exec(*stage.task, {env_.pool, env_.vspace, static_cast<uint32_t>(env_.cpus)},
            {.bytes = {reinterpret_cast<const std::byte*>(mapped->address), image->size},
             .memory = mapped->memory.selector(), .perms = VM_READ | VM_EXECUTE},
            {.argv = argv, .caps = caps,
             .resources = {.bytes = 4 * 1024 * 1024, .caps = 160,
                .kinds = pr_base_kinds | OBJ_BIT(ObjChannel), .slots = 160, .pages = 24,
                .pool_perms = RIGHT_CREATE | RIGHT_INSPECT},
             .cpu = cpu, .events = env_.events});
        if (status == STATUS_OK) {
            const auto copy = sys::cap_duplicate(resource, 0, RIGHT_ATTACH | RIGHT_INSPECT);
            status = copy.status;
            if (status == STATUS_OK) status = stage.task->start(cap_owner{copy.value});
        }
        return status;
    }
public:
    explicit sh_session(const rt_env& env) : env_(env), input_(env.selector(con_input), env.events) {
        if (const auto status = services_.open(env.selector(sv_service), env.pool, env.cspace, env.events); status != STATUS_OK) sys::exit(status);
        if (const auto status = management_.open(env.pool, 8, 1); status != STATUS_OK) sys::exit(status);
        if (const auto status = management_rx_.open(management_.ends[0].selector(), env.events); status != STATUS_OK) sys::exit(status);
    }
    void prompt() {
        if (!line_start_ && !prompt_visible_) enqueue("\n");
        enqueue("myos> "); enqueue(line_); prompt_visible_ = true;
    }
    void terminate(sh_job& job) {
        job.state = sh_state::Terminating;
        leave(job);
        for (auto& stage : job.stages) if (stage.task) (void)stage.task->stop();
    }
    void manage(const msg_packet& packet, msg_reply& response) {
        const auto& request = packet.message;
        auto& message = response.message;
        message.status = STATUS_NOT_FOUND;
        sh_job* target{};
        sh_stage* stage{};
        uint64_t id{};
        const auto op = static_cast<mg_op>(request.operation);
        for (auto& entry : jobs_) {
            for (size_t i = 0; i < entry->stages.size(); ++i) {
                const auto candidate = (entry->id << 8) | i;
                if (op == mg_op::Tasks ? candidate > request.value : candidate == request.value) {
                    target = entry.get(); stage = &entry->stages[i]; id = candidate; break;
                }
            }
            if (target) break;
        }
        if (!target) return;
        if (op == mg_op::Tasks || op == mg_op::Task) {
            constexpr mg_state states[]{mg_state::Running, mg_state::Suspending, mg_state::Suspended, mg_state::Draining, mg_state::Done};
            mg_task row{id, target->id, target->id, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
                static_cast<uint32_t>(states[static_cast<size_t>(target->state)]),
                static_cast<uint32_t>(mg_observe | (packet.badge == mg_control && target->id != foreground_ ? mg_control | mg_close | mg_pause | mg_resume | mg_stop : 0)), {}};
            if (stage->task) {
                if (const auto thread = stage->task->info()) row.runtime = thread->runtime;
                if (const auto memory = stage->task->memory_info()) row.resident = memory->resident;
                if (const auto quota = stage->task->resource_info()) {
                    row.charged = quota->memory_limit - quota->memory_available; row.limit = quota->memory_limit;
                }
            }
            ::memcpy(row.name, target->command.data(), std::min(target->command.size(), sizeof(row.name) - 1));
            mg_pack(message, row); message.value = id; message.deadline = sys::clock_now().value; return;
        }
        if (request.size != sizeof(mg_target)) { message.status = STATUS_BAD_ARGS; return; }
        mg_target expected; ::memcpy(&expected, request.data, sizeof(expected));
        if (expected.incarnation != target->id) { message.status = STATUS_STALE; return; }
        if (op == mg_op::Dependencies) {
            const auto index = static_cast<size_t>(stage - target->stages.data());
            const bool upstream = index && expected.argument == 0;
            if (!upstream && (expected.argument != static_cast<uint64_t>(index != 0) || index + 1 == target->stages.size())) return;
            mg_dependency edge{(target->id << 8) | (upstream ? index - 1 : index + 1), target->id, 0, 1, 0, 4, {}};
            const std::string_view name{upstream ? "stdin pipe" : "stdout pipe"};
            ::memcpy(edge.name, name.data(), name.size());
            mg_pack(message, edge); message.value = expected.argument + 1; return;
        }
        if (op == mg_op::Caps) {
            if (!stage->task) { message.status = STATUS_CLOSED; return; }
            auto records = stage->task->capabilities(expected.argument);
            if (!records) { message.status = records.error(); return; }
            if (!records->count) return;
            mg_pack(message, records->records[0]);
            message.value = records->count > 1 || records->cursor ? records->records[0].selector : 0; return;
        }
        if (packet.badge != mg_control || target->id == foreground_) { message.status = STATUS_DENIED; return; }
        if (op == mg_op::CloseCap) {
            message.status = stage->task ? sys::cap_close(expected.argument, stage->task->cspace()).status : STATUS_CLOSED;
            if (message.status == STATUS_INVALID_CAP) message.status = STATUS_STALE;
            return;
        }
        if (op != mg_op::Action || !stage->task) { message.status = STATUS_CLOSED; return; }
        const auto action = static_cast<mg_action>(request.status);
        if (action == mg_action::Stop) { terminate(*target); message.status = STATUS_OK; }
        else if (action == mg_action::Pause || action == mg_action::Resume) {
            const bool enable = action == mg_action::Resume;
            if (!enable) leave(*target);
            target->state = enable ? sh_state::Running : sh_state::Suspending;
            message.status = STATUS_OK;
            for (auto& member : target->stages)
                if (member.task) {
                    const auto status = member.task->execution(enable).status;
                    if (status != STATUS_OK) message.status = status;
                }
        } else message.status = STATUS_INVALID_OP;
    }
    void pump(bool sleep = true) {
        for (unsigned n = 0; n < 16; ++n) {
            msg_packet packet;
            const auto status = management_rx_.receive(packet);
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK || packet.count != 1) continue;
            msg_reply response;
            response.message.operation = packet.message.operation;
            if (packet.badge != mg_observe && packet.badge != mg_control) response.message.status = STATUS_DENIED;
            else manage(packet, response);
            response.respond(packet.capabilities[0].selector());
        }
        (void)management_rx_.arm();
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
                        const auto result = stage.task->collect();
                        if (!stage.result) stage.result = static_cast<status_t>(result.value);
                        stage.task.reset(); stage.end = i + 1 < job.stages.size();
                        if (i) (void)sys::channel_close(job.pipes[i - 1]->ends[0].selector());
                    } else if (status != STATUS_WOULD_BLOCK && !pr_retryable(status)) sys::exit(status);
                }
                if (stage.end) {
                    const auto status = msg_send(stage.end_sender.selector(),
                        {.operation = static_cast<uint64_t>(str_frame::End), .status = *stage.result}, false).status;
                    if (status != STATUS_WOULD_BLOCK && status != STATUS_BUSY) stage.end = false;
                }
                done &= !stage.task && !stage.end;
            }
            if (job.state == sh_state::Suspending && suspended) job.state = sh_state::Suspended;
            for (unsigned i = 0; i < 16; ++i) {
                if (foreground_ == job.id && output_.size() >= 64) break;
                msg_frame frame;
                const auto status = job.terminal_rx.try_receive(frame).status;
                if (status == STATUS_WOULD_BLOCK) break;
                if (status != STATUS_OK) break;
                if (frame.operation == static_cast<uint64_t>(tm_op::Fullscreen)) {
                    job.fullscreen = true;
                    if (foreground_ == job.id) focus(job, true);
                } else if (frame.operation == static_cast<uint64_t>(tm_op::Frame)
                    && foreground_ == job.id && frame.value == job.focus && output_.size() < 64)
                    output_.push_back({frame, job.id, job.focus});
            }
            if (done && job.fullscreen && foreground_ == job.id) leave(job);
            if (!screen_ && output_.size() < 64) {
                msg_frame frame;
                const auto status = job.output_rx.try_receive(frame).status;
                job.output_empty = status == STATUS_WOULD_BLOCK;
                if (status == STATUS_OK && frame.operation == static_cast<uint64_t>(str_frame::Data)) {
                    frame.operation = static_cast<uint64_t>(con_operation::Bytes);
                    const bool background = !foreground_;
                    if (background && prompt_visible_) { enqueue("\r\x1b[2K"); prompt_visible_ = false; }
                    output_.push_back({frame, job.id, 0});
                    line_start_ = frame.size && (frame.data[frame.size - 1] == '\n' || frame.data[frame.size - 1] == '\r');
                    if (background && line_start_ && reading_) prompt();
                }
            }
            while (!job.pending_input.empty()) {
                const auto status = msg_send(job.input_sender.selector(), job.pending_input.front(), false).status;
                if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) break;
                job.pending_input.pop_front();
            }
            if (done && job.output_empty && job.state != sh_state::Done) {
                leave(job); job.state = sh_state::Done;
            }
            if (job.state == sh_state::Done && job.background && !job.announced && reading_ && !foreground_) {
                libk::fmt::fixed_buffer<96> notice;
                (void)libk::fmt::format_to<"\r\x1b[2K[{}] Done {}\n">(notice, job.id, job.stages.back().result.value_or(STATUS_CANCELED));
                enqueue(notice.view()); line_start_ = true; prompt_visible_ = false; prompt(); job.announced = true;
            }
            (void)job.terminal_rx.arm();
            (void)job.output_rx.arm();
        }
        for (unsigned i = 0; i < 16; ++i) {
            msg_frame frame;
            const auto status = input_.try_receive(frame).status;
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) sys::exit(status);
            for (size_t i = 0; i < frame.size; ++i) input(frame.data[i]);
        }
        while (!output_.empty()) {
            auto& next = output_.front();
            if (next.focus && (next.owner != foreground_ || !job(next.owner) || next.focus != job(next.owner)->focus)) { output_.pop_front(); continue; }
            next.frame.operation = static_cast<uint64_t>(con_operation::Bytes);
            const auto status = msg_send(env_.selector(con_output), next.frame, false).status;
            if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) break;
            if (status != STATUS_OK) sys::exit(status);
            output_.pop_front();
        }
        (void)input_.arm();
        if (sleep && !command_) {
            // A bounded sampling tick also advances asynchronous owner retirement
            // and output backpressure; readiness handles ordinary idle wakeups.
            const auto next = rt_after_ms(1);
            if (next) (void)sys::notification_wait(env_.events, *next);
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
        auto status = job.input.open(env_.pool, 8, 0);
        if (status == STATUS_OK) status = job.output.open(env_.pool, 8, 0);
        if (status == STATUS_OK) status = job.terminal.open(env_.pool, 8, 0);
        if (status == STATUS_OK) {
            const auto input = sys::channel_mint(job.input.ends[0].selector(), env_.cspace, 1, RIGHT_SEND);
            job.input_sender = cap_owner{input.value}; status = input.status;
        }
        if (status == STATUS_OK) {
            const auto terminal = sys::channel_mint(job.terminal.ends[0].selector(), env_.cspace, 1, RIGHT_SEND);
            job.terminal_sender = cap_owner{terminal.value}; status = terminal.status;
        }
        if (status == STATUS_OK) status = job.output_rx.open(job.output.ends[1].selector(), env_.events);
        if (status == STATUS_OK) status = job.terminal_rx.open(job.terminal.ends[0].selector(), env_.events);
        for (size_t i = 1; i < job.stages.size() && status == STATUS_OK; ++i) {
            auto& pipe = job.pipes.emplace_back(std::make_unique<ch_pair>());
            status = pipe->open(env_.pool, 4, 0);
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
    str_writer console{info.selector(con_output)};
    const auto split = text.find(' ');
    const auto name = std::string_view{text}.substr(0, split);
    const auto tail = split == text.npos ? std::string_view{} : std::string_view{text}.substr(split + 1);
    std::string argument_copy{tail};
    char* argument = argument_copy.data();
    const auto control = info.selector(sv_service), pool = info.pool, cspace = info.cspace, events = info.events;
    const std::string name_copy{name};
    const auto line = name_copy.c_str();
    if (name.empty()) return;
    if (name == "help") {
        console.write("PROGRAM [ARGS] [&] | PIPELINE | jobs | status | fg/bg/kill/wait [ID]\n"
            "view PROGRAM | ls | cat | edit | tasks | mkdir | create | write/append | rm | mv | cp | stat\n"
            "PROGRAM > FILE | PROGRAM >> FILE | svc [start/stop/restart SERVICE] | mkfs\n"
            "Ctrl+C terminates the foreground job; Ctrl+Z suspends it.\n");
        return;
    }
    if (std::string_view{line} == "svc") {
        rpc_client rpc;
        auto status = rpc.open(info.selector(mg_tasks), pool, cspace, events);
        if (status != STATUS_OK) { error(console, status); return; }
        const auto split = tail.find(' ');
        const auto verb = tail.substr(0, split);
        const auto target = split == tail.npos ? std::string_view{} : tail.substr(split + 1);
        uint64_t cursor{};
        for (;;) {
            auto request = mg_request(mg_op::Tasks, cursor);
            status = rpc.exchange(request);
            if (status == STATUS_NOT_FOUND) break;
            if (status != STATUS_OK || request.size != sizeof(mg_task)) { error(console, status); return; }
            mg_task service; ::memcpy(&service, request.data, sizeof(service));
            cursor = service.id;
            if (tail.empty()) {
                constexpr const char* names[]{"stopped", "draining", "starting", "running", "failed", "suspending", "suspended", "done"};
                (void)libk::fmt::format_to<"{} {}\n">(console, service.name, names[std::min(service.state, 7U)]);
            } else if (target == service.name) {
                const auto action = verb == "start" ? mg_action::Start : verb == "stop" ? mg_action::Stop : mg_action::Restart;
                if (verb != "start" && verb != "stop" && verb != "restart") { error(console, STATUS_BAD_ARGS); return; }
                request = mg_request(mg_op::Action, service.id, service.incarnation, 0, action);
                status = rpc.exchange(request);
                if (status == STATUS_OK) console.write("service change requested\n");
                else error(console, status);
                return;
            }
        }
        if (!tail.empty()) error(console, STATUS_NOT_FOUND);
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
        rpc_client directory, admin;
        auto status = directory.open(control, pool, cspace, events);
        auto endpoint = status == STATUS_OK
                            ? sv_lookup(directory, st_admin)
                            : std::expected<cap_owner, status_t>{std::unexpected(status)};
        if (!endpoint) {
            error(console, endpoint.error());
            return;
        }
        status = admin.open(endpoint->selector(), pool, cspace, events);
        if (status == STATUS_OK)
            status = st_format(admin, text.empty() ? nullptr : id);
        if (status != STATUS_OK)
            error(console, status);
        return;
    }
    if (name == "jobs") { session.list(console); return; }
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
    const bool file_command = name == "stat" || name == "create" || name == "write" || name == "append"
        || name == "mkdir" || name == "rm" || name == "mv";
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
    str_writer{info.selector(con_output)}.write("myos native shell\n");
    session.prompt();
    for (;;) {
        command(session, session.read(), info);
        session.prompt();
    }
}
