#include <vm.hpp>
#include <proc.hpp>

#include <mgr/mgr.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/parse.hpp>
#include <libk/fmt.hpp>
#include <bit>
#include <deque>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

struct sh_chunk { status_t status{}; size_t size{}; char data[80]{}; };
static auto sh_send(cap_t channel, const sh_chunk& chunk, bool block = true) -> status_t {
    ubus_msg message; message.status = chunk.status;
    if (chunk.size) message.data.put("data", std::as_bytes(std::span{chunk.data, chunk.size}));
    return ubus_send(channel, message, block);
}
static auto sh_receive(ubus_rx& input, sh_chunk& chunk) -> status_t {
    ubus_msg message; const auto status = input.receive(message); if (status != STATUS_OK) return status;
    chunk = {}; chunk.status = message.status;
    if (message.count) return STATUS_PEER_FAULT;
    if (const auto bytes = message.data["data"].as<std::span<const std::byte>>()) {
        if (bytes->size() > sizeof(chunk.data)) return STATUS_PEER_FAULT;
        chunk.size = bytes->size(); ::memcpy(chunk.data, bytes->data(), bytes->size());
    } else if (message.data["data"]) return STATUS_PEER_FAULT;
    return STATUS_OK;
}
class sh_writer {
    cap_t output_;
public:
    explicit sh_writer(cap_t output) : output_(output) {}
    void write(const char* data, size_t size) const {
        while (size) {
            sh_chunk chunk; chunk.size = std::min(size, sizeof(chunk.data)); ::memcpy(chunk.data, data, chunk.size);
            const auto status = sh_send(output_, chunk); if (status != STATUS_OK) sys::exit(status);
            data += chunk.size; size -= chunk.size;
        }
    }
    void write(const char* text) const { write(text, ::strlen(text)); }
    void put(char value) const { write(&value, 1); }
};
static auto sh_path(std::string_view path, std::string_view base = "/") -> std::string {
    std::string result = path.starts_with('/') ? std::string{path} : std::string{base} + "/" + std::string{path};
    size_t size{}, cursor{};
    while (cursor < result.size()) {
        const auto begin = cursor; while (cursor < result.size() && result[cursor] != '/') ++cursor;
        const std::string_view part{result.data() + begin, cursor - begin};
        if (part == "..") { while (size && result[size - 1] != '/') --size; if (size) --size; }
        else if (!part.empty() && part != ".") { if (size) result[size++] = '/'; ::memmove(result.data() + size, part.data(), part.size()); size += part.size(); }
        ++cursor;
    }
    result.resize(size); result.insert(0, "/"); return result;
}

struct sh_stage {
    std::shared_ptr<mgr_child> child;
    auto task() const -> proc_task* { return child ? child->task.get() : nullptr; }
    std::optional<status_t> result;
    bool end{};
    cap_owner end_sender;
    ev_watch end_ready;
};
enum class sh_state { Running, Suspending, Suspended, Terminating, Done };
struct sh_pipeline {
    uint64_t id;
    std::string command;
    std::vector<sh_stage> stages;
    std::vector<std::unique_ptr<ch_pair>> pipes;
    ch_pair input, output;
    ubus_object terminal;
    cap_owner input_sender, terminal_sender, terminal_connector;
    ev_watch terminal_ready;
    std::optional<ubus_msg> focus_event;
    std::optional<bool> execution;
    ev_watch input_ready;
    ubus_rx output_rx;
    std::deque<sh_chunk> pending_input;
    sh_state state{sh_state::Running};
    bool fullscreen{}, output_empty{}, background{}, announced{};
    std::string profile;
    uint64_t focus{};
};

