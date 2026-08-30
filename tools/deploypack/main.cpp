#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "packer.hpp"

int main(int argc, char** argv) {
    if (argc != 2 && argc != 5) {
        std::cerr << "usage: deploypack OUTPUT | deploypack production OUTPUT"
                     " PROCESS_SERVER.ELF PROOF.ELF\n";
        return 2;
    }
    const bool production = argc == 5
        && std::string_view{argv[1]} == "production";
    if (argc == 3 && !production) {
        std::cerr << "unknown deployment profile\n";
        return 2;
    }
    try {
        const auto bytes = production
            ? myos::deploy::host::pack_production(
                  myos::deploy::host::production_segment_count(argv[3]),
                  myos::deploy::host::production_segment_count(argv[4]))
            : myos::deploy::host::pack_fixture();
        const char* output_path = production ? argv[2] : argv[1];
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
