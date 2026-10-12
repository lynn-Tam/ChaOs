#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const int source = open(argv[1], O_RDONLY);
    if (source < 0) return source;
    const int target = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (target < 0) { close(source); return target; }
    char data[4096];
    int status = 0;
    for (;;) {
        const auto count = read(source, data, sizeof(data));
        if (count < 0) { status = count; break; }
        if (count == 0) break;
        if (write(target, data, count) != count) { status = 1; break; }
    }
    if (status == 0) status = fsync(target);
    const int target_closed = close(target);
    const int source_closed = close(source);
    return status ? status : target_closed ? target_closed : source_closed;
}
