#include <posix.hpp>
#include <ubus/ubus.hpp>
#include <libk/fmt.hpp>
#include <libk/parse.hpp>
#include <unistd.h>
#include <charconv>

struct swap_output { void write(const char* p, size_t n) { (void)::write(STDOUT_FILENO, p, n); } };
int main(int argc, char** argv) {
    if (const auto s = px_init(); s != STATUS_OK) return s;
    if (argc < 2) { constexpr auto help = "swap status | format/on VOLUME [--offset BYTES] [--bytes BYTES] [--limit BYTES] [--priority N] | pause/off [VOLUME] | set NAME VALUE [SESSION]\n"; (void)::write(1, help, ::strlen(help)); return STATUS_BAD_ARGS; }
    ubus_conn environment, admin;
    auto status = environment.bind(_startup().bootstrap); if (status != STATUS_OK) return status;
    ubus_msg request, response; request.data.put("name", "memory.admin");
    status = environment.invoke("Get", std::move(request), response); if (status != STATUS_OK) return status;
    status = admin.bind(response.capability(response.data["connector"])); if (status != STATUS_OK) return status;
    const std::string_view action{argv[1]}; std::string_view method;
    request = {};
    if (action == "status") { if (argc != 2) return STATUS_BAD_ARGS;
        method = "Status"; }
    else if (action == "format" || action == "on") {
        if (argc < 3 || argc % 2 != 1) return STATUS_BAD_ARGS;
        method = action == "format" ? "Format" : "Enable";
        request.data.put("volume", std::string_view{argv[2]});
        for (int i = 3; i < argc; i += 2) {
            std::string_view name{argv[i]}; if (!name.starts_with("--")) return STATUS_BAD_ARGS; name.remove_prefix(2);
            if (name == "priority") { int64_t n{}; const std::string_view text{argv[i+1]}; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), n);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return STATUS_BAD_ARGS;
                request.data.put(name, n); }
            else { if (name != "offset" && name != "bytes" && name != "limit") return STATUS_BAD_ARGS;
                const auto n = libk::parse<uint64_t>(argv[i+1]); if (!n) return STATUS_BAD_ARGS; request.data.put(name, *n); }
        }
    } else if (action == "pause" || action == "off") {
        if (argc > 3) return STATUS_BAD_ARGS;
        method = action == "pause" ? "Pause" : "Disable";
        if (argc == 3) request.data.put("volume", std::string_view{argv[2]});
    } else if (action == "set") {
        if (argc != 4 && argc != 5) return STATUS_BAD_ARGS;
        method = "Configure";
        const auto n = libk::parse<uint64_t>(argv[3]); if (!n) return STATUS_BAD_ARGS;
        request.data.put("name", std::string_view{argv[2]}).put("value", *n);
        if (argc == 5) { const auto id = libk::parse<uint64_t>(argv[4]); if (!id) return STATUS_BAD_ARGS; request.data.put("session", *id); }
    } else return STATUS_BAD_ARGS;
    status = admin.invoke(method, std::move(request), response); if (status != STATUS_OK) return status;
    if (const auto id = response.data["operation"].as<uint64_t>(); id && action != "status") {
        for (;;) {
            request = {}; request.data.put("operation", *id);
            status = admin.invoke("Query", std::move(request), response); if (status != STATUS_OK) return status;
            const auto result = response.data["result"].as<int64_t>().value_or(STATUS_PEER_FAULT);
            if (result != STATUS_PENDING) { status = static_cast<status_t>(result); break; }
            const auto due = _after(std::chrono::milliseconds{10}); if (!due) return STATUS_INTERNAL;
            const auto wake = _ev_wait(&_events(), *due).status; if (wake != STATUS_OK && wake != STATUS_TIMED_OUT) return wake;
        }
    }
    swap_output output;
    if (action == "status") {
        (void)libk::fmt::format_to<"free={} reserve={} reads={} writes={} released={} refaults={} scanned={} copied={} failures={} pool={}\n">(output,
            response.data["free"].as<uint64_t>().value_or(0), response.data["reserve"].as<uint64_t>().value_or(0),
            response.data["reads"].as<uint64_t>().value_or(0), response.data["writes"].as<uint64_t>().value_or(0),
            response.data["released"].as<uint64_t>().value_or(0), response.data["refaults"].as<uint64_t>().value_or(0),
            response.data["scanned"].as<uint64_t>().value_or(0), response.data["copied"].as<uint64_t>().value_or(0),
            response.data["failures"].as<uint64_t>().value_or(0), response.data["pool-bytes"].as<uint64_t>().value_or(0));
        (void)libk::fmt::format_to<"serving={} operation={} result={} waiting={}/{} io={}/{} charged={} index~={}\n">(output,
            response.data["serving"].as<bool>().value_or(false), response.data["operation"].as<uint64_t>().value_or(0),
            response.data["result"].as<int64_t>().value_or(STATUS_PEER_FAULT), response.data["waiting-pages"].as<uint64_t>().value_or(0),
            response.data["waiting-capacity"].as<uint64_t>().value_or(0), response.data["inflight-reads"].as<uint64_t>().value_or(0),
            response.data["inflight-writes"].as<uint64_t>().value_or(0), response.data["charged-bytes"].as<uint64_t>().value_or(0),
            response.data["index-bytes-estimate"].as<uint64_t>().value_or(0));
        for (const auto v : response.data["volumes"].values())
            (void)libk::fmt::format_to<"{} {} used={}/{} priority={} error={}\n">(output, v["name"].as<std::string_view>().value_or(""), v["state"].as<std::string_view>().value_or(""),
                v["used"].as<uint64_t>().value_or(0), v["bytes"].as<uint64_t>().value_or(0), v["priority"].as<int64_t>().value_or(0), v["error"].as<int64_t>().value_or(0));
        for (const auto s : response.data["sessions"].values())
            (void)libk::fmt::format_to<"session={} logical={} limit={} resident~={} target={} swap={}\n">(output, s["id"].as<uint64_t>().value_or(0), s["logical"].as<uint64_t>().value_or(0),
                s["limit"].as<uint64_t>().value_or(0), s["resident"].as<uint64_t>().value_or(0), s["target"].as<uint64_t>().value_or(0), s["swap"].as<uint64_t>().value_or(0));
    } else (void)libk::fmt::format_to<"swap {}: {}\n">(output, action, int64_t{status});
    return status;
}
