#include <ubus/value.hpp>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>

static void check(bool value, std::source_location at = std::source_location::current()) {
    if (value) return;
    std::cerr << "Value regression failed: " << at.line() << '\n'; std::abort();
}
static void text(const ubus_data& data, std::string_view key, std::string_view expected) {
    check(data.valid()); check(data[key].as<std::string_view>() == expected);
}
int main() {
    const std::string large(6000, 'x');
    ubus_data data;
    data.put("a", large); data.put("b", data["a"]);
    text(data, "a", large); text(data, "b", large);

    data = {}; data.reserve(32768);
    data.put("z", large); const auto* address = data.bytes().data();
    data.put("a", data["z"]);
    check(data.bytes().data() == address);
    text(data, "a", large); text(data, "z", large);

    data = {}; data.put("source", "new");
    data.put(*data["source"].as<std::string_view>(), data["source"]);
    text(data, "new", "new"); text(data, "source", "new");
    data.put("copy", *data["source"].as<std::string_view>());
    text(data, "copy", "new");
    data.put("blob", data["source"].data());
    data.put("blob2", *data["blob"].as<std::span<const std::byte>>());
    check(data.valid()); check(data["blob2"].data().size() == 3);
    check(::memcmp(data["blob2"].data().data(), "new", 3) == 0);

    ubus_data array{ubus_type::Array}; array.push(large); array.push(array.view()[0]);
    check(array.valid()); check(array.view()[1].as<std::string_view>() == large);
    ubus_data nested; nested.put("item", large); nested.put("nested", nested);
    text(nested, "item", large); check(nested["nested"]["item"].as<std::string_view>() == large);
    check(nested.assign(nested.bytes())); text(nested, "item", large);
    check(nested.assign(nested["nested"].bytes())); text(nested, "item", large);
    nested.put("after", uint64_t{42});
    text(nested, "item", large); check(nested["after"].as<uint64_t>() == 42);

    const uint64_t numbers[]{1, 7, 42};
    data = {}; data.array("z", std::span{numbers});
    data.array("a", *data["z"].vector<uint64_t>());
    check(data.valid()); check((*data["a"].vector<uint64_t>())[2] == 42);
    std::cout << "[value] alias snapshots, insertion, arrays and assign: OK\n";
}
