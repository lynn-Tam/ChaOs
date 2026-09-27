#include <user/lib/clock.hpp>
#include <user/lib/vfs_client.hpp>
#include <user/lib/stream.hpp>
#include <user/lib/terminal.hpp>
#include <user/lib/volume_path.hpp>

namespace {
using namespace myos;

class Draft final {
    vfs::Client& storage_;
    vfs::File file_{};
    char target_[sizeof(io::ControlMessage::data) + 1]{};
    char temporary_[24]{};
    uint64_t offset_{};
    bool present_{};
public:
    explicit Draft(vfs::Client& storage) noexcept : storage_(storage) {}
    auto present() const noexcept -> bool { return present_; }
    auto path() const noexcept -> const char* { return temporary_; }

    auto open(const char* path) noexcept -> myos_status_t {
        const size_t length = service::length(path);
        if (length == 0 || length > sizeof(io::ControlMessage::data)) return MYOS_STATUS_BAD_ARGS;
        if (volume_path::boot_name(path) != nullptr) return MYOS_STATUS_DENIED;
        service::copy(target_, path, length + 1);
        const auto now = clock_now();
        if (now.status != MYOS_STATUS_OK) return now.status;
        uint64_t candidate = now.value;
        constexpr char digits[] = "0123456789abcdef";
        for (;;) {
            service::copy(temporary_, "/.edit-", 7);
            for (size_t i = 0; i < 16; ++i)
                temporary_[7 + i] = digits[(candidate >> ((15 - i) * 4)) & 15];
            temporary_[23] = '\0';
            const auto status = storage_.open(temporary_,
                vfs::Write | vfs::Create | vfs::Exclusive, file_);
            if (status == MYOS_STATUS_BUSY && candidate != UINT64_MAX) { ++candidate; continue; }
            if (status == MYOS_STATUS_OK) present_ = true;
            return status;
        }
    }

    auto line(const char* text) noexcept -> myos_status_t {
        const size_t length = service::length(text);
        auto status = storage_.write(file_, offset_,
            reinterpret_cast<const uint8_t*>(text), length);
        if (status != MYOS_STATUS_OK) return status;
        offset_ += length;
        const uint8_t newline = '\n';
        status = storage_.write(file_, offset_, &newline, 1);
        if (status == MYOS_STATUS_OK) ++offset_;
        return status;
    }

    auto save() noexcept -> myos_status_t {
        auto status = storage_.sync(file_);
        const auto closed = storage_.close(file_);
        if (status == MYOS_STATUS_OK) status = closed;
        if (status == MYOS_STATUS_OK) {
            status = storage_.rename(temporary_, target_);
            if (status == MYOS_STATUS_OK) present_ = false;
        }
        return status;
    }

    auto discard() noexcept -> myos_status_t {
        auto status = storage_.close(file_);
        const auto removed = storage_.remove(temporary_);
        if (removed == MYOS_STATUS_OK) present_ = false;
        if (status == MYOS_STATUS_OK) status = removed;
        return status;
    }
};
} // namespace

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    if (info.argument_count() != 2) exit(MYOS_STATUS_BAD_ARGS);
    const auto stdout = service::capability(info, bootstrap::imports::Stdout);
    stream::Writer output{stdout};
    vfs::Client storage;
    auto status = storage.connect(info);
    if (status != MYOS_STATUS_OK) exit(status);
    Draft draft{storage};
    status = draft.open(info.argument(1));
    if (status != MYOS_STATUS_OK) exit(status);
    terminal::LineReader input{service::capability(info, bootstrap::imports::Stdin), stdout};
    output.write("Enter replacement text; '.' saves, ':q' cancels.\n");
    char line[128]{};
    for (;;) {
        output.write("edit> ");
        const auto result = input.read(line);
        if (result == terminal::LineResult::TooLong) {
            output.write("line too long\n");
            continue;
        }
        if (result == terminal::LineResult::End || service::equal(line, ":q")) {
            status = draft.discard();
            break;
        }
        if (service::equal(line, ".")) {
            status = draft.save();
            break;
        }
        status = draft.line(line);
        if (status != MYOS_STATUS_OK) {
            (void)draft.discard();
            break;
        }
    }
    if (draft.present()) {
        output.write("draft: ");
        output.write(draft.path());
        output.put('\n');
    }
    const auto closed = storage.close();
    if (status == MYOS_STATUS_OK) status = closed;
    exit(status);
}
