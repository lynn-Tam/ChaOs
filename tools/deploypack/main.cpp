#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "manifest.hpp"
auto validate_manifest(const void*, size_t) -> int;

int main(int argc, char** argv) {
    const bool file_fault = argc == 6 && std::string_view{argv[1]} == "file-fault";
    const bool file_session = argc == 6 && (std::string_view{argv[1]} == "file-session" || file_fault);
    const bool exhaustion = argc == 5 && std::string_view{argv[1]} == "application-exhaustion";
    const bool denied = argc == 5 && std::string_view{argv[1]} == "application-denied";
    const bool application = argc == 5 && (std::string_view{argv[1]} == "application" || exhaustion || denied);
    const bool fixture = argc == 2;
    const bool io_test = argc == 4 && std::string_view{argv[1]} == "io-test";
    const bool channel_test = argc == 7 && std::string_view{argv[1]} == "channel-test";
    const bool io_session = argc == 5 && std::string_view{argv[1]} == "io-session";
    const bool start_failure = argc == 9 && std::string_view{argv[1]} == "console-start-failure";
    const bool console = argc == 9 && (std::string_view{argv[1]} == "console" || start_failure);
    const bool console_storage = (argc == 11 || argc == 12)
        && std::string_view{argv[1]} == "console-storage";
    if (!fixture && !console && !console_storage && !io_test && !io_session && !file_session && !application && !channel_test) {
        std::cerr << "usage: deploypack OUTPUT | deploypack console|console-start-failure OUTPUT"
                     " UART.ELF PROCESS_SERVER.ELF SHELL.ELF BLOCK.ELF FILES.ELF VFS.ELF"
                     " | deploypack console-storage OUTPUT UART.ELF PROCESS_SERVER.ELF"
                     " SHELL.ELF BLOCK.ELF FILES.ELF BLOCK.ELF STORE.ELF VFS.ELF [VOLUME_ID]"
                     " | deploypack io-test OUTPUT WORKER.ELF"
                     " | deploypack channel-test OUTPUT COORDINATOR.ELF WORKER.ELF PROVIDER.ELF HOLDER.ELF"
                     " | deploypack io-session OUTPUT BLOCK.ELF CLIENT.ELF"
                     " | deploypack file-session|file-fault OUTPUT BLOCK.ELF FILES.ELF CLIENT.ELF"
                     " | deploypack application[-exhaustion|-denied] OUTPUT NAME IMAGE.ELF\n";
        return 2;
    }
    try {
        const auto bytes = file_session ? deploy::host::pack_file_session(argv + 3, file_fault)
            : channel_test ? deploy::host::pack_channel_test(argv[3], argv[4], argv[5], argv[6])
            : application ? deploy::host::pack_application(argv[3], argv[4], exhaustion ? 9'000'000 : deploy::host::ApplicationBudget, denied)
            : io_session ? deploy::host::pack_io_session(argv[3], argv[4])
            : io_test ? deploy::host::pack_io_test(argv[3])
            : console_storage ? deploy::host::pack_console(argv + 3, false, true,
                argc == 12 ? std::string_view{argv[11]} : std::string_view{})
            : console ? deploy::host::pack_console(argv + 3, start_failure)
            : deploy::host::pack_fixture();
        if (console || console_storage || io_test || io_session || file_session || application || channel_test) {
            const int error = validate_manifest(bytes.data(), bytes.size());
            if (error != 0) throw std::runtime_error("invalid generated manifest: " + std::to_string(error));
        }
        const char* output_path = (console || console_storage || io_test || io_session || file_session || application || channel_test) ? argv[2] : argv[1];
        std::ofstream output(output_path, std::ios::binary);
        if (!output) {
            std::cerr << "cannot open output\n";
            return 1;
        }
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        return output ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "deploypack: " << error.what() << '\n';
        return 1;
    }
}
