#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/storage.h>
#include <libk/fmt.hpp>
#include <string_view>

namespace {
struct output {
    void write(const char* data, size_t size) { ::write(STDOUT_FILENO, data, size); }
};
int print(std::string_view text) {
    const auto count = write(STDOUT_FILENO, text.data(), text.size());
    return count < 0 ? count : count == static_cast<int64_t>(text.size()) ? 0 : 1;
}
int list(const char* path) {
    auto* dir = opendir(path);
    if (!dir) return 1;
    dirent entry{};
    int status{};
    while ((status = readdir(dir, &entry)) > 0) {
        if (const auto error = print(entry.name)) { status = error; break; }
        if (const auto error = print("\n")) { status = error; break; }
    }
    const auto closed = closedir(dir);
    return status < 0 ? status : closed;
}
int transfer(int source, int target) {
    char bytes[4096];
    for (;;) {
        const auto size = read(source, bytes, sizeof(bytes));
        if (size <= 0) return size;
        const auto sent = write(target, bytes, size);
        if (sent != size) return sent < 0 ? sent : 1;
    }
}
int copy(const char* from, const char* to) {
    if (std::string_view(*from == '/' ? from + 1 : from)
        == (*to == '/' ? to + 1 : to)) return 1;
    const int source = open(from, O_RDONLY);
    if (source < 0) return source;
    const int target = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    int status = target < 0 ? target : transfer(source, target);
    if (status == 0) status = fsync(target);
    if (target >= 0) { const auto closed = close(target); if (!status) status = closed; }
    const auto closed = close(source);
    return status ? status : closed;
}
}

int main(int argc, char** argv) {
    if (argc < 2) return print("fs ls [DIR] | cat/stat/touch FILE | write/append FILE TEXT | mkdir/rm PATH | mv/copy OLD NEW | device | volid | sync\n");
    const std::string_view command = argv[1];
    if (command == "sync" && argc == 2) return sync();
    if (command == "ls" && (argc == 2 || argc == 3)) return list(argc == 3 ? argv[2] : "/");
    if ((command == "device" || command == "volid") && argc == 2) {
        uint8_t id[20]{};
        const int status = command == "device" ? device_id(id) : volume_id(id);
        if (status) return status;
        constexpr char digits[] = "0123456789abcdef";
        char text[41]{};
        const int size = command == "device" ? 20 : 16;
        for (int i = 0; i < size; ++i) { text[2*i] = digits[id[i] >> 4]; text[2*i+1] = digits[id[i] & 15]; }
        text[2*size] = '\n';
        return print({text, static_cast<size_t>(2*size+1)});
    }
    if (argc == 4 && command == "mv") return rename(argv[2], argv[3]);
    if (argc == 4 && command == "copy") return copy(argv[2], argv[3]);
    if (argc == 3 && command == "mkdir") return mkdir(argv[2]);
    if (argc == 3 && command == "rm") return unlink(argv[2]);
    const bool writing = argc >= 4 && (command == "write" || command == "append");
    const bool reading = argc == 3 && (command == "cat" || command == "stat");
    const bool touch = argc == 3 && command == "touch";
    if (!writing && !reading && !touch) return 1;
    const int flags = reading ? O_RDONLY : O_WRONLY | O_CREAT
        | (command == "append" ? O_APPEND : touch ? 0 : O_TRUNC);
    const int fd = open(argv[2], flags);
    if (fd < 0) return fd;
    int status{};
    if (command == "stat") {
        struct stat info{};
        status = fstat(fd, &info);
        if (!status) { output out; (void)libk::fmt::format_to<"{} bytes\n">(out, info.size); }
    } else if (command == "cat") status = transfer(fd, STDOUT_FILENO);
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
