#include <ubus/ubus.hpp>
#include <libk/fmt.hpp>

struct svc_output {
    cap_t channel{};
    void write(const char* bytes, size_t size) const {
        while (size) {
            const auto count = std::min(size, size_t{80});
            ubus_msg message; message.data.put("data", std::as_bytes(std::span{bytes, count}));
            if (const auto status = ubus_send(channel, message); status != STATUS_OK) sys::exit(status);
            bytes += count; size -= count;
        }
    }
};
int main(int argc, char** argv) {
    ubus_conn environment, owner;
    auto status = environment.bind(rt_startup().environment); if (status != STATUS_OK) return status;
    ubus_msg setup; status = environment.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    svc_output output{setup.capability(setup.data["message"]["stdout"])};
    auto root = setup.take(setup.data["message"]["scope"]); if (!root) return STATUS_DENIED;
    status = owner.bind(root.selector()); if (status != STATUS_OK) return status;
    const auto list = [&](auto&& self, ubus_conn& connection, std::string path, size_t depth) -> status_t {
        if (depth > 16) return STATUS_EXHAUSTED;
        ubus_msg response; auto status = connection.invoke("List", {}, response); if (status != STATUS_OK) return status;
        for (const auto row : response.data["items"].values()) {
            const auto name = row["name"].as<std::string_view>().value_or("");
            const auto full = path.empty() ? std::string{name} : path + "/" + std::string{name};
            (void)libk::fmt::format_to<"{} {}\n">(output, full, row["state"].as<std::string_view>().value_or("unknown"));
            if (row["children"].as<bool>().value_or(false)) {
                ubus_msg request, nested;
                request.data.put("id", row["id"]).put("gen", row["gen"]).put("facet", "children");
                status = connection.invoke("Inspect", std::move(request), nested); if (status != STATUS_OK) return status;
                ubus_conn child; status = child.bind(nested.capability(nested.data["connector"])); if (status != STATUS_OK) return status;
                status = self(self, child, full, depth + 1); if (status != STATUS_OK) return status;
            }
        }
        return STATUS_OK;
    };
    if (argc == 1) return list(list, owner, {}, 0);
    if (argc < 3) return STATUS_BAD_ARGS;
    const std::string_view verb{argv[1]};
    const bool spawn = verb == "spawn";
    if (spawn ? argc < 4 : argc != 3) return STATUS_BAD_ARGS;
    if (!spawn && verb != "start" && verb != "stop" && verb != "restart" && verb != "remove") return STATUS_BAD_ARGS;
    std::string_view path{argv[2]};
    while (path.starts_with('/')) path.remove_prefix(1);
    for (size_t depth = 0; depth < 16; ++depth) {
        const auto slash = path.find('/'); const auto name = path.substr(0, slash);
        ubus_msg request, response;
        if (spawn && slash == path.npos) {
            ubus_data args{ubus_type::Array}; for (int i = 4; i < argc; ++i) args.push(std::string_view{argv[i]});
            request.data.put("name", name).put("program", std::string_view{argv[3]}).put("argv", args);
            status = owner.invoke("Spawn", std::move(request), response);
            if (status == STATUS_OK) { constexpr std::string_view text{"deployment accepted\n"}; output.write(text.data(), text.size()); }
            return status;
        }
        request.data.put("name", name);
        status = owner.invoke("Lookup", std::move(request), response); if (status != STATUS_OK) return status;
        auto cap = response.take(response.data["connector"]);
        ubus_conn target; status = target.bind(cap.selector()); if (status != STATUS_OK) return status;
        request = {}; request.data.put("facet", "overview");
        status = target.invoke("Inspect", std::move(request), response); if (status != STATUS_OK) return status;
        const auto gen = response.data["items"][size_t{0}]["gen"].as<uint64_t>(); if (!gen) return STATUS_PEER_FAULT;
        if (slash == path.npos) {
            request = {}; request.data.put("action", verb).put("gen", *gen).put("version", response.data["items"][size_t{0}]["config"]);
            status = target.invoke("Action", std::move(request), response);
            if (status == STATUS_OK) { constexpr std::string_view text{"service change requested\n"}; output.write(text.data(), text.size()); }
            return status;
        }
        request = {}; request.data.put("facet", "children").put("gen", *gen);
        status = target.invoke("Inspect", std::move(request), response); if (status != STATUS_OK) return status;
        cap = response.take(response.data["connector"]);
        status = owner.close(); if (status == STATUS_OK) status = owner.bind(cap.selector()); if (status != STATUS_OK) return status;
        root = std::move(cap); path.remove_prefix(slash + 1);
    }
    return STATUS_EXHAUSTED;
}
