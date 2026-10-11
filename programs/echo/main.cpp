#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (i != 1 && write(STDOUT_FILENO, " ", 1) != 1) return 1;
        const auto size = ::strlen(argv[i]);
        if (write(STDOUT_FILENO, argv[i], size) != static_cast<int64_t>(size)) return 1;
    }
    return write(STDOUT_FILENO, "\n", 1) == 1 ? 0 : 1;
}
