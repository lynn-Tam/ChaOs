#include <vm.hpp>
#include <cerrno>
#include <posix.hpp>
#include <start.hpp>
#include <ubus/ubus.hpp>
#include <unistd.h>
#include <uapi/abi.h>
#include <string_view>

struct fd_image { cap_owner memory; uint64_t size; };
static auto fd_map(ubus_conn& files, std::string_view path, word_t perms = VM_READ) -> std::expected<fd_image, status_t> {
    ubus_msg request, response; request.data.put("path", path).put("perms", perms);
    const auto status = files.invoke("Map", std::move(request), response);
    if (status != STATUS_OK) return std::unexpected(status);
    const auto buffer = response.data["buffer"].as<ubus_buffer>();
    if (!buffer || buffer->offset) return std::unexpected(STATUS_PEER_FAULT);
    return fd_image{response.take(response.data["buffer"]), buffer->size};
}

static int cache() {
    char byte{};
    int files[9];
    // Last Close writes back before releasing each inode. Reopen must use the
    // filesystem contents, without a dormant cache retaining these nine files.
    for (unsigned i = 0; i < 9; ++i) {
        char path[] = "cache-aa";
        path[6] += i / 26;
        path[7] += i % 26;
        const char value = 'a' + i;
        files[i] = open(path, O_RDWR | O_CREAT | O_TRUNC, 0);
        if (files[i] < 0 || write(files[i], &value, 1) != 1)
            return 52;
    }
    for (const auto fd : files)
        if (close(fd))
            return 52;
    for (unsigned i = 0; i < 9; ++i) {
        char path[] = "cache-aa";
        path[6] += i / 26;
        path[7] += i % 26;
        const int fd = open(path, O_RDONLY);
        if (fd < 0 || read(fd, &byte, 1) != 1 || byte != 'a' + i || close(fd))
            return 53;
    }
    // Native Map sees accepted content without Sync, and cannot gain Execute
    // from this application's read-only Directory grant.
    const auto& env = rt_startup();
    ubus_conn bus, directory;
    if (bus.bind(env.environment) != STATUS_OK) return 62;
    ubus_msg query, response; query.data.put("name", "read");
    if (bus.invoke("Get", std::move(query), response) != STATUS_OK
        || directory.bind(response.capability(response.data["connector"])) != STATUS_OK) return 62;
    const auto denied = fd_map(directory, "/boot/cat.elf", VM_READ | VM_EXECUTE);
    if (denied || denied.error() != STATUS_DENIED)
        return 63;
    const int file = open("fd-map", O_RDWR | O_CREAT | O_TRUNC, 0);
    if (file < 0 || write(file, "old", 3) != 3)
        return 64;
    auto first = fd_map(directory, "fd-map");
    if (!first || first->size != 3)
        return 65;
    auto old = vm_area::map(rt_vm(), std::move(first->memory), 0, 4096, VM_READ);
    if (!old || pwrite(file, "new", 3, 0) != 3)
        return 66;
    auto second = fd_map(directory, "fd-map");
    if (!second || second->size != 3)
        return 67;
    auto fresh = vm_area::map(rt_vm(), std::move(second->memory), 0, 4096, VM_READ);
    if (!fresh || close(file))
        return 68;
    const auto* before = reinterpret_cast<const uint8_t*>(old->address);
    const auto* after = reinterpret_cast<const uint8_t*>(fresh->address);
    if (::memcmp(before, "old", 3) || ::memcmp(after, "new", 3))
        return 69;
    for (size_t i = 3; i < 4096; ++i)
        if (before[i] || after[i])
            return 70;
    if (const auto status = px_sync(); status)
        return status;
    return write(STDOUT_FILENO, "fd cache ok\n", 12) == 12 ? 0 : 55;
}

