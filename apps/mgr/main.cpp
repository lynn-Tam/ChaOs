#include <libk/cpio.hpp>
#include <mgr/mgr.hpp>
#include <map>

using mgr_context = std::map<std::string, ubus_msg, std::less<>>;

// Package references are resolved here, with real capabilities, before handing
// the ordinary recipe to the mechanism. They never become startup ABI keys.
static auto append(ubus_data& data, std::string_view name, ubus_value value, ubus_msg& out,
                   const ubus_msg* source, const mgr_context& context, size_t depth = ubus_depth, bool resolve = true) -> status_t {
    if (!depth) return STATUS_BAD_ARGS;
    if (const auto literal = value["$literal"]; resolve && literal) { data.put(name, literal); return STATUS_OK; }
    if (const auto ref = value["$ref"].as<std::string_view>(); resolve && ref) {
        const auto item = context.find(*ref);
        if (item == context.end()) return STATUS_NOT_FOUND;
        const auto leaf = item->second.data["value"];
        if (leaf.type() == ubus_type::Cap && value["rights"]) {
            const auto cap = item->second.capability(leaf);
            const auto rights = value["rights"].as<uint64_t>();
            if (!rights) return STATUS_BAD_ARGS;
            const auto badge = value["badge"].as<uint64_t>().value_or(0);
            const auto granted = badge ? _channel_mint(cap, _startup().cspace, badge, *rights)
                : _cap_duplicate(cap, _startup().cspace, *rights);
            if (granted.status != STATUS_OK) return granted.status;
            const auto index = out.offer(_cap_owner{granted.value}, *rights); if (!index) return STATUS_NO_MEMORY;
            data.cap(name, *index); return STATUS_OK;
        }
        return append(data, name, leaf, out, &item->second, context, depth - 1, resolve);
    }
    if (value.type() == ubus_type::Cap || value.type() == ubus_type::Buffer) {
        if (!source) return STATUS_BAD_ARGS;
        const auto cap = source->capability(value);
        const auto info = _cap_copy<CapInfo>(_cap_info(cap)); if (!info) return info.error();
        std::expected<size_t, status_t> index = std::unexpected(STATUS_NOT_FOUND);
        for (size_t i = 0; i < out.count; ++i)
            if (out.transfers[i].source == cap && out.transfers[i].rights == info->view.rights
                && out.transfers[i].operation == CAP_COPY) { index = i; break; }
        if (!index) index = out.attach(cap, info->view.rights);
        if (!index) return index.error();
        if (const auto buffer = value.as<ubus_buffer>()) data.put(name, ubus_buffer{*index, buffer->offset, buffer->size});
        else data.cap(name, *index);
    } else if (value.type() == ubus_type::Record || value.type() == ubus_type::Array) {
        ubus_data nested{value.type()};
        for (const auto field : value.values()) {
            const auto status = append(nested, field.name(), field, out, source, context, depth - 1, resolve);
            if (status != STATUS_OK) return status;
        }
        data.put(name, nested);
    } else data.put(name, value);
    return STATUS_OK;
}
int main() {
    const auto& env = _startup();
    ubus_conn parent;
    auto status = parent.bind(env.bootstrap); if (status != STATUS_OK) return status;
    ubus_msg received;
    status = parent.invoke("Receive", {}, received); if (status != STATUS_OK) return status;
    mgr_context context;
    for (const auto field : received.data["message"].values()) {
        ubus_msg value;
        status = append(value.data, "value", field, value, &received, context, ubus_depth, false);
        if (status != STATUS_OK) return status;
        auto owned = value.clone(); if (!owned) return owned.error();
        context.emplace(std::string{field.name()}, std::move(*owned));
    }
    _vm_area archive_memory;
    libk::cpio_view archive;
    const auto archive_image = [&](std::string_view name) -> std::expected<_proc_image, status_t> {
        while (name.starts_with('/')) name.remove_prefix(1);
        const auto file = archive.find(name);
        if (!file) return std::unexpected(STATUS_NOT_FOUND);
        return _proc_image{.bytes=file->data};
    };
    if (const auto found = context.find("archive"); found != context.end()) {
        const auto buffer = found->second.data["value"].as<ubus_buffer>();
        if (!buffer || buffer->offset) return STATUS_BAD_ARGS;
        auto mapping = _vm_area::map(_vm(), found->second.capability(found->second.data["value"]), 0, (buffer->size + 4095) & ~size_t{4095}, VM_READ);
        if (!mapping) return mapping.error();
        archive_memory = std::move(*mapping);
        archive = libk::cpio_view::parse({reinterpret_cast<const std::byte*>(archive_memory.address), buffer->size});
        if (!archive) return STATUS_BAD_ARGS;
    }
    ubus_data config;
    const ubus_msg* config_source{};
    if (const auto found = context.find("config"); found != context.end()) {
        config_source = &found->second;
        if (!config.assign(found->second.data["value"].bytes())) return STATUS_BAD_ARGS;
    } else {
        const auto file = archive.find("mgr.config");
        if (!file || !config.assign(file->data)) return STATUS_BAD_ARGS;
    }
    struct loading {
        enum phase { LocalBind, LocalGet, ParentBind, ParentGet, FilesBind, Map } step{};
        ubus_conn route, files;
        _cap_owner connector;
        bool started{};
    };
    mgr* self{};
    mgr owner{[&](mgr_child& child) -> std::expected<_proc_image, status_t> {
        if (auto image = archive_image(child.program())) return image;
        auto state = std::make_shared<loading>();
        auto connector = self->object().connector(mgr_observe | mgr_control);
        if (!connector) return std::unexpected(connector.error());
        state->connector = std::move(*connector);
        child.loading = [&, state]() -> std::expected<_proc_image, status_t> {
            auto& load = *state;
            const auto& program = child.program();
            const auto fail = [](status_t status) -> std::expected<_proc_image, status_t> { return std::unexpected(status); };
            for (;;) {
                const bool binding = load.step == loading::LocalBind || load.step == loading::ParentBind || load.step == loading::FilesBind;
                auto& connection = load.step >= loading::FilesBind ? load.files : load.route;
                if (!load.started) {
                    status_t status;
                    if (binding) status = connection.begin_bind(load.step == loading::ParentBind ? env.bootstrap : load.connector.selector());
                    else {
                        ubus_msg request;
                        if (load.step == loading::Map) request.data.put("path", program).put("perms", word_t{VM_READ | VM_EXECUTE});
                        else request.data.put("name", "exec");
                        status = connection.begin(load.step == loading::Map ? "Map" : "Get", std::move(request));
                    }
                    if (status != STATUS_OK) return fail(status);
                    load.started = true;
                }
                ubus_msg response;
                auto status = binding ? connection.poll_bind() : connection.poll(response);
                if (status != STATUS_OK) return fail(status);
                if (!binding) status = response.status;
                load.started = false;
                if (load.step == loading::LocalGet && status == STATUS_NOT_FOUND) {
                    load.step = loading::ParentBind; continue;
                }
                if (status != STATUS_OK) return fail(status);
                if (load.step == loading::LocalBind) load.step = loading::LocalGet;
                else if (load.step == loading::ParentBind) load.step = loading::ParentGet;
                else if (load.step == loading::LocalGet || load.step == loading::ParentGet) {
                    load.connector = response.take(response.data["connector"]); load.step = loading::FilesBind;
                } else if (load.step == loading::FilesBind) load.step = loading::Map;
                else {
                    const auto buffer = response.data["buffer"].as<ubus_buffer>(); if (!buffer) return fail(STATUS_PEER_FAULT);
                    return _proc_image::map(child.source, response.take(response.data["buffer"]),
                        buffer->offset, buffer->size, VM_READ | VM_EXECUTE);
                }
            }
        };
        return child.loading();
    }};
    self = &owner;
    status = owner.open(); if (status != STATUS_OK) return status;
    std::deque<std::unique_ptr<_ch_pair>> channels;
    for (const auto object : config["objects"].values()) {
        const auto kind = object["kind"].as<std::string_view>(); if (!kind) return STATUS_BAD_ARGS;
        if (*kind == "Manager") {
            const auto rights = object["rights"].as<uint64_t>();
            if (!rights || !*rights || (*rights & ~(mgr_observe | mgr_control | mgr_create | mgr_debug))) return STATUS_BAD_ARGS;
            auto cap = owner.connector(*rights); if (!cap) return cap.error();
            ubus_msg value;
            const auto cap_idx = value.offer(std::move(*cap), RIGHT_SEND | RIGHT_DUPLICATE);
            if (!cap_idx) return cap_idx.error();
            value.data.cap("value", *cap_idx);
            context.emplace(std::string{object.name()}, std::move(value));
        } else if (*kind == "Channel") {
            auto pair = std::make_unique<_ch_pair>();
            status = pair->open(env.alloc(), object["depth"].as<uint64_t>().value_or(16), object["caps"].as<uint64_t>().value_or(0));
            if (status != STATUS_OK) return status;
            for (size_t side = 0; side < 2; ++side) {
                ubus_msg value;
                const auto cap_idx = value.attach(pair->ends[side].selector(), ~uint64_t{});
                if (!cap_idx) return cap_idx.error();
                value.data.cap("value", *cap_idx);
                const auto info = _cap_copy<CapInfo>(_cap_info(pair->ends[side].selector())); if (!info) return info.error();
                value.transfers[0].rights = info->view.rights;
                auto owned = value.clone(); if (!owned) return owned.error();
                context.emplace(std::string{object.name()} + "." + std::to_string(side), std::move(*owned));
            }
            channels.push_back(std::move(pair));
        } else {
            const auto made = *kind == "Quota" ? _quota_create(env.alloc(),
                object["bytes"].as<uint64_t>().value_or(0), object["caps"].as<uint64_t>().value_or(0),
                object["kinds"].as<uint64_t>().value_or(OBJ_KINDS)) : *kind == "Scope" ? _scope_create(env.alloc()) : _sys_result{.status=STATUS_BAD_ARGS};
            if (made.status != STATUS_OK) return made.status;
            ubus_msg value;
            const auto cap_idx = value.give(_cap_owner{made.value});
            if (!cap_idx) return cap_idx.error();
            value.data.cap("value", *cap_idx);
            context.emplace(std::string{object.name()}, std::move(value));
        }
    }
    const auto recipe_for = [&](ubus_value child) -> std::expected<ubus_msg, status_t> {
        ubus_msg recipe;
        for (const auto field : child.values()) {
            if (field.name() == "optional") continue;
            const auto status = append(recipe.data, field.name(), field, recipe, config_source, context);
            if (status != STATUS_OK) return std::unexpected(status);
        }
        if (!recipe.data["routes"] && config["routes"]) recipe.data.put("routes", config["routes"]);
        recipe.data.put("$package", child["name"]);
        return recipe;
    };
    status = owner.recover([&](ubus_msg snapshot) -> std::expected<ubus_msg, status_t> {
        const auto package = snapshot.data["$package"].as<std::string_view>();
        if (!package) return snapshot;
        for (const auto child : config["children"].values()) if (child["name"].as<std::string_view>() == package) {
            auto recipe = recipe_for(child); if (!recipe) return recipe;
            recipe->data.put("$package", *package); return recipe;
        }
        return std::unexpected(STATUS_NOT_FOUND);
    });
    if (status != STATUS_OK) return status;
    for (const auto child : config["children"].values()) {
        const auto name = child["name"].as<std::string_view>().value_or("");
        if (owner.find(name)) continue;
        if (owner.known(name)) continue;
        const auto program = child["program"].as<std::string_view>();
        if (!program) return STATUS_BAD_ARGS;
        if (child["optional"].as<bool>().value_or(false) && !archive_image(*program)) continue;
        auto recipe = recipe_for(child); if (!recipe) return recipe.error();
        const auto installed = owner.configure(std::move(*recipe)); if (!installed) return installed.error();
    }
    status = owner.attach(parent); if (status != STATUS_OK) return status;
    for (;;) {
        status = owner.poll(); if (status != STATUS_OK) return status;
        status = _ev_wait(&_events(), owner.deadline()).status;
        if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    }
}
