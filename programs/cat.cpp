#include <posix.hpp>
#include <start.hpp>
#include <protocols/file.hpp>
#include <protocols/stream.hpp>
#include <unistd.h>
#include <cerrno>

int main(int argc, char** argv) {
    const auto& env = rt_startup();
    auto directory = env.selector(vfs_service);
    if (!directory) directory = env.selector(vfs_read);
    if (const auto rc = px_init(env.alloc(), env.vspace, env.events, directory,
        {env.selector(rt_stdin), env.selector(rt_stdout), env.selector(rt_stderr)}); rc != STATUS_OK) return rc;

    if (argc > 2) return 1;
    const int fd = argc == 1 ? STDIN_FILENO : open(argv[1], O_RDONLY);
    if (fd < 0) return errno;
    char buffer[4096];
    for (;;) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0) return errno;
        if (count == 0) break;
        if (write(STDOUT_FILENO, buffer, count) != count) return 1;
    }
    return fd == STDIN_FILENO || close(fd) == 0 ? 0 : 1;
}