int main(int argc, char** argv) {
    if (const auto rc =
            px_init();
        rc != STATUS_OK)
        return rc;

    if (argc == 2 && std::string_view(argv[1]) == "stdio") {
        const int input = dup(STDIN_FILENO), output = dup(STDOUT_FILENO);
        char text[8]{};
        if (input < 0 || output < 0 || read(STDIN_FILENO, text, 2) != 2 || close(STDIN_FILENO) ||
            close(STDOUT_FILENO))
            return 58;
        std::size_t size = 2;
        for (;;) {
            const auto count = read(input, text + size, sizeof(text) - size);
            if (count < 0 || size == sizeof(text))
                return 59;
            if (!count)
                break;
            size += count;
        }
        if (std::string_view(text, size) != "abcdef\n" || close(input))
            return 60;
        return write(output, "stdio aliases ok\n", 17) == 17 && !close(output) ? 0 : 61;
    }

    if (argc == 2 && std::string_view(argv[1]) == "cache")
        return cache();
    // Last Close must retire the inode even without a separate Sync.
    if (argc == 2 && std::string_view(argv[1]) == "close") {
        const int fd = open("fd-async", O_RDWR);
        if (fd < 0 || pwrite(fd, "C", 1, 0) != 1 || close(fd))
            return 50;
        return write(STDOUT_FILENO, "close accepted\n", 15) == 15 ? 0 : 51;
    }

    const int a = open("fdcheck", O_RDWR | O_CREAT | O_TRUNC, 0);
    if (a < 0)
        return errno;
    if (write(a, "abcd", 4) != 4)
        return errno;
    const int shared = dup(a);
    const int separate = open("//./fdcheck", O_RDONLY);
    if (shared < 0 || separate < 0)
        return 2;
    char byte{};
    if (read(shared, &byte, 1) != 0)
        return 3;
    if (pread(a, &byte, 1, 1) != 1 || byte != 'b')
        return 4;
    if (read(separate, &byte, 1) != 1 || byte != 'a')
        return 5;
    if (pwrite(a, "Z", 1, 1) != 1)
        return 6;
    if (close(a) != 0 || write(shared, "e", 1) != 1)
        return 7;
    char text[5]{};
    if (pread(separate, text, 5, 0) != 5 || text[0] != 'a' || text[1] != 'Z' || text[2] != 'c' ||
        text[3] != 'd' || text[4] != 'e')
        return 8;
    if (read(separate, &byte, 1) != 1 || byte != 'Z')
        return 9;
    if (fsync(shared) != 0 || rename("fdcheck", "fdrenamed") != 0)
        return 10;
    if (pwrite(shared, "Q", 1, 0) != 1 || rename("fdrenamed", "/fdrenamed") != 0)
        return 22;
    const int renamed = open("fdrenamed", O_RDONLY);
    if (renamed < 0 || read(renamed, &byte, 1) != 1 || byte != 'Q' || close(renamed))
        return 23;
    if (pread(separate, &byte, 1, 4) != 1 || byte != 'e')
        return 11;
    if (unlink("fdrenamed") != 0)
        return 12;
    const int replacement = open("fdrenamed", O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (replacement < 0 || write(replacement, "new", 3) != 3)
        return 13;
    if (pread(shared, &byte, 1, 1) != 1 || byte != 'Z')
        return 14;
    if (fsync(replacement) != 0 || close(replacement) != 0)
        return 15;
    const int current = open("fdrenamed", O_RDONLY);
    if (current < 0 || read(current, &byte, 1) != 1 || byte != 'n')
        return 16;
    if (close(current) != 0 || close(shared) != 0 || close(separate) != 0)
        return 17;
    const int first = open("fdrenamed", O_WRONLY | O_APPEND);
    const int second = open("fdrenamed", O_WRONLY | O_APPEND);
    if (first < 0 || second < 0)
        return 18;
    if (write(first, "a", 1) != 1 || write(second, "b", 1) != 1 || write(first, "c", 1) != 1 ||
        pwrite(second, "N", 1, 0) != 1)
        return 19;
    const int reader = open("fdrenamed", O_RDONLY);
    char appended[6]{};
    if (reader < 0 || read(reader, appended, sizeof(appended)) != 6 || appended[0] != 'N' ||
        appended[1] != 'e' || appended[2] != 'w' || appended[3] != 'a' || appended[4] != 'b' ||
        appended[5] != 'c')
        return 20;
    if (fsync(first) != 0 || close(reader) != 0 || close(first) != 0 || close(second) != 0)
        return 21;
    if (px_mkdir("fd-dir"))
        return 24;
    const int child = open("fd-dir/item", O_RDWR | O_CREAT | O_TRUNC, 0);
    if (child < 0 || write(child, "a", 1) != 1 || rename("fd-dir", "fd-moved"))
        return 25;
    if (pwrite(child, "b", 1, 0) != 1)
        return 26;
    const int moved = open("/fd-moved//./item", O_RDONLY);
    if (moved < 0 || read(moved, &byte, 1) != 1 || byte != 'b')
        return 27;
    const int target = open("fd-target", O_RDWR | O_CREAT | O_TRUNC, 0);
    if (target < 0 || write(target, "A", 1) != 1 || rename("fd-moved/item", "fd-target") ||
        pwrite(child, "c", 1, 0) != 1)
        return 28;
    const int replaced = open("/fd-target", O_RDONLY);
    if (replaced < 0 || read(replaced, &byte, 1) != 1 || byte != 'c' ||
        pread(target, &byte, 1, 0) != 1 || byte != 'A' || pread(moved, &byte, 1, 0) != 1 ||
        byte != 'c')
        return 29;
    if (fsync(child) || close(child) || close(moved) || close(target) || close(replaced))
        return 30;
    const int async = open("fd-async", O_RDWR | O_CREAT | O_TRUNC, 0);
    char source[3 * IO_LIMIT + 7]{};
    for (unsigned i = 0; i < sizeof(source); ++i)
        source[i] = 'a' + i % 26;
    if (async < 0 || write(async, source, sizeof(source)) != sizeof(source) || fsync(async))
        return 31;
    char boundary[20]{};
    if (pread(async, boundary, sizeof(boundary), IO_LIMIT - 6) != sizeof(boundary))
        return 45;
    for (unsigned i = 0; i < sizeof(boundary); ++i)
        if (boundary[i] != source[IO_LIMIT - 6 + i])
            return 46;
    if (pread(async, boundary, sizeof(boundary), sizeof(source) - 3) != 3 ||
        boundary[0] != source[sizeof(source) - 3])
        return 47;
    request_id ids[IO_DEPTH]{};
    char bytes[IO_DEPTH]{};
    for (unsigned i = 0; i < IO_DEPTH; ++i) {
        io_request request{async, IO_READ, &bytes[i], 1, i * 257};
        if (px_submit(&request, &ids[i]))
            return 32;
    }
    io_request overflow{async, IO_READ, &byte, 1, 0};
    request_id extra{};
    if (px_submit(&overflow, &extra) != STATUS_BUSY || fsync(async) ||
        px_cancel(ids[0]) != STATUS_BUSY || close(async))
        return 33;
    bool seen[IO_DEPTH]{};
    for (unsigned i = 0; i < IO_DEPTH; ++i) {
        io_completion result{};
        if (px_completion(&result, IO_WAIT) != 1 || result.result != 1)
            return 34;
        unsigned j{};
        while (j < IO_DEPTH && ids[j] != result.id)
            ++j;
        if (j == IO_DEPTH || seen[j] || bytes[j] != source[j * 257])
            return 35;
        seen[j] = true;
    }
    io_completion empty{};
    if (px_completion(&empty, 0) != 0)
        return 36;
    const int writer = open("fd-async", O_RDWR);
    char before[2]{};
    io_request prior{writer, IO_READ, before, sizeof(before), 0};
    if (writer < 0 || px_submit(&prior, &extra) || pwrite(writer, "Z", 1, 0) != 1)
        return 48;
    io_completion prior_result{};
    if (px_completion(&prior_result, IO_WAIT) != 1 || prior_result.id != extra ||
        prior_result.result != sizeof(before) || before[0] != source[0] || before[1] != source[1])
        return 49;
    char changed[] = "QR";
    for (unsigned i = 0; i < 2; ++i) {
        io_request request{writer, IO_WRITE, &changed[i], 1, i};
        if (writer < 0 || px_submit(&request, &ids[i]))
            return 41;
    }
    if (fsync(writer) || close(writer))
        return 42;
    for (unsigned i = 0; i < 2; ++i) {
        io_completion result{};
        if (px_completion(&result, IO_WAIT) != 1 || result.result != 1)
            return 43;
    }
    const int verify = open("fd-async", O_RDONLY);
    char changed_read[2]{};
    io_request denied_write{verify, IO_WRITE, changed, 2, 0};
    if (verify < 0 || pread(verify, changed_read, 2, 0) != 2 || changed_read[0] != 'Q' ||
        changed_read[1] != 'R' || (pwrite(verify, changed, 2, 0) != -1 || errno != EACCES) ||
        px_submit(&denied_write, &extra) != STATUS_DENIED || close(verify))
        return 44;
    const int inflight = open("/boot/README.TXT", O_RDONLY);
    char head[8][16]{};
    for (unsigned i = 0; i < 8; ++i) {
        io_request request{inflight, IO_READ, head[i], sizeof(head[i]), 0};
        if (inflight < 0 || px_submit(&request, &ids[i]))
            return 37;
    }
    if (close(inflight))
        return 38;
    for (unsigned i = 0; i < 8; ++i) {
        io_completion result{};
        if (px_completion(&result, IO_WAIT) != 1 || result.result != sizeof(head[0]))
            return 39;
    }
    for (unsigned i = 1; i < 8; ++i)
        for (unsigned j = 0; j < sizeof(head[0]); ++j)
            if (head[i][j] != head[0][j])
                return 40;
    return write(STDOUT_FILENO, "fd offsets ok\n", 14) == 14 ? 0 : 11;
}
