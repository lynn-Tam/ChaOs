#include <cerrno>
#include <posix.hpp>
#include <start.hpp>
#include <ubus/ubus.hpp>
#include <dirent.h>
#include <unistd.h>
#include <libk/fmt.hpp>
#include <libk/parse.hpp>
#include <string>
#include <string_view>
#include <vector>


enum : status_t { fs_exists = -256 };
static auto fs_path(std::string_view path, std::string_view base = "/") -> std::string {
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

struct output {
    int fd{STDOUT_FILENO};
    void write(const char* data, size_t size) { ::write(fd, data, size); }
};
static int print(std::string_view text) {
    const auto count = write(STDOUT_FILENO, text.data(), text.size());
    return count < 0 ? count : count == static_cast<int64_t>(text.size()) ? 0 : 1;
}
static int transfer(int source, int target) {
    char bytes[4096];
    for (;;) {
        const auto size = read(source, bytes, sizeof(bytes));
        if (size <= 0) return size;
        const auto sent = write(target, bytes, size);
        if (sent != size) return sent < 0 ? sent : 1;
    }
}

static int make_dir(const std::string& path, bool parents) {
    auto make = [parents](const char* name) {
        const int status = px_mkdir(name);
        // Existing directories also succeed on a read-only mount with -p.
        if (parents && (status == fs_exists || status == STATUS_DENIED)) {
            auto* dir = opendir(name);
            return dir ? closedir(dir) : -1;
        }
        return status;
    };
    if (parents)
        for (auto slash = path.find('/', 1); slash != path.npos; slash = path.find('/', slash + 1))
            if (const int status = make(path.substr(0, slash).c_str())) return status;
    return make(path.c_str());
}
static int remove_path(std::string_view path, bool recursive, bool force) {
    if (path.empty()) return force ? 0 : -ENOENT;
    while (path.size() > 1 && path.ends_with('/')) path.remove_suffix(1);
    const auto name = path.substr(path.find_last_of('/') + 1);
    auto* base = getcwd(nullptr, 0);
    if (!base) return -errno;
    const auto absolute = fs_path(path, base);
    free(base);
    if (recursive && (absolute == "/" || name == "." || name == "..")) return -EINVAL;
    struct entry { std::string path; bool visited{}; };
    std::vector<entry> pending{{absolute}};
    int result{};
    while (!pending.empty()) {
        auto current = std::move(pending.back()); pending.pop_back();
        if (!current.visited) {
            if (auto* dir = opendir(current.path.c_str())) {
                if (!recursive) { closedir(dir); return -EISDIR; }
                pending.push_back({current.path, true});
                errno = 0;
                while (auto* child = readdir(dir)) {
                    const std::string_view name{child->d_name};
                    if (name != "." && name != "..") pending.push_back({current.path + "/" + std::string{name}});
                }
                const int error = errno;
                if (closedir(dir) < 0 && !result) result = -errno;
                if (error && !result) result = -error;
                continue;
            }
            if (errno != ENOTDIR && errno != ENOENT) { if (!result) result = -errno; continue; }
        }
        if (unlink(current.path.c_str()) < 0 && !(force && errno == ENOENT) && !result) result = -errno;
    }
    return result;
}

int main(int argc, char** argv) {
    const auto& env = rt_startup();
    if (const auto rc = px_init(); rc != STATUS_OK) return rc;

    std::string_view command = argv[0];
    command.remove_prefix(command.find_last_of('/') + 1);
    if (command.ends_with(".elf")) command.remove_suffix(4);
    const bool applet = command == "touch" || command == "mkdir" || command == "rm" || command == "mv";
    int first = applet ? 1 : 2;
    if (!applet) {
        if (argc < 2) return print("fs stat/create FILE | touch FILE... | mkdir [-p] DIR... | write/append FILE TEXT | save FILE [-a] | rm PATH | mv OLD NEW | device | volid | sync\n");
        command = argv[1];
    }
    if (command == "touch" || command == "mkdir" || command == "rm" || command == "mv") {
        const std::string_view usage = command == "touch" ? "touch [--] FILE...\n"
            : command == "mkdir" ? "mkdir [-p] [--] DIR...\n"
            : command == "rm" ? "rm [-rf] [--] PATH...\n" : "mv [--] SOURCE... DEST\n";
        if (argc == first + 1 && std::string_view{argv[first]} == "--help") return print(usage);
        bool parents{}, recursive{}, force{};
        for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
            const std::string_view option = argv[first];
            if (option == "--") { ++first; break; }
            if (command == "mkdir" && option == "-p") parents = true;
            else if (command == "rm" && option.find_first_not_of("rRf", 1) == option.npos) {
                recursive |= option.find_first_of("rR", 1) != option.npos;
                force |= option.find('f', 1) != option.npos;
            } else { output err{STDERR_FILENO}; err.write(usage.data(), usage.size()); return 1; }
        }
        if (command == "rm" && force && first == argc) return 0;
        if (first == argc) { output err{STDERR_FILENO}; err.write(usage.data(), usage.size()); return 1; }
        std::string destination;
        bool directory{};
        if (command == "mv") {
            if (argc - first < 2) { output err{STDERR_FILENO}; err.write(usage.data(), usage.size()); return 1; }
            destination = argv[--argc];
            if (auto* dir = opendir(destination.c_str())) { directory = true; closedir(dir); }
            else if (errno != ENOENT && errno != ENOTDIR) return -errno;
            if (!directory && argc - first > 1) return -ENOTDIR;
        }
        int result{};
        for (; first < argc; ++first) {
            int status{};
            if (command == "mkdir") status = make_dir(argv[first], parents);
            else if (command == "rm") status = remove_path(argv[first], recursive, force);
            else if (command == "mv") {
                std::string_view source{argv[first]};
                while (source.size() > 1 && source.ends_with('/')) source.remove_suffix(1);
                auto target = destination;
                if (directory) target += "/" + std::string{source.substr(source.find_last_of('/') + 1)};
                if (rename(argv[first], target.c_str()) < 0) status = -errno;
            }
            else {
                const int fd = open(argv[first], O_WRONLY | O_CREAT, 0);
                status = fd < 0 ? -errno : close(fd) == 0 ? 0 : -errno;
            }
            if (status) {
                output err{STDERR_FILENO};
                (void)libk::fmt::format_to<"{}: {}: error {}\n">(err, command, argv[first], status);
                result = 1;
            }
        }
        return result;
    }
    if (command == "format") {
        if (argc != 2 && argc != 3) return STATUS_BAD_ARGS;
        ubus_conn bus, admin; ubus_msg request, response; request.data.put("name", "admin");
        auto status = bus.bind(env.environment);
        if (status == STATUS_OK) status = bus.invoke("Get", std::move(request), response);
        if (status == STATUS_OK) status = admin.bind(response.capability(response.data["connector"]));
        request = {};
        if (argc == 3) {
            const std::string_view text{argv[2]}; if (text.size() != 32) return STATUS_BAD_ARGS;
            std::array<std::byte, 16> id{};
            for (size_t i = 0; i < id.size(); ++i) {
                const auto byte = libk::parse<uint8_t>(text.substr(i * 2, 2), 16); if (!byte) return STATUS_BAD_ARGS;
                id[i] = std::byte{*byte};
            }
            request.data.put("id", std::span<const std::byte>{id});
        }
        return status == STATUS_OK ? admin.invoke("Format", std::move(request), response) : status;
    }
    if (command == "sync" && argc == 2) return px_sync();
    if ((command == "device" || command == "volid") && argc == 2) {
        uint8_t id[20]{};
        ubus_conn bus, service;
        auto status = bus.bind(env.environment);
        ubus_msg request, response; request.data.put("name", "files");
        if (status == STATUS_OK) status = bus.invoke("Get", std::move(request), response);
        if (status == STATUS_OK) status = service.bind(response.capability(response.data["connector"]));
        if (status == STATUS_OK) {
            status = service.invoke(command == "device" ? "DeviceId" : "VolumeId", {}, response);
            if (status == STATUS_OK) {
                const auto value = response.data["id"].as<std::span<const std::byte>>();
                const auto size = command == "device" ? size_t{20} : size_t{16};
                if (!value || value->size() != size) return STATUS_PEER_FAULT;
                ::memcpy(id, value->data(), size);
            }
        }
        if (status) return status;
        constexpr char digits[] = "0123456789abcdef";
        char text[41]{};
        const int size = command == "device" ? 20 : 16;
        for (int i = 0; i < size; ++i) { text[2*i] = digits[id[i] >> 4]; text[2*i+1] = digits[id[i] & 15]; }
        text[2*size] = '\n';
        return print({text, static_cast<size_t>(2*size+1)});
    }
    const bool writing = argc >= 4 && (command == "write" || command == "append");
    const bool reading = argc == 3 && command == "stat";
    const bool saving = command == "save" && (argc == 3 || (argc == 4 && std::string_view{argv[3]} == "-a"));
    const bool create = argc == 3 && command == "create";
    if (!writing && !reading && !create && !saving) return 1;
    const int flags = reading ? O_RDONLY : O_WRONLY | O_CREAT
        | ((command == "append" || (saving && argc == 4)) ? O_APPEND : create ? 0 : O_TRUNC);
    const int fd = open(argv[2], flags, 0);
    if (fd < 0) return fd;
    int status{};
    if (command == "stat") {
        uint64_t size{};
        status = px_size(fd, &size);
        if (!status) { output out; (void)libk::fmt::format_to<"{} bytes\n">(out, size); }
    } else if (saving) { status = transfer(STDIN_FILENO, fd); if (!status) status = fsync(fd); }
    else if (writing) {
        for (int i = 3; i < argc && !status; ++i) {
            if (i > 3 && write(fd, " ", 1) != 1) { status = 1; break; }
            const std::string_view word = argv[i];
            const auto size = write(fd, word.data(), word.size());
            if (size != static_cast<int64_t>(word.size())) status = size < 0 ? size : 1;
        }
        if (!status) status = fsync(fd);
    }
    const int closed = close(fd);
    return status ? status : closed;
}
