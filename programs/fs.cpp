#include <protocols/msg.hpp>
#include <cerrno>
#include <posix.hpp>
#include <start.hpp>
#include <protocols/file.hpp>
#include <protocols/stream.hpp>
#include <unistd.h>
#include <libk/fmt.hpp>
#include <string_view>


struct output {
    void write(const char* data, size_t size) { ::write(STDOUT_FILENO, data, size); }
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

int main(int argc, char** argv) {
    const auto& env = rt_startup();
    auto directory = env.selector(vfs_service);
    if (!directory) directory = env.selector(vfs_read);
    if (const auto rc = px_init(env.pool, env.vspace, env.cspace, env.events, directory,
        {env.selector(rt_stdin), env.selector(rt_stdout), env.selector(rt_stderr)}); rc != STATUS_OK) return rc;

    if (argc < 2) return print("fs stat/create FILE | write/append FILE TEXT | save FILE [-a] | mkdir/rm PATH | mv OLD NEW | device | volid | sync\n");
    const std::string_view command = argv[1];
    if (command == "sync" && argc == 2) return px_sync();
    if ((command == "device" || command == "volid") && argc == 2) {
        uint8_t id[20]{};
        rpc_client service;
        auto status = service.open(directory, env.pool, env.cspace, env.events);
        if (status == STATUS_OK) {
            if (command == "device") status = st_device_id(service, id);
            else { uint8_t volume[16]{}; status = st_volume_id(service, volume); std::copy_n(volume, 16, id); }
        }
        if (status) return status;
        constexpr char digits[] = "0123456789abcdef";
        char text[41]{};
        const int size = command == "device" ? 20 : 16;
        for (int i = 0; i < size; ++i) { text[2*i] = digits[id[i] >> 4]; text[2*i+1] = digits[id[i] & 15]; }
        text[2*size] = '\n';
        return print({text, static_cast<size_t>(2*size+1)});
    }
    if (argc == 4 && command == "mv") return rename(argv[2], argv[3]);
    if (argc == 3 && command == "mkdir") return px_mkdir(argv[2]);
    if (argc == 3 && command == "rm") return unlink(argv[2]);
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
