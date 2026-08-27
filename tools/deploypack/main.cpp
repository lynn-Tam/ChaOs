#include <fstream>
#include <iostream>
#include <string>

#include "packer.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: deploypack OUTPUT\n";
        return 2;
    }
    const auto bytes = myos::deploy::host::pack_fixture();
    std::ofstream output(argv[1], std::ios::binary);
    if (!output) {
        std::cerr << "cannot open output\n";
        return 1;
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return output ? 0 : 1;
}
