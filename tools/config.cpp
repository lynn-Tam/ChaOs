#include <ubus/value.hpp>
#include <uapi/cap.h>
#include <json-c/json.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>

static auto constant(std::string_view text) -> std::optional<uint64_t> {
#define FLAG(name) std::pair<std::string_view, uint64_t>{#name, name}
    static constexpr std::array flags{
        FLAG(RIGHT_DUPLICATE), FLAG(RIGHT_DELEGATE), FLAG(RIGHT_RESERVE), FLAG(RIGHT_MAP),
        FLAG(RIGHT_UNMAP), FLAG(RIGHT_PROTECT), FLAG(RIGHT_DESTROY), FLAG(RIGHT_INSPECT),
        FLAG(RIGHT_CONTROL), FLAG(RIGHT_MANAGE), FLAG(RIGHT_REVOKE), FLAG(RIGHT_CREATE),
        FLAG(RIGHT_SPLIT), FLAG(RIGHT_CLOSE), FLAG(RIGHT_SIGNAL), FLAG(RIGHT_RECEIVE),
        FLAG(RIGHT_CONNECT), FLAG(RIGHT_ACK), FLAG(RIGHT_SEND), FLAG(RIGHT_SERVE),
        FLAG(RIGHT_SUPPLY), FLAG(RIGHT_FAIL), FLAG(RIGHT_WRITEBACK_ACK), FLAG(RIGHT_ROUTE),
        FLAG(RIGHT_OBSERVE), FLAG(RIGHT_ATTACH), FLAG(RIGHT_ADOPT), FLAG(RIGHT_REBILL), FLAG(OBJ_KINDS)};
#undef FLAG
    uint64_t result{};
    do {
        const auto end = text.find('|');
        auto part = text.substr(0, end);
        while (part.starts_with(' ')) part.remove_prefix(1);
        while (part.ends_with(' ')) part.remove_suffix(1);
        const auto flag = std::ranges::find(flags, part, &decltype(flags)::value_type::first);
        if (flag == flags.end()) return {};
        result |= flag->second;
        if (end == text.npos) return result;
        text.remove_prefix(end + 1);
    } while (!text.empty());
    return {};
}
static auto append(ubus_data& output, std::string_view name, json_object* value, size_t depth = ubus_depth) -> bool {
    if (!depth) return false;
    switch (json_object_get_type(value)) {
    case json_type_null: output.put(name, nullptr); break;
    case json_type_boolean: output.put(name, bool(json_object_get_boolean(value))); break;
    case json_type_int:
        if (*json_object_get_string(value) == '-') output.put(name, json_object_get_int64(value));
        else output.put(name, json_object_get_uint64(value));
        break;
    case json_type_double: output.put(name, json_object_get_double(value)); break;
    case json_type_string: output.put(name, std::string_view{json_object_get_string(value), size_t(json_object_get_string_len(value))}); break;
    case json_type_object: {
        json_object* symbol{};
        if (json_object_object_length(value) == 1 && json_object_object_get_ex(value, "$const", &symbol)) {
            if (json_object_get_type(symbol) != json_type_string) return false;
            const auto bits = constant(json_object_get_string(symbol)); if (!bits) return false;
            output.put(name, *bits); break;
        }
        ubus_data object;
        json_object_iter iterator;
        json_object_object_foreachC(value, iterator)
            if (!append(object, iterator.key, iterator.val, depth - 1)) return false;
        output.put(name, object); break;
    }
    case json_type_array: {
        ubus_data array{ubus_type::Array};
        for (size_t i = 0; i < json_object_array_length(value); ++i)
            if (!append(array, {}, json_object_array_get_idx(value, i), depth - 1)) return false;
        output.put(name, array); break;
    }
    }
    return true;
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: config INPUT.json OUTPUT\n"; return 1; }
    std::unique_ptr<json_object, decltype(&json_object_put)> source{json_object_from_file(argv[1]), json_object_put};
    if (!source || json_object_get_type(source.get()) != json_type_object) {
        std::cerr << "expected a JSON configuration object: " << json_util_get_last_err() << '\n'; return 1;
    }
    ubus_data wrapper;
    if (!append(wrapper, "config", source.get()) || !wrapper.valid()) { std::cerr << "invalid configuration value\n"; return 1; }
    const auto bytes = wrapper["config"].bytes();
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return out ? 0 : 1;
}
