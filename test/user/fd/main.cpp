#include <unistd.h>
#include <sys/stat.h>
#include <sys/io.h>
#include <sys/status.h>
#include <sys/storage.h>
#include <string_view>

namespace {
int cache() {
    char byte{};
    // More distinct dirty inodes than cache slots exercise ordinary writeback
    // pressure. Close preserves accepted contents without demanding durability.
    for (unsigned i = 0; i < 9; ++i) {
        char path[] = "cache-aa";
        path[6] += i / 26;
        path[7] += i % 26;
        const char value = 'a' + i;
        const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC);
        if (fd < 0 || write(fd, &value, 1) != 1 || close(fd)) return 52;
    }
    for (unsigned i = 0; i < 9; ++i) {
        char path[] = "cache-aa";
        path[6] += i / 26;
        path[7] += i % 26;
        const int fd = open(path, O_RDONLY);
        if (fd < 0 || read(fd, &byte, 1) != 1 || byte != 'a' + i || close(fd)) return 53;
    }
    if (sync()) return 54;
    return write(STDOUT_FILENO, "fd cache ok\n", 12) == 12 ? 0 : 55;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "admin") {
        if (format_volume(nullptr, 0) != denied) return 56;
        return write(STDOUT_FILENO, "admin denied\n", 13) == 13 ? 0 : 57;
    }
    if (argc == 2 && std::string_view(argv[1]) == "cache") return cache();
    // Used by the host's device trace to separate close from explicit sync.
    if (argc == 2 && std::string_view(argv[1]) == "close") {
        const int fd = open("fd-async", O_RDWR);
        if (fd < 0 || pwrite(fd, "C", 1, 0) != 1 || close(fd)) return 50;
        return write(STDOUT_FILENO, "close accepted\n", 15) == 15 ? 0 : 51;
    }

    const int a = open("fdcheck", O_RDWR | O_CREAT | O_TRUNC);
    if (a < 0 || write(a, "abcd", 4) != 4) return 1;
    const int shared = dup(a);
    const int separate = open("//./fdcheck", O_RDONLY);
    if (shared < 0 || separate < 0) return 2;
    char byte{};
    if (read(shared, &byte, 1) != 0) return 3;
    if (pread(a, &byte, 1, 1) != 1 || byte != 'b') return 4;
    if (read(separate, &byte, 1) != 1 || byte != 'a') return 5;
    if (pwrite(a, "Z", 1, 1) != 1) return 6;
    if (close(a) != 0 || write(shared, "e", 1) != 1) return 7;
    char text[5]{};
    if (pread(separate, text, 5, 0) != 5
        || text[0] != 'a' || text[1] != 'Z' || text[2] != 'c'
        || text[3] != 'd' || text[4] != 'e') return 8;
    if (read(separate, &byte, 1) != 1 || byte != 'Z') return 9;
    if (fsync(shared) != 0 || rename("fdcheck", "fdrenamed") != 0) return 10;
    if (pwrite(shared, "Q", 1, 0) != 1 || rename("fdrenamed", "/fdrenamed") != 0) return 22;
    const int renamed = open("fdrenamed", O_RDONLY);
    if (renamed < 0 || read(renamed, &byte, 1) != 1 || byte != 'Q' || close(renamed)) return 23;
    if (pread(separate, &byte, 1, 4) != 1 || byte != 'e') return 11;
    if (unlink("fdrenamed") != 0) return 12;
    const int replacement = open("fdrenamed", O_WRONLY | O_CREAT | O_TRUNC);
    if (replacement < 0 || write(replacement, "new", 3) != 3) return 13;
    if (pread(shared, &byte, 1, 1) != 1 || byte != 'Z') return 14;
    if (fsync(replacement) != 0 || close(replacement) != 0) return 15;
    const int current = open("fdrenamed", O_RDONLY);
    if (current < 0 || read(current, &byte, 1) != 1 || byte != 'n') return 16;
    if (close(current) != 0 || close(shared) != 0 || close(separate) != 0) return 17;
    const int first = open("fdrenamed", O_WRONLY | O_APPEND);
    const int second = open("fdrenamed", O_WRONLY | O_APPEND);
    if (first < 0 || second < 0) return 18;
    if (write(first, "a", 1) != 1 || write(second, "b", 1) != 1
        || write(first, "c", 1) != 1 || pwrite(second, "N", 1, 0) != 1) return 19;
    const int reader = open("fdrenamed", O_RDONLY);
    char appended[6]{};
    if (reader < 0 || read(reader, appended, sizeof(appended)) != 6
        || appended[0] != 'N' || appended[1] != 'e' || appended[2] != 'w'
        || appended[3] != 'a' || appended[4] != 'b' || appended[5] != 'c') return 20;
    if (fsync(first) != 0 || close(reader) != 0 || close(first) != 0 || close(second) != 0) return 21;
    if (mkdir("fd-dir")) return 24;
    const int child = open("fd-dir/item", O_RDWR | O_CREAT | O_TRUNC);
    if (child < 0 || write(child, "a", 1) != 1 || rename("fd-dir", "fd-moved")) return 25;
    if (pwrite(child, "b", 1, 0) != 1) return 26;
    const int moved = open("/fd-moved//./item", O_RDONLY);
    if (moved < 0 || read(moved, &byte, 1) != 1 || byte != 'b') return 27;
    const int target = open("fd-target", O_RDWR | O_CREAT | O_TRUNC);
    if (target < 0 || write(target, "A", 1) != 1
        || rename("fd-moved/item", "fd-target") || pwrite(child, "c", 1, 0) != 1) return 28;
    const int replaced = open("/fd-target", O_RDONLY);
    if (replaced < 0 || read(replaced, &byte, 1) != 1 || byte != 'c'
        || pread(target, &byte, 1, 0) != 1 || byte != 'A'
        || pread(moved, &byte, 1, 0) != 1 || byte != 'c') return 29;
    if (fsync(child) || close(child) || close(moved) || close(target) || close(replaced)) return 30;
    const int async = open("fd-async", O_RDWR | O_CREAT | O_TRUNC);
    char source[3 * IO_LIMIT + 7]{};
    for (unsigned i = 0; i < sizeof(source); ++i) source[i] = 'a' + i % 26;
    if (async < 0 || write(async, source, sizeof(source)) != sizeof(source) || fsync(async)) return 31;
    char boundary[20]{};
    if (pread(async, boundary, sizeof(boundary), IO_LIMIT - 6) != sizeof(boundary)) return 45;
    for (unsigned i = 0; i < sizeof(boundary); ++i)
        if (boundary[i] != source[IO_LIMIT - 6 + i]) return 46;
    if (pread(async, boundary, sizeof(boundary), sizeof(source) - 3) != 3
        || boundary[0] != source[sizeof(source) - 3]) return 47;
    request_id ids[IO_DEPTH]{};
    char bytes[IO_DEPTH]{};
    for (unsigned i = 0; i < IO_DEPTH; ++i) {
        io_request request{async, IO_READ, &bytes[i], 1, i * 257};
        if (submit(&request, &ids[i])) return 32;
    }
    io_request overflow{async, IO_READ, &byte, 1, 0};
    request_id extra{};
    if (submit(&overflow, &extra) != busy || fsync(async) || cancel(ids[0]) != busy
        || close(async)) return 33;
    bool seen[IO_DEPTH]{};
    for (unsigned i = 0; i < IO_DEPTH; ++i) {
        io_completion result{};
        if (completion(&result, IO_WAIT) != 1 || result.result != 1) return 34;
        unsigned j{};
        while (j < IO_DEPTH && ids[j] != result.id) ++j;
        if (j == IO_DEPTH || seen[j] || bytes[j] != source[j * 257]) return 35;
        seen[j] = true;
    }
    io_completion empty{};
    if (completion(&empty, 0) != 0) return 36;
    const int writer = open("fd-async", O_RDWR);
    char before[2]{};
    io_request prior{writer, IO_READ, before, sizeof(before), 0};
    if (writer < 0 || submit(&prior, &extra) || pwrite(writer, "Z", 1, 0) != 1) return 48;
    io_completion prior_result{};
    if (completion(&prior_result, IO_WAIT) != 1 || prior_result.id != extra
        || prior_result.result != sizeof(before) || before[0] != source[0]
        || before[1] != source[1]) return 49;
    char changed[] = "QR";
    for (unsigned i = 0; i < 2; ++i) {
        io_request request{writer, IO_WRITE, &changed[i], 1, i};
        if (writer < 0 || submit(&request, &ids[i])) return 41;
    }
    if (fsync(writer) || close(writer)) return 42;
    for (unsigned i = 0; i < 2; ++i) {
        io_completion result{};
        if (completion(&result, IO_WAIT) != 1 || result.result != 1) return 43;
    }
    const int verify = open("fd-async", O_RDONLY);
    char changed_read[2]{};
    io_request denied_write{verify, IO_WRITE, changed, 2, 0};
    if (verify < 0 || pread(verify, changed_read, 2, 0) != 2
        || changed_read[0] != 'Q' || changed_read[1] != 'R'
        || pwrite(verify, changed, 2, 0) != denied
        || submit(&denied_write, &extra) != denied || close(verify)) return 44;
    const int inflight = open("/boot/README.TXT", O_RDONLY);
    char head[8][16]{};
    for (unsigned i = 0; i < 8; ++i) {
        io_request request{inflight, IO_READ, head[i], sizeof(head[i]), 0};
        if (inflight < 0 || submit(&request, &ids[i])) return 37;
    }
    if (close(inflight)) return 38;
    for (unsigned i = 0; i < 8; ++i) {
        io_completion result{};
        if (completion(&result, IO_WAIT) != 1 || result.result != sizeof(head[0])) return 39;
    }
    for (unsigned i = 1; i < 8; ++i)
        for (unsigned j = 0; j < sizeof(head[0]); ++j)
            if (head[i][j] != head[0][j]) return 40;
    return write(STDOUT_FILENO, "fd offsets ok\n", 14) == 14 ? 0 : 11;
}