class sh_shell {
    const rt_env& env_;
    ubus_rx input_;
    ubus_conn bus_, files_;
    ubus_msg setup_;
    vm_area source_;
    cap_t input_cap_{}, output_cap_{};
    mgr owner_{[this](mgr_child& child) { return image(child.image); }};
    std::deque<std::unique_ptr<sh_pipeline>> pipelines_;
    uint64_t next_{}, foreground_{}, focus_{};
    struct output { sh_chunk frame; uint64_t owner{}, focus{}; };
    std::deque<output> output_;
    ev_watch output_ready_;
    std::string line_;
    std::string cwd_{"/"};
    std::vector<status_t> result_;
    std::optional<std::string> command_;
    bool carriage_{}, overflow_{}, screen_{}, reading_{}, prompt_visible_{}, line_start_{true};
    auto job(uint64_t id) noexcept -> sh_pipeline* {
        for (auto& job : pipelines_) if (job->id == id) return job.get();
        return nullptr;
    }
    auto collect(sh_pipeline& job) -> status_t {
        result_.clear();
        for (const auto& stage : job.stages) result_.push_back(stage.result.value_or(STATUS_CANCELED));
        const auto status = result_.back();
        const auto id = job.id;
        for (auto& stage : job.stages) if (stage.child) {
            for (;;) {
                const auto status = owner_.erase(stage.child->child_id);
                if (status == STATUS_OK || status == STATUS_NOT_FOUND) break;
                if (status != STATUS_WOULD_BLOCK) sys::exit(status);
                pump();
            }
            stage.child = nullptr;
        }
        std::erase_if(pipelines_, [=](const auto& job) { return job->id == id; });
        return status;
    }
    void enqueue(std::string_view text, uint64_t owner = 0, uint64_t focus = 0) {
        while (!text.empty()) {
            if (output_.empty() || output_.back().owner != owner || output_.back().focus != focus
                || output_.back().frame.size == sizeof(sh_chunk::data)) {
                if (output_.size() == 128) return; // Echo/status overflow; child streams stop draining at 64.
                output_.push_back({{}, owner, focus});
            }
            auto& frame = output_.back().frame;
            const auto count = std::min(text.size(), sizeof(frame.data) - frame.size);
            ::memcpy(frame.data + frame.size, text.data(), count);
            frame.size += count; text.remove_prefix(count);
        }
    }
    void focus(sh_pipeline& job, bool gained) {
        // Supersede pending screen/control text; ordinary job bytes keep order.
        std::erase_if(output_, [](const auto& output) { return output.focus || !output.owner; });
        job.focus = ++focus_;
        // Old queued events can be replaced; old in-flight frames still carry
        // their old focus sequence and are rejected by the output broker.
        ubus_msg event;
        event.data.put("focus", job.focus).put("focused", gained).put("rows", uint64_t{24})
            .put("columns", uint64_t{80});
        job.focus_event.emplace(std::move(event));
        if (gained) { enqueue("\x1b[?1049h\x1b[?25h"); screen_ = true; }
        else if (screen_) { enqueue("\x1b[?1049l\x1b[?25h"); screen_ = false; }
    }
    void leave(sh_pipeline& job) {
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
                    target->execution = false;
                }
            } else { line_.clear(); overflow_ = false; enqueue("^C\n"); line_start_ = true; prompt_visible_ = false; prompt(); }
            return;
        }
        if (target) {
            if (target->pending_input.size() == 32) { enqueue("\a"); return; }
            if (byte == 4 && !target->fullscreen)
                target->pending_input.push_back({});
            else {
                sh_chunk frame{.size = 1}; frame.data[0] = byte;
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
    auto image(std::string_view path) -> std::expected<proc_image, status_t> {
        ubus_msg query, response;
        auto status = files_.state();
        if (status != STATUS_OK) {
            if (const auto closed = files_.close(); closed != STATUS_OK) return std::unexpected(closed);
            query.data.put("name", "exec");
            status = bus_.invoke("Get", std::move(query), response);
            if (status == STATUS_OK) status = files_.bind(response.capability(response.data["connector"]), "io.files", 1, 1);
            if (status != STATUS_OK) return std::unexpected(status);
        }
        query = {}; query.data.put("path", path).put("perms", word_t{VM_READ | VM_EXECUTE});
        status = files_.invoke("Map", std::move(query), response); if (status != STATUS_OK) return std::unexpected(status);
        const auto buffer = response.data["buffer"].as<ubus_buffer>(); if (!buffer) return std::unexpected(STATUS_PEER_FAULT);
        return proc_image::map(source_, response.take(response.data["buffer"]), buffer->offset, buffer->size, VM_READ | VM_EXECUTE);
    }
    auto launch(sh_stage& stage, sh_pipeline& job, size_t index, std::span<const std::string_view> argv) -> status_t {
        const auto config = setup_.data["message"];
        const auto profile = config["profiles"][job.profile];
        auto policy = profile["commands"][argv[0]];
        if (!policy) policy = config["commands"][argv[0]];
        std::string path{policy["program"].as<std::string_view>().value_or(argv[0])};
        if (path.find('/') == path.npos) { path = "/boot/" + path; if (!path.ends_with(".elf")) path += ".elf"; }
        else path = sh_path(path, cwd_);
        const auto base = path.substr(path.find_last_of('/') + 1);
        const auto limits = policy["resources"];
        const auto caps = limits["caps"].as<uint64_t>().value_or(160);
        const auto bytes = limits["bytes"].as<uint64_t>().value_or(4 * 1024 * 1024);
        auto& child = owner_.add(base + "." + std::to_string(job.id) + "." + std::to_string(index), path,
            {.bytes=bytes, .caps=caps, .slots=std::max<uint64_t>(256, caps), .pages=(std::max<uint64_t>(256, caps) + 7) / 8 + 4,
                .quota_perms=RIGHT_CREATE | RIGHT_INSPECT | (limits["children"].as<bool>().value_or(false) ? RIGHT_SPLIT : 0),
                .watches=limits["watches"].as<uint64_t>().value_or(64), .hints=limits["hints"].as<uint64_t>().value_or(16), .threads=limits["threads"].as<uint64_t>().value_or(64), .max_bytes=std::max<uint64_t>(bytes, 16 * 1024 * 1024), .max_caps=std::max<uint64_t>(caps, 256)});
        stage.child = owner_.find(child.child_id); child.args.clear();
        if (policy["argv"]) for (const auto arg : policy["argv"].values()) child.args.emplace_back(*arg.as<std::string_view>());
        else child.args = {std::string{argv[0]}};
        for (const auto arg : argv.subspan(1)) child.args.emplace_back(arg);
        child.env = {"PWD=" + cwd_};
        child.cpu = (job.id + index) % env_.cpus;
        auto routes = profile["routes"]; if (!routes) routes = policy["routes"]; if (!routes) routes = config["routes"];
        if (routes && !child.routes.assign(routes.bytes())) return STATUS_BAD_ARGS;
        const auto delivery = profile ? profile["commands"][argv[0]]["setup"] : policy["setup"];
        if (delivery) {
            auto message = setup_.slice(delivery); if (!message) return message.error();
            child.setup = std::move(*message);
        }
        for (const auto& [name, source, rights, badge] : std::array{
            std::tuple{"stdin", index ? job.pipes[index - 1]->ends[1].selector() : job.input.ends[1].selector(), word_t{RIGHT_RECEIVE}, word_t{0}},
            std::tuple{"stdout", index + 1 < job.stages.size() ? job.pipes[index]->ends[0].selector() : job.output.ends[0].selector(), word_t{RIGHT_SEND}, word_t{1}},
            std::tuple{"stderr", job.output.ends[0].selector(), word_t{RIGHT_SEND}, word_t{1}}}) {
            const auto copy = badge ? sys::channel_mint(source, env_.cspace, badge, rights) : sys::cap_duplicate(source, env_.cspace, rights);
            if (copy.status != STATUS_OK) return copy.status;
            const auto cap_idx = child.setup.give(cap_owner{copy.value});
            if (!cap_idx) return cap_idx.error();
            child.setup.data.cap(name, *cap_idx);
        }
        if (job.stages.size() == 1) {
            const auto cap = sys::cap_duplicate(job.terminal_connector.selector(), env_.cspace, RIGHT_SEND | RIGHT_DUPLICATE);
            if (cap.status != STATUS_OK) return cap.status;
            const auto term_idx = child.setup.offer(cap_owner{cap.value}, RIGHT_SEND | RIGHT_DUPLICATE);
            if (!term_idx) return term_idx.error();
            child.setup.data.cap("terminal", *term_idx);
        }
        return owner_.start(child);
    }

public:
    explicit sh_shell(const rt_env& env) : env_(env) {
        auto status = bus_.bind(env.environment); if (status != STATUS_OK) sys::exit(status);
        status = bus_.invoke("Receive", {}, setup_); if (status != STATUS_OK) sys::exit(status);
        input_cap_ = setup_.capability(setup_.data["message"]["stdin"]);
        output_cap_ = setup_.capability(setup_.data["message"]["stdout"]);
        status = input_.open(input_cap_, rt_events()); if (status != STATUS_OK) sys::exit(status);
        status = owner_.open(); if (status != STATUS_OK) sys::exit(status);
        if (const auto status = owner_.attach(bus_); status != STATUS_OK) sys::exit(status);
    }
    void prompt() {
        if (!line_start_ && !prompt_visible_) enqueue("\n");
        enqueue(cwd_); enqueue(" > "); enqueue(line_); prompt_visible_ = true;
    }
    auto output() const -> cap_t { return output_cap_; }
    auto profile(std::string_view name) const -> bool { return bool(setup_.data["message"]["profiles"][name]); }
    auto cwd() const noexcept -> const std::string& { return cwd_; }
    auto cd(std::string_view path) -> status_t {
        if (path.empty()) return STATUS_BAD_ARGS;
        ubus_conn files;
        ubus_msg query, reply; query.data.put("name", "read");
        auto status = bus_.invoke("Get", std::move(query), reply);
        if (status == STATUS_OK) status = files.bind(reply.capability(reply.data["connector"]), "io.files", 1, 1);
        if (status != STATUS_OK) return status;
        const auto target = sh_path(path, cwd_);
        query = {}; query.data.put("path", target);
        status = files.invoke("List", std::move(query), reply);
        if (status == STATUS_OK) cwd_ = target;
        return status;
    }
    void terminate(sh_pipeline& job) {
        job.state = sh_state::Terminating;
        leave(job);
        for (auto& stage : job.stages) if (stage.task()) (void)stage.task()->stop();
    }
    void pump(bool sleep = true) {
        bool polling{}, progress{};
        if (const auto status = owner_.poll(); status != STATUS_OK) sys::exit(status);
        auto deadline = owner_.deadline();
        for (auto& entry : pipelines_) {
            auto& job = *entry;
            bool suspended = true, done = true;
            for (size_t i = 0; i < job.stages.size(); ++i) {
                auto& stage = job.stages[i];
                if (stage.task()) {
                    if (job.execution && !stage.task()->closing()) {
                        const auto state = stage.task()->execution_info();
                        const bool applied = state && state->requested == state->applied
                            && bool(state->flags & DOMAIN_ENABLED) == *job.execution && state->status == STATUS_OK;
                        suspended &= applied;
                        if (state && state->phase == DOMAIN_OPEN && !applied) {
                            const auto status = stage.task()->execution(*job.execution).status;
                            if (status != STATUS_OK && status != STATUS_BUSY) terminate(job);
                        }
                    }
                    polling |= stage.task()->polling();
                }
                if (stage.child && !stage.task() && !stage.result) {
                    stage.result = stage.child->result; stage.end = i + 1 < job.stages.size(); progress = true;
                    if (i) (void)sys::channel_close(job.pipes[i - 1]->ends[0].selector());
                }
                if (stage.end) {
                    const auto status = sh_send(stage.end_sender.selector(), {.status = *stage.result}, false);
                    if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
                        const auto armed = stage.end_ready ? stage.end_ready.arm().status
                            : stage.end_ready.open(rt_events(), stage.end_sender.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
                        if (armed != STATUS_OK) sys::exit(armed);
                    } else {
                        stage.end = false; progress = true;
                        if (const auto closed = stage.end_ready.close(); closed != STATUS_OK) sys::exit(closed);
                    }
                }
                done &= !stage.task() && !stage.end;
            }
            if (job.execution && suspended) {
                job.execution.reset();
                if (job.state == sh_state::Suspending) job.state = sh_state::Suspended;
                progress = true;
            }
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
                        : job.terminal_ready.open(rt_events(), job.terminal_sender.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
                    if (armed != STATUS_OK) sys::exit(armed);
                } else {
                    job.focus_event.reset();
                    if (const auto closed = job.terminal_ready.close(); closed != STATUS_OK) sys::exit(closed);
                    if (status != STATUS_OK) job.terminal_sender = {};
                    progress = true;
                }
            }
            if (done && job.fullscreen && foreground_ == job.id) leave(job);
            for (unsigned i = 0; !screen_ && output_.size() < 64 && i < 16; ++i) {
                sh_chunk frame;
                const auto status = sh_receive(job.output_rx, frame);
                job.output_empty = status == STATUS_WOULD_BLOCK || status == STATUS_CLOSED || status == STATUS_PEER_CLOSED;
                if (status != STATUS_OK) break;
                progress = true;
                if (frame.size) {
                    const bool background = !foreground_;
                    if (background && prompt_visible_) { enqueue("\r\x1b[2K"); prompt_visible_ = false; }
                    output_.push_back({frame, job.id, 0});
                    line_start_ = frame.size && (frame.data[frame.size - 1] == '\n' || frame.data[frame.size - 1] == '\r');
                    if (background && line_start_ && reading_) prompt();
                }
            }
            while (!job.pending_input.empty()) {
                const auto status = sh_send(job.input_sender.selector(), job.pending_input.front(), false);
                if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
                    const auto armed = job.input_ready ? job.input_ready.arm().status
                        : job.input_ready.open(rt_events(), job.input_sender.selector(), SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
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
            sh_chunk frame;
            const auto status = sh_receive(input_, frame);
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) sys::exit(status);
            progress = true;
            for (size_t i = 0; i < frame.size; ++i) input(frame.data[i]);
        }
        while (!output_.empty()) {
            auto& next = output_.front();
            if (next.focus && (next.owner != foreground_ || !job(next.owner) || next.focus != job(next.owner)->focus)) { output_.pop_front(); continue; }
            const auto status = sh_send(output_cap_, next.frame, false);
            if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
                const auto armed = output_ready_ ? output_ready_.arm().status
                    : output_ready_.open(rt_events(), output_cap_, SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
                if (armed != STATUS_OK) sys::exit(armed);
                break;
            }
            if (status != STATUS_OK) sys::exit(status);
            output_.pop_front(); progress = true;
        }
        if (output_.empty())
            if (const auto status = output_ready_.close(); status != STATUS_OK) sys::exit(status);
        (void)input_.arm();
        for (auto& entry : pipelines_) { (void)entry->terminal.arm(); (void)entry->output_rx.arm(); }
        if (sleep && !command_ && !progress) {
            // Successful executions use Exit/Scope/Quota notifications.
            if (polling) {
                const auto next = rt_after_ms(1).value_or(0);
                if (next && (!deadline || next < deadline)) deadline = next;
            }
            (void)ev_wait(rt_events(), deadline);
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
               bool background, std::string_view command, std::string_view profile = {}) -> status_t {
        auto entry = std::make_unique<sh_pipeline>();
        auto& job = *entry;
        job.id = ++next_; job.command = command; job.background = background; job.profile = profile;
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
        if (status == STATUS_OK) status = job.terminal.add("Frame", {{"focus", ubus_type::U64}, {"data", ubus_type::Bytes, false, sizeof(sh_chunk::data)}}, {},
            [this, &job](auto& request, auto&) {
                if (foreground_ != job.id || request.data["focus"].template as<uint64_t>() != job.focus) return STATUS_STALE;
                if (output_.size() >= 64) return STATUS_BUSY;
                sh_chunk frame;
                const auto bytes = *request.data["data"].template as<std::span<const std::byte>>();
                frame.size = bytes.size(); ::memcpy(frame.data, bytes.data(), frame.size);
                output_.push_back({frame, job.id, job.focus}); return STATUS_OK;
            }, 0, true);
        if (status == STATUS_OK) {
            auto connector = job.terminal.connector();
            if (!connector) status = connector.error(); else job.terminal_connector = std::move(*connector);
        }
        if (status == STATUS_OK) status = job.output_rx.open(job.output.ends[1].selector(), rt_events());
        for (size_t i = 1; i < job.stages.size() && status == STATUS_OK; ++i) {
            auto& pipe = job.pipes.emplace_back(std::make_unique<ch_pair>());
            status = pipe->open(env_.alloc(), 4, 0);
            if (status == STATUS_OK) {
                const auto end = sys::channel_mint(pipe->ends[0].selector(), env_.cspace, 1, RIGHT_SEND);
                job.stages[i - 1].end_sender = cap_owner{end.value}; status = end.status;
            }
        }
        if (status != STATUS_OK) return status;
        pipelines_.push_back(std::move(entry));
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
            if (job.state == sh_state::Terminating || std::ranges::all_of(job.stages, [](const auto& stage) { return !stage.task(); }))
                while (job.state != sh_state::Done) pump();
            if (job.state == sh_state::Done) return collect(job);
        }
        return STATUS_OK;
    }
    auto latest(bool active) const noexcept -> uint64_t {
        for (const auto& job : pipelines_ | std::views::reverse)
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
            target->execution = true;
            target->state = sh_state::Running;
            if (operation == "bg") { target->background = true; return STATUS_OK; }
        }
        if (operation == "fg") {
            while (foreground_) pump();
            if (target->state == sh_state::Terminating || std::ranges::all_of(target->stages, [](const auto& stage) { return !stage.task(); }))
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
    void status(sh_writer& console) const {
        for (size_t i = 0; i < result_.size(); ++i)
            (void)libk::fmt::format_to<"stage {}: {}\n">(console, i + 1, result_[i]);
    }
    void list(sh_writer& console) const {
        constexpr const char* names[]{"Running", "Suspending", "Suspended", "Terminating", "Done"};
        for (const auto& job : pipelines_)
            (void)libk::fmt::format_to<"[{}] {} {}\n">(console, job->id, names[static_cast<size_t>(job->state)], std::string_view{job->command});
    }
};

static void error(sh_writer& console, status_t status) {
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

static void command(sh_shell& shell, std::string text) {
    sh_writer console{shell.output()};
    const auto split = text.find(' ');
    const auto name = std::string_view{text}.substr(0, split);
    const auto tail = split == text.npos ? std::string_view{} : std::string_view{text}.substr(split + 1);
    if (name.empty()) return;
    if (name == "help") {
        console.write("PROGRAM [ARGS] [&] | PIPELINE | jobs | status | fg/bg/kill/wait [ID]\n"
            "cd [DIR] | pwd | ls | cat | edit | tasks | touch | mkdir [-p] | rm [-rf] | mv | cp | stat\n"
            "PROGRAM > FILE | PROGRAM >> FILE | svc [start/stop/restart/remove PATH] | mkfs\n"
            "Ctrl+C terminates the foreground job; Ctrl+Z suspends it.\n");
        return;
    }
    if (name == "jobs") { shell.list(console); return; }
    if (name == "cd" || name == "pwd") {
        std::vector<std::string> words;
        std::vector<size_t> cuts{0};
        char target[128]{};
        bool append{}, background{};
        if (!arguments(text.data(), words, cuts, target, append, background) || cuts.size() != 1
            || *target || background || words.size() > (name == "cd" ? 2U : 1U)) {
            error(console, STATUS_BAD_ARGS); return;
        }
        if (name == "pwd") { console.write(shell.cwd().c_str()); console.write("\n"); }
        else if (const auto status = shell.cd(words.size() == 1 ? "/" : std::string_view{words[1]}); status != STATUS_OK)
            error(console, status);
        return;
    }
    if (name == "status") { shell.status(console); return; }
    if (name == "fg" || name == "bg" || name == "kill" || name == "wait") {
        uint64_t id = shell.latest(name == "fg" || name == "bg"), deadline{};
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
        if (const auto status = shell.control(name, id, deadline); status != STATUS_OK) error(console, status);
        return;
    }
    const std::string profile = shell.profile(name) ? std::string{name} : "";
    if (!profile.empty()) text = std::string{tail};
    bool background{};
    std::vector<std::string> words;
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
    if (const auto status = shell.spawn(words, cuts, background, text, profile); status != STATUS_OK) error(console, status);
}

int main() {
    const auto& info = rt_startup();
    sh_shell shell{info};
    shell.prompt();
    for (;;) {
        command(shell, shell.read());
        shell.prompt();
    }
}
