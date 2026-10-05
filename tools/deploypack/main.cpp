#include <fstream>
#include <iostream>
#include "manifest.hpp"

auto validate_manifest(const void*, size_t) -> int;

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    const bool app = mode == "application" && (argc == 5 || argc == 6);
    const bool console = mode == "console" && argc == 9;
    const bool storage = mode == "console-storage" && (argc == 11 || argc == 12);
    const std::string_view mode_access = app && argc == 6 ? argv[5] : "none";
    const bool valid_perms = mode_access == "none" || mode_access == "read"
        || mode_access == "write" || mode_access == "admin";
    if ((!app && !console && !storage) || !valid_perms) {
        std::cerr << "usage: deploypack application OUTPUT NAME ELF [none|read|write|admin] | "
            "console OUTPUT UART PROCESS SHELL BLOCK FILES VFS | "
            "console-storage OUTPUT UART PROCESS SHELL BLOCK FILES DATA_BLOCK STORE VFS [VOLUME_ID]\n";
        return 2;
    }
    try {
        const auto bytes = app ? deploy::host::pack_application(argv[3], argv[4],
                mode_access == "read" ? deploy::host::access::read
                : mode_access == "write" ? deploy::host::access::write
                : mode_access == "admin" ? deploy::host::access::admin : deploy::host::access::none)
            : deploy::host::pack_console(argv + 3, storage,
                argc == 12 ? std::string_view{argv[11]} : std::string_view{});
        const int error = validate_manifest(bytes.data(), bytes.size());
        if (error) throw std::runtime_error("invalid manifest: " + std::to_string(error));
        std::ofstream out(argv[2], std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return out ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "deploypack: " << error.what() << '\n';
        return 1;
    }
}
