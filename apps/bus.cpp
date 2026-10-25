#include <ubus/ubus.hpp>
#include <libk/parse.hpp>
#include <charconv>

static auto hex(std::string_view text) -> std::expected<std::vector<std::byte>, status_t> {
    if (text.starts_with("0x")) text.remove_prefix(2);
    if (text.size() % 2) return std::unexpected(STATUS_BAD_ARGS);
    std::vector<std::byte> bytes;
    for (size_t i = 0; i < text.size(); i += 2) {
        const auto byte = libk::parse<uint8_t>(text.substr(i, 2), 16);
        if (!byte) return std::unexpected(STATUS_BAD_ARGS);
        bytes.push_back(std::byte{*byte});
    }
    return bytes;
}
static auto hex(std::span<const std::byte> bytes) -> std::string {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) { const auto n = uint8_t(byte); result += digits[n >> 4]; result += digits[n & 15]; }
    return result;
}
static void text(std::string& out, std::string_view value) {
    out += '"';
    for (const auto ch : value) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if (uint8_t(ch) < 32) { constexpr char digits[] = "0123456789abcdef"; out += "\\u00"; out += digits[uint8_t(ch) >> 4]; out += digits[ch & 15]; }
        else out += ch;
    }
    out += '"';
}
static void format(std::string& out, ubus_value value) {
    switch (value.type()) {
    case ubus_type::Null: out += "null"; break;
    case ubus_type::Bool: out += *value.as<bool>() ? "true" : "false"; break;
    case ubus_type::U64: out += std::to_string(*value.as<uint64_t>()); break;
    case ubus_type::I64: out += std::to_string(*value.as<int64_t>()); break;
    case ubus_type::String: text(out, *value.as<std::string_view>()); break;
    case ubus_type::Record: case ubus_type::Array: {
        const bool record = value.type() == ubus_type::Record;
        out += record ? '{' : '['; bool comma{};
        for (const auto child : value.values()) {
            if (std::exchange(comma, true)) out += ',';
            if (record) { text(out, child.name()); out += ':'; }
            format(out, child);
        }
        out += record ? '}' : ']'; break;
    }
    case ubus_type::Cap: {
        uint64_t index; ::memcpy(&index, value.data().data(), sizeof(index));
        out += "{\"$cap\":" + std::to_string(index) + "}"; break;
    }
    case ubus_type::Buffer: {
        const auto buffer = *value.as<ubus_buffer>();
        out += "{\"$buffer\":{\"cap\":" + std::to_string(buffer.cap) + ",\"offset\":" + std::to_string(buffer.offset)
            + ",\"size\":" + std::to_string(buffer.size) + "}}"; break;
    }
    case ubus_type::Vector: {
        ubus_vector header; ::memcpy(&header, value.data().data(), sizeof(header));
        out += "{\"$vector\":{\"atom\":" + std::to_string(uint64_t(header.atom)) + ",\"count\":" + std::to_string(header.count)
            + ",\"data\":\"" + hex(value.data().subspan(sizeof(header))) + "\"}}"; break;
    }
    case ubus_type::F64: case ubus_type::Bytes:
        out += value.type() == ubus_type::F64 ? "{\"$f64\":\"" : "{\"$bytes\":\"";
        out += hex(value.data()); out += "\"}"; break;
    }
}
static auto argument(ubus_msg& request, ubus_value field, std::string_view name, std::string_view value) -> status_t {
    if (value.starts_with('^')) {
        auto bytes = hex(value.substr(1));
        if (!bytes) return bytes.error();
        ubus_data data;
        if (!data.assign(*bytes)) return STATUS_BAD_ARGS;
        request.data.put(name, data); return STATUS_OK;
    }
    const auto type = static_cast<ubus_type>(*field["type"].as<uint64_t>());
    if (type == ubus_type::String) request.data.put(name, value);
    else if (type == ubus_type::Null && value == "null") request.data.put(name, nullptr);
    else if (type == ubus_type::Bool && (value == "true" || value == "false")) request.data.put(name, value == "true");
    else if (type == ubus_type::U64 || type == ubus_type::I64 || type == ubus_type::F64) {
        const bool negative = value.starts_with('-');
        if (negative) value.remove_prefix(1);
        const bool bits = value.starts_with("0x"); if (bits) value.remove_prefix(2);
        const auto number = libk::parse<uint64_t>(value, bits ? 16 : 10);
        if (!number) return STATUS_BAD_ARGS;
        if (type == ubus_type::F64) {
            if (!bits || negative) return STATUS_BAD_ARGS;
            request.data.put(name, std::bit_cast<double>(*number));
        } else if (type == ubus_type::I64) {
            if (*number > (uint64_t{1} << 63) - uint64_t(!negative)) return STATUS_BAD_ARGS;
            request.data.put(name, std::bit_cast<int64_t>(negative ? uint64_t{0} - *number : *number));
        } else {
            if (negative) return STATUS_BAD_ARGS;
            request.data.put(name, *number);
        }
    } else if (type == ubus_type::Bytes) {
        auto bytes = hex(value); if (!bytes) return bytes.error();
        request.data.put(name, *bytes);
    } else return STATUS_BAD_ARGS;
    return STATUS_OK;
}
int main(int argc, char** argv) {
    ubus_conn directory, target;
    auto status = directory.bind(_startup().bootstrap);
    if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = directory.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    const auto output = setup.capability(setup.data["message"]["stdout"]);
    const auto write = [output](std::string_view text) {
        while (!text.empty()) {
            const auto count = std::min(text.size(), size_t{80});
            ubus_msg message; message.data.put("data", std::as_bytes(std::span{text.data(), count}));
            const auto status = ubus_send(output, message);
            if (status != STATUS_OK) return status;
            text.remove_prefix(count);
        }
        return STATUS_OK;
    };
    const auto print = [&](ubus_value value) { std::string result; format(result, value); result += '\n'; (void)write(result); };
    if (argc < 2) {
        (void)write("bus list | wait NAME | describe NAME | call NAME METHOD [FIELD=VALUE ...]\n"
            "--value HEX: public ubus Record; FIELD=^HEX: any public Value\n"
            "--cap KEY / --move KEY: attach a received capability; F64=0xBITS, Bytes=HEX\n");
        return STATUS_OK;
    }
    const std::string_view command{argv[1]};
    if (command == "list") {
        ubus_data cursor;
        for (;;) {
            ubus_msg request, response;
            if (cursor["epoch"]) request.data.put("cursor", cursor);
            status = directory.invoke("List", std::move(request), response);
            if (status != STATUS_OK) return status;
            print(response.data["items"]);
            if (!response.data["cursor"]) return STATUS_OK;
            if (!cursor.assign(response.data["cursor"].bytes())) return STATUS_PEER_FAULT;
        }
    }
    if (argc < 3) return STATUS_BAD_ARGS;
    const std::string_view name{argv[2]};
    if (command == "wait") {
        ubus_msg request, response; request.data.put("name", name);
        return directory.invoke("Get", std::move(request), response);
    }
    ubus_conn* connection = &directory;
    if (name != ".") {
        if (name.starts_with('@')) {
            status = target.bind(setup.capability(setup.data["message"][name.substr(1)]));
        } else {
            ubus_msg request, response; request.data.put("name", name);
            status = directory.invoke("Get", std::move(request), response);
            if (status == STATUS_OK) status = target.bind(response.capability(response.data["connector"]));
        }
        if (status != STATUS_OK) return status;
        connection = &target;
    }
    if (command == "describe") { print(connection->description()); return STATUS_OK; }
    if (command != "call" || argc < 4) return STATUS_BAD_ARGS;
    ubus_value method;
    for (const auto item : connection->description()["methods"].values()) if (item["name"].as<std::string_view>() == argv[3]) method = item;
    if (!method) return STATUS_INVALID_OP;
    ubus_msg request, response;
    for (int i = 4; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--value" && i + 1 < argc) {
            auto bytes = hex(argv[++i]); if (!bytes) return bytes.error();
            if (!request.data.assign(*bytes)) return STATUS_BAD_ARGS;
        } else if ((arg == "--cap" || arg == "--move") && i + 1 < argc) {
            const auto value = setup.data["message"][std::string_view{argv[++i]}];
            const auto input = setup.capability(value);
            if (!input) return STATUS_INVALID_CAP;
            if (arg == "--move") { if (!request.give(setup.take(value))) return STATUS_NO_MEMORY; }
            else {
                const auto info = _cap_copy<CapInfo>(_cap_info(input)); if (!info) return info.error();
                if (!request.attach(input, info->view.rights)) return STATUS_NO_MEMORY;
            }
        } else {
            const auto split = arg.find('=');
            if (split == arg.npos) return STATUS_BAD_ARGS;
            const auto key = arg.substr(0, split);
            ubus_value field;
            for (const auto item : method["input"].values()) if (item["name"].as<std::string_view>() == key) field = item;
            if (!field) return STATUS_BAD_ARGS;
            status = argument(request, field, key, arg.substr(split + 1));
            if (status != STATUS_OK) return status;
        }
    }
    status = connection->invoke(argv[3], std::move(request), response);
    if (status == STATUS_OK) print(response.data.view());
    return status;
}
