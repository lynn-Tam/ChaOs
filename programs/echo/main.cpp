#include <unistd.h>

namespace {
auto length(const char* text) -> uint64_t {
    uint64_t size{};
    while (text[size] != '\0') ++size;
    return size;
}
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (i != 1 && write(STDOUT_FILENO, " ", 1) != 1) return 1;
        const auto size = length(argv[i]);
        if (write(STDOUT_FILENO, argv[i], size) != static_cast<int64_t>(size)) return 1;
    }
    return write(STDOUT_FILENO, "\n", 1) == 1 ? 0 : 1;
}
