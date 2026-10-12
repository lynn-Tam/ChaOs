#include <sys/storage.h>
#include <sys/status.h>
#include <libk/parse.hpp>

int main(int argc, char** argv) {
    if (argc == 1) return format_volume(nullptr, 0);
    if (argc != 2) return invalid;
    const std::string_view text = argv[1];
    if (text.size() != 32) return invalid;
    uint8_t id[16];
    for (unsigned i = 0; i < sizeof(id); ++i) {
        const auto byte = libk::parse<uint8_t>(text.substr(2 * i, 2), 16);
        if (!byte) return invalid;
        id[i] = *byte;
    }
    return format_volume(id, sizeof(id));
}
