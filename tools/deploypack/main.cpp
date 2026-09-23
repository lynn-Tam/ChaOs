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
    const bool production = argc == 8 && std::string_view{argv[1]} == "production";
    const bool io_test = argc == 4 && std::string_view{argv[1]} == "io-test";
    const bool channel_test = argc == 5 && std::string_view{argv[1]} == "channel-test";
    const bool io_session = argc == 5 && std::string_view{argv[1]} == "io-session";
    const bool console = argc == 8 && std::string_view{argv[1]} == "console";
    if (!fixture && !production && !console && !io_test && !io_session && !file_session && !application && !channel_test) {
        std::cerr << "usage: deploypack OUTPUT | deploypack production OUTPUT"
                     " PROCESS_SERVER.ELF PROOF.ELF CONSUMER.ELF"
                     " PAGER.ELF UART.ELF | deploypack console OUTPUT"
                     " UART.ELF PROCESS_SERVER.ELF SHELL.ELF BLOCK.ELF FILES.ELF"
                     " | deploypack io-test OUTPUT WORKER.ELF"
                     " | deploypack channel-test OUTPUT COORDINATOR.ELF WORKER.ELF"
                     " | deploypack io-session OUTPUT BLOCK.ELF CLIENT.ELF"
                     " | deploypack file-session|file-fault OUTPUT BLOCK.ELF FILES.ELF CLIENT.ELF"
                     " | deploypack application[-exhaustion|-denied] OUTPUT NAME IMAGE.ELF\n";
        return 2;
    }
    try {
        const auto bytes = file_session ? myos::deploy::host::pack_file_session(argv + 3, file_fault)
            : channel_test ? myos::deploy::host::pack_channel_test(argv[3], argv[4])
            : application ? myos::deploy::host::pack_application(argv[3], argv[4], exhaustion ? 9'000'000 : myos::deploy::host::ApplicationBudget, denied)
            : io_session ? myos::deploy::host::pack_io_session(argv[3], argv[4])
            : io_test ? myos::deploy::host::pack_io_test(argv[3])
            : console ? myos::deploy::host::pack_console(argv + 3) : production
            ? myos::deploy::host::pack_production(
                  myos::deploy::host::production_image_metrics(argv[3]),
                  myos::deploy::host::production_image_metrics(argv[4]),
                  myos::deploy::host::production_image_metrics(argv[5]),
                  myos::deploy::host::production_image_metrics(argv[6]),
                  myos::deploy::host::production_image_metrics(argv[7]))
            : myos::deploy::host::pack_fixture();
        if (console || io_test || io_session || file_session || application || channel_test) {
            const int error = validate_manifest(bytes.data(), bytes.size());
            if (error != 0) throw std::runtime_error("invalid generated manifest: " + std::to_string(error));
        }
        const char* output_path = (production || console || io_test || io_session || file_session || application || channel_test) ? argv[2] : argv[1];
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
