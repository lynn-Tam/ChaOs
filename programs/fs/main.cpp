#include <libk/fmt.hpp>
#include <user/lib/stream.hpp>
#include <user/lib/vfs_client.hpp>
#include <user/lib/volume_path.hpp>

namespace {
using namespace myos;

auto same(const char* a, const char* b) noexcept -> bool { return service::equal(a, b); }

void hex(stream::Writer& output, const uint8_t* bytes, size_t count) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        output.put(digits[bytes[i] >> 4]);
        output.put(digits[bytes[i] & 15]);
    }
    output.put('\n');
}

auto write_words(vfs::Client& fs, vfs::File file,
    const bootstrap::BootstrapView& info, size_t first, uint64_t offset) noexcept -> myos_status_t {
    for (size_t i = first; i < info.argument_count(); ++i) {
        if (i != first) {
            const uint8_t space = ' ';
            const auto status = fs.write(file, offset++, &space, 1);
            if (status != MYOS_STATUS_OK) return status;
        }
        const auto* word = info.argument(i);
        const size_t length = service::length(word);
        const auto status = fs.write(file, offset, reinterpret_cast<const uint8_t*>(word), length);
        if (status != MYOS_STATUS_OK) return status;
        offset += length;
    }
    return fs.sync(file);
}

auto copy(vfs::Client& fs, const char* from, const char* to) noexcept -> myos_status_t {
    const bool data_paths = volume_path::boot_name(from) == nullptr
        && volume_path::boot_name(to) == nullptr;
    const char* source_name = data_paths && *from == '/' ? from + 1 : from;
    const char* target_name = data_paths && *to == '/' ? to + 1 : to;
    if (same(source_name, target_name)) return MYOS_STATUS_BAD_ARGS;
    vfs::File source{};
    auto status = fs.open(from, vfs::Read, source);
    if (status != MYOS_STATUS_OK) return status;
    vfs::File target{};
    status = fs.open(to, vfs::Write | vfs::Create | vfs::Truncate, target);
    if (status == MYOS_STATUS_OK) {
        myos_status_t copied = MYOS_STATUS_OK;
        status = fs.read(source, [&](uint64_t offset, const uint8_t* bytes, size_t count) {
            if (copied == MYOS_STATUS_OK) copied = fs.write(target, offset, bytes, count);
        });
        if (status == MYOS_STATUS_OK) status = copied;
        if (status == MYOS_STATUS_OK) status = fs.sync(target);
        const auto closed = fs.close(target);
        if (status == MYOS_STATUS_OK) status = closed;
    }
    const auto closed = fs.close(source);
    return status == MYOS_STATUS_OK ? closed : status;
}
} // namespace

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    stream::Writer output{service::capability(info, bootstrap::imports::Stdout)};
    if (info.argument_count() < 2) {
        output.write("fs ls [DIR] | cat/stat/touch FILE | write/append FILE TEXT | mkdir/rm PATH | mv/copy OLD NEW | device | volid\n");
        exit();
    }
    const auto* command = info.argument(1);
    const auto count = info.argument_count();
    const auto arg = [&](size_t i) noexcept { return info.argument(i); };
    const bool listing = same(command, "ls") && (count == 2 || count == 3);
    const bool unary = count == 3 && (same(command, "cat") || same(command, "stat")
        || same(command, "touch") || same(command, "mkdir") || same(command, "rm"));
    const bool binary = count == 4 && (same(command, "mv") || same(command, "copy"));
    const bool writing = count >= 4 && (same(command, "write") || same(command, "append"));
    const bool identity = count == 2 && (same(command, "device") || same(command, "volid"));
    if (!listing && !unary && !binary && !writing && !identity) exit(MYOS_STATUS_BAD_ARGS);

    vfs::Client fs;
    auto status = fs.connect(info);
    if (status != MYOS_STATUS_OK) exit(status);
    if (identity) {
        if (same(command, "device")) {
            uint8_t id[20]{};
            status = fs.device_id(id);
            if (status == MYOS_STATUS_OK) hex(output, id, sizeof(id));
        } else {
            uint8_t id[store::VolumeIdSize]{};
            status = fs.volume_id(id);
            if (status == MYOS_STATUS_OK) hex(output, id, sizeof(id));
        }
        const auto closed = fs.close();
        exit(status == MYOS_STATUS_OK ? closed : status);
    }
    if (listing) {
        io::ControlMessage entry{};
        do {
            status = fs.list(entry, count == 3 ? arg(2) : "/");
            if (status != MYOS_STATUS_OK) break;
            if (entry.size != 0) { output.write(entry.data, entry.size); output.put('\n'); }
        } while (entry.value != 0);
    } else if (same(command, "mkdir")) status = fs.mkdir(arg(2));
    else if (same(command, "rm")) status = fs.remove(arg(2));
    else if (same(command, "mv")) status = fs.rename(arg(2), arg(3));
    else if (same(command, "copy")) status = copy(fs, arg(2), arg(3));
    else {
        const bool append = same(command, "append");
        const bool touch = same(command, "touch");
        const bool read = same(command, "cat") || same(command, "stat");
        vfs::File file{};
        status = fs.open(arg(2), read ? vfs::Read
            : touch ? vfs::Write | vfs::Create
            : append ? vfs::Write | vfs::Create
            : vfs::Write | vfs::Create | vfs::Truncate, file);
        if (status == MYOS_STATUS_OK) {
            if (same(command, "stat"))
                (void)libk::fmt::format_to<"{} bytes\n">(output, file.size);
            else if (same(command, "cat"))
                status = fs.read(file, [&](uint64_t, const uint8_t* bytes, size_t count) {
                    output.write(reinterpret_cast<const char*>(bytes), count);
                });
            else if (writing) status = write_words(fs, file, info, 3, append ? file.size : 0);
            const auto closed = fs.close(file);
            if (status == MYOS_STATUS_OK) status = closed;
        }
    }
    const auto closed = fs.close();
    exit(status == MYOS_STATUS_OK ? closed : status);
}
