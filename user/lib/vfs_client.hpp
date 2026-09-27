#pragma once

#include <user/lib/store_client.hpp>

namespace myos::vfs {

using File = store::File;
using store::Read;
using store::Write;
using store::Create;
using store::Truncate;
using store::Exclusive;

// VFS and Store share the bounded file session wire format. Only the VFS
// directory is granted to ordinary programs; backend management stays private.
class Client final : public store::Session {
public:
    [[nodiscard]] auto connect(const bootstrap::BootstrapView& info,
        bootstrap::Import directory = bootstrap::imports::Vfs) noexcept -> myos_status_t {
        return store::Session::connect(info, directory);
    }
};

} // namespace myos::vfs
