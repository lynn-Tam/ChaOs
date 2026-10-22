#include "value.hpp"
#include <algorithm>

ubus_value::ubus_value(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(head_)) return;
    ::memcpy(&head_, bytes.data(), sizeof(head_));
    if (head_.bytes < sizeof(head_) + ubus_align(head_.name) || head_.bytes > bytes.size()
        || head_.type > ubus_type::Vector) return;
    bytes_ = bytes.first(head_.bytes);
}
auto ubus_value::operator[](std::string_view key) const noexcept -> ubus_value {
    for (const auto value : values()) if (value.name() == key) return value;
    return {};
}
auto ubus_value::operator[](size_t index) const noexcept -> ubus_value {
    for (const auto value : values()) if (!index--) return value;
    return {};
}
auto ubus_value::valid(size_t depth) const noexcept -> bool {
    if (!*this || !depth) return false;
    const auto payload = data();
    switch (type()) {
    case ubus_type::Null: return payload.empty();
    case ubus_type::Bool: return as<bool>().has_value();
    case ubus_type::I64: case ubus_type::U64: case ubus_type::F64: case ubus_type::Cap:
        return payload.size() == 8;
    case ubus_type::Buffer: {
        const auto slice = as<ubus_buffer>();
        return slice && slice->offset <= UINT64_MAX - slice->size;
    }
    case ubus_type::String: case ubus_type::Bytes: return true;
    case ubus_type::Vector: {
        if (payload.size() < sizeof(ubus_vector)) return false;
        ubus_vector head; ::memcpy(&head, payload.data(), sizeof(head));
        if (head.reserved || head.atom > ubus_atom::F64) return false;
        const auto atom = static_cast<size_t>(head.atom);
        const size_t size = atom < 8 ? size_t{1} << (atom / 2) : atom == 8 ? 4 : 8;
        return payload.size() - sizeof(head) == head.count * size;
    }
    case ubus_type::Array: case ubus_type::Record: {
        std::optional<std::string_view> previous;
        for (auto bytes = payload; !bytes.empty();) {
            const ubus_value child{bytes};
            if (!child.valid(depth - 1) || ubus_align(child.bytes().size()) > bytes.size()) return false;
            if (type() == ubus_type::Record) {
                if (previous && *previous >= child.name()) return false;
                previous = child.name();
            } else if (!child.name().empty()) return false;
            bytes = bytes.subspan(ubus_align(child.bytes().size()));
        }
        return true;
    }
    }
    return false;
}
auto ubus_range::iterator::operator*() const noexcept -> ubus_value { return ubus_value{bytes}; }
auto ubus_range::iterator::operator++() noexcept -> iterator& {
    const ubus_value value{bytes};
    const auto size = ubus_align(value.bytes().size());
    bytes = size && size <= bytes.size() ? bytes.subspan(size) : std::span<const std::byte>{};
    return *this;
}
ubus_data::ubus_data(ubus_type type) {
    if (type == ubus_type::Record) return;
    bytes_.resize(sizeof(ubus_head));
    const ubus_head head{sizeof(ubus_head), type, 0};
    ::memcpy(bytes_.data(), &head, sizeof(head));
}
static auto ubus_overlap(std::span<const std::byte> a, std::span<const std::byte> b) -> bool {
    const auto x = reinterpret_cast<uintptr_t>(a.data()), y = reinterpret_cast<uintptr_t>(b.data());
    return !a.empty() && !b.empty() && (x >= y ? x - y < b.size() : y - x < a.size());
}
void ubus_data::append(std::string_view name, ubus_type type, std::span<const std::byte> data,
                       std::span<const std::byte> extra) {
    if (bytes_.empty()) {
        const auto empty = std::as_bytes(std::span{&empty_, size_t{1}});
        bytes_.assign(empty.begin(), empty.end());
    }
    const auto prefix = sizeof(ubus_head) + ubus_align(name.size()) + extra.size();
    if (failed_ || name.size() > UINT16_MAX || prefix > UINT32_MAX || data.size() > UINT32_MAX - prefix
        || bytes_.size() > UINT32_MAX - ubus_align(prefix + data.size())) { failed_ = true; return; }
    size_t offset = sizeof(ubus_head) + ubus_align(view().name().size());
    if (view().type() == ubus_type::Record) {
        for (const auto child : view().values()) {
            if (name == child.name()) { failed_ = true; return; }
            if (name < child.name()) break;
            offset += ubus_align(child.bytes().size());
        }
    } else if (view().type() == ubus_type::Array && name.empty()) offset = bytes_.size();
    else { failed_ = true; return; }
    const ubus_head head{static_cast<uint32_t>(prefix + data.size()), type, static_cast<uint16_t>(name.size())};
    const auto oldsize = bytes_.size(), amount = ubus_align(head.bytes);
    // Insertion can overwrite borrowed input even when capacity does not grow.
    std::span<const std::byte> inputs[]{std::as_bytes(std::span{name}), data, extra};
    size_t saved_size{};
    for (const auto input : inputs) if (ubus_overlap(input, bytes_)) saved_size += input.size();
    std::vector<std::byte> saved(saved_size);
    size_t cursor{};
    for (auto& input : inputs) if (ubus_overlap(input, bytes_)) {
        ::memcpy(saved.data() + cursor, input.data(), input.size());
        input = std::span{saved}.subspan(cursor, input.size()); cursor += input.size();
    }
    name = {reinterpret_cast<const char*>(inputs[0].data()), inputs[0].size()};
    data = inputs[1]; extra = inputs[2];
    bytes_.resize(oldsize + amount);
    ::memmove(bytes_.data() + offset + amount, bytes_.data() + offset, oldsize - offset);
    ::memset(bytes_.data() + offset, 0, amount);
    ::memcpy(bytes_.data() + offset, &head, sizeof(head));
    if (!name.empty()) ::memcpy(bytes_.data() + offset + sizeof(head), name.data(), name.size());
    if (!extra.empty()) ::memcpy(bytes_.data() + offset + prefix - extra.size(), extra.data(), extra.size());
    if (!data.empty()) ::memcpy(bytes_.data() + offset + prefix, data.data(), data.size());
    const auto size = static_cast<uint32_t>(bytes_.size());
    ::memcpy(bytes_.data(), &size, sizeof(size));
}
auto ubus_data::assign(std::span<const std::byte> bytes) -> bool {
    const ubus_value value{bytes};
    if (!value.valid() || value.bytes().size() != bytes.size()) { failed_ = true; return false; }
    if (ubus_overlap(bytes, bytes_)) {
        ::memmove(bytes_.data(), bytes.data(), bytes.size()); bytes_.resize(bytes.size());
    } else bytes_.assign(bytes.begin(), bytes.end());
    failed_ = false; return true;
}
