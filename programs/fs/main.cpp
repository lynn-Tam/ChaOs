#include <libk/fmt.hpp>
#include <user/lib/file_client.hpp>
#include <user/lib/store_client.hpp>
#include <user/lib/stream.hpp>

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

auto write_words(store::Client& storage, store::File file,
    const bootstrap::BootstrapView& info, size_t first, uint64_t offset) noexcept -> myos_status_t {
    for (size_t i = first; i < info.argument_count(); ++i) {
        if (i != first) {
            const uint8_t space = ' ';
            const auto status = storage.write(file, offset++, &space, 1);
            if (status != MYOS_STATUS_OK) return status;
        }
        const auto* word = info.argument(i);
        const size_t length = service::length(word);
        const auto status = storage.write(file, offset, reinterpret_cast<const uint8_t*>(word), length);
        if (status != MYOS_STATUS_OK) return status;
        offset += length;
    }
    return storage.sync(file);
}
} // namespace

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    stream::Writer output{service::capability(info, bootstrap::imports::Stdout)};
    if (info.argument_count() < 2) {
        output.write("fs ls [DIR] | cat FILE | stat FILE | touch FILE | write FILE TEXT | append FILE TEXT | mkdir DIR | rm FILE | mv OLD NEW | copy BOOTFILE FILE | device | volid\n");
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

    store::Client storage;
    auto status = storage.connect(info);
    if (status != MYOS_STATUS_OK) exit(status);
    if (listing) {
        io::ControlMessage entry{};
        do {
            status = storage.list(entry, count == 3 ? arg(2) : nullptr);
            if (status != MYOS_STATUS_OK) break;
            if (entry.size != 0) { output.write(entry.data, entry.size); output.put('\n'); }
        } while (entry.value != 0);
    } else if (same(command, "device")) {
        uint8_t id[20]{};
        status = storage.device_id(id);
        if (status == MYOS_STATUS_OK) hex(output, id, sizeof(id));
    } else if (same(command, "volid")) {
        uint8_t id[store::VolumeIdSize]{};
        status = storage.volume_id(id);
        if (status == MYOS_STATUS_OK) hex(output, id, sizeof(id));
    } else if (same(command, "mkdir")) status = storage.mkdir(arg(2));
    else if (same(command, "rm")) status = storage.remove(arg(2));
    else if (same(command, "mv")) status = storage.rename(arg(2), arg(3));
    else if (same(command, "copy")) {
        files::Client boot;
        status = boot.connect(info);
        if (status == MYOS_STATUS_OK) {
            files::File source{};
            status = boot.open(arg(2), service::length(arg(2)), source);
            if (status == MYOS_STATUS_OK) {
                store::File target{};
                status = storage.open(arg(3), store::Write | store::Create | store::Truncate, target);
                if (status == MYOS_STATUS_OK) {
                    myos_status_t copied = MYOS_STATUS_OK;
                    status = boot.read(source, [&](uint64_t offset, const uint8_t* data, size_t bytes) {
                        if (copied == MYOS_STATUS_OK) copied = storage.write(target, offset, data, bytes);
                    });
                    if (status == MYOS_STATUS_OK) status = copied;
                    if (status == MYOS_STATUS_OK) status = storage.sync(target);
                    const auto closed = storage.close(target);
                    if (status == MYOS_STATUS_OK) status = closed;
                }
                const auto closed = boot.close(source);
                if (status == MYOS_STATUS_OK) status = closed;
            }
            const auto closed = boot.close();
            if (status == MYOS_STATUS_OK) status = closed;
        }
    } else {
        const bool append = same(command, "append");
        const bool touch = same(command, "touch");
        const bool read = same(command, "cat") || same(command, "stat");
        store::File file{};
        status = storage.open(arg(2), read ? store::Read
            : touch ? store::Write | store::Create
            : append ? store::Write | store::Create
            : store::Write | store::Create | store::Truncate, file);
        if (status == MYOS_STATUS_OK) {
            if (same(command, "stat"))
                (void)libk::fmt::format_to<"{} bytes\n">(output, file.size);
            else if (same(command, "cat"))
                status = storage.read(file, [&](uint64_t, const uint8_t* data, size_t bytes) {
                    output.write(reinterpret_cast<const char*>(data), bytes);
                });
            else if (writing) status = write_words(storage, file, info, 3, append ? file.size : 0);
            const auto closed = storage.close(file);
            if (status == MYOS_STATUS_OK) status = closed;
        }
    }
    const auto closed = storage.close();
    if (status == MYOS_STATUS_OK) status = closed;
    exit(status);
}
