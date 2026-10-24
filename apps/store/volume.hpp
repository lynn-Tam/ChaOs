#pragma once
#include <libk/noncopyable.hpp>
#include <ubus/ubus.hpp>
#include <vm.hpp>
#include <lfs.h>

static constexpr size_t st_limit = 4096, st_depth = 32, st_volume_id_size = 16;
enum : uint64_t { st_lookup = 1, st_modify = 2, st_execute = 4, st_admin = 8 };
enum : uint64_t { st_read = 1, st_write = 2, st_create = 4, st_truncate = 8, st_exclusive = 16, st_append = 32 };
enum : status_t { st_exists = -256, st_not_dir, st_is_dir, st_not_empty, st_name_long, st_too_large, st_no_space, st_cross_device };
enum class st_op { Read = 1, Write, Flush, Identify };
struct st_cmd { uint64_t id{}; st_op operation{}; uint64_t offset{}, buffer_offset{}, length{}; bool sequential{}; uint64_t deadline{}; };
struct st_done { uint64_t id{}; status_t status{}; uint64_t bytes{}; };
struct st_extent { uint64_t offset{}; size_t size{}, skip{}, bytes{}; };

// The filesystem owns its device buffer and pending dependencies. Public
// requests/replies carry only common Values and native Buffer capabilities.
class st_device {
    struct operation { st_cmd command; void* destination{}; };
    ubus_conn peer_;
    vm_area buffer_;
    cap_desc descriptor_;
    cap_owner exported_;
    ubus_batch<operation, st_depth> calls_;
    uint64_t capacity_{}, next_{};
public:
    auto connect(ubus_conn& directory, cap_t budget) -> status_t {
        ubus_msg request, response;
        const auto budget_idx = request.attach(budget, RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (!budget_idx) return budget_idx.error();
        request.data.cap("budget", *budget_idx);
        auto status = directory.invoke("Open", std::move(request), response); if (status != STATUS_OK) return status;
        status = peer_.adopt(response.take(response.data["handle"])); if (status != STATUS_OK) return status;
        status = peer_.invoke("Stat", {}, response); if (status != STATUS_OK) return status;
        const auto size = response.data["size"].as<uint64_t>(); if (!size) return STATUS_PEER_FAULT;
        capacity_ = *size; next_ = response.data["sequence"].as<uint64_t>().value_or(0);
        const auto made = sys::memory_create(rt_startup().alloc(), st_depth * st_limit, VM_READ | VM_WRITE);
        if (made.status != STATUS_OK) return made.status;
        auto mapped = vm_area::map(rt_vm(), cap_owner{made.value}, 0, st_depth * st_limit, VM_READ | VM_WRITE);
        if (!mapped) return mapped.error();
        buffer_ = std::move(*mapped);
        const CapView view{.version=CAP_ATTENUATION_VERSION_CURRENT, .kind=ObjMem, .size=CAP_ATTENUATION_SIZE,
            .rights=RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_REVOKE, .words={0, st_depth, VM_READ | VM_WRITE}};
        auto grant = descriptor_.derive(buffer_.memory.selector(), view); if (!grant) return grant.error();
        exported_ = std::move(*grant); return STATUS_OK;
    }
    auto capacity() const -> uint64_t { return capacity_; }
    auto error() const -> status_t { return peer_.state(); }
    auto submit(st_cmd& command, void* data = nullptr) -> status_t {
        if (command.sequential || next_ == UINT64_MAX) return STATUS_BAD_ARGS;
        const auto kind = command.operation;
        if (kind == st_op::Flush) { if (command.offset || command.length) return STATUS_BAD_ARGS; }
        else if (kind == st_op::Identify) { if (command.offset || command.length != 20 || !data) return STATUS_BAD_ARGS; }
        else if ((kind != st_op::Read && kind != st_op::Write) || !command.length || command.length > st_limit
            || command.length % 512 || command.offset % 512 || command.offset > capacity_ || command.length > capacity_ - command.offset
            || (kind == st_op::Write && !data)) return STATUS_BAD_ARGS;
        static constexpr std::array<std::string_view, 4> methods{"Read", "Write", "Flush", "Identify"};
        command.id = ++next_;
        return calls_.submit(command.id, operation{command, data}, peer_, methods[size_t(kind) - 1],
            [&](size_t slot, operation& local, ubus_msg& message) {
                command.buffer_offset = slot * st_limit; local.command.buffer_offset = command.buffer_offset;
                if (kind == st_op::Write) ::memcpy(reinterpret_cast<void*>(buffer_.address + command.buffer_offset), data, command.length);
                message.deadline = command.deadline; message.data.put("id", command.id);
                if (kind != st_op::Flush) {
                    const auto cap = message.attach(exported_.selector(), RIGHT_MAP | RIGHT_DUPLICATE); if (!cap) return STATUS_NO_MEMORY;
                    message.data.put("buffer", ubus_buffer{*cap, command.buffer_offset, command.length}).put("offset", command.offset);
                }
                return STATUS_OK;
            });
    }
    auto completion(st_done& result, uint64_t id = 0) -> status_t {
        operation local; ubus_msg response;
        const auto status = calls_.completion(local, response, id); if (status != STATUS_OK) return status;
        const auto count = response.data["bytes"].as<uint64_t>();
        auto outcome = response.status;
        if (outcome == STATUS_OK && (!count || *count != local.command.length)) outcome = STATUS_PEER_FAULT;
        const auto bytes = outcome == STATUS_OK ? *count : 0;
        if (bytes && local.destination && local.command.operation != st_op::Write)
            ::memcpy(local.destination, reinterpret_cast<const void*>(buffer_.address + local.command.buffer_offset), bytes);
        result = {local.command.id, outcome, bytes}; return STATUS_OK;
    }
    auto copy(uint64_t offset, std::span<std::byte> output) const -> status_t {
        if (offset > buffer_.size || output.size() > buffer_.size - offset) return STATUS_BAD_ARGS;
        ::memcpy(output.data(), reinterpret_cast<const void*>(buffer_.address + offset), output.size()); return STATUS_OK;
    }
    auto wait(st_done& result, uint64_t id = 0) -> status_t {
        for (;;) {
            const auto status = completion(result, id); if (status != STATUS_WOULD_BLOCK) return status;
            if (!calls_.next()) return STATUS_NOT_FOUND;
            const auto wake = ev_wait(rt_events(), calls_.deadline()).status; if (wake != STATUS_OK) return wake;
        }
    }
    auto wait(uint64_t id) -> status_t { st_done result; const auto status = wait(result, id); return status == STATUS_OK ? result.status : status; }
    auto cancel(uint64_t id) -> status_t { ubus_msg q, r; q.data.put("id", id); return peer_.invoke("Cancel", std::move(q), r); }
    auto dependencies(uint64_t offset, size_t bytes, bool all = false) -> status_t {
        uint64_t after{};
        while (const auto* item = calls_.next(after)) {
            const auto command = item->command; after = command.id;
            if (command.operation == st_op::Write && (all || (command.offset < offset + bytes && offset < command.offset + command.length))) {
                const auto status = wait(after); if (status != STATUS_OK) return status;
            }
        }
        return error();
    }
    auto drain_writes() -> status_t { return dependencies(0, 0, true); }
    auto read(uint64_t offset, uint8_t* data, size_t size) -> status_t {
        if ((!data && size) || size % 512 || offset % 512 || offset > capacity_ || size > capacity_ - offset) return STATUS_BAD_ARGS;
        const auto status = dependencies(offset, size); if (status != STATUS_OK) return status;
        while (size) {
            const auto count = std::min(size, st_limit);
            const auto status = command(st_op::Read, offset, data, count); if (status != STATUS_OK) return status;
            offset += count; data += count; size -= count;
        }
        return STATUS_OK;
    }
    auto flush() -> status_t { const auto status = drain_writes(); return status == STATUS_OK ? command(st_op::Flush, 0, nullptr, 0) : status; }
    auto identify(uint8_t (&id)[20]) -> status_t { const auto status = drain_writes(); return status == STATUS_OK ? command(st_op::Identify, 0, id, sizeof(id)) : status; }
private:
    auto command(st_op kind, uint64_t offset, void* data, size_t size) -> status_t {
        st_cmd request{.operation=kind, .offset=offset, .length=size};
        const auto status = submit(request, data); return status == STATUS_OK ? wait(request.id) : status;
    }
};
extern "C" int lfs_file_map(lfs_t*, lfs_file_t*, lfs_off_t, lfs_block_t*, lfs_off_t*, lfs_size_t*);
extern "C" void lfs_file_drop(lfs_t*, lfs_file_t*);

// This adapter is the sole owner of the disk geometry and littlefs state.
// A 4 KiB logical erase is a real full-block overwrite on a sector device;
// sync is a real virtio-blk FLUSH. No implicit formatting occurs on mount.
class st_vol final : private libk::noncopyable_nonmovable {
  public:
    static constexpr uint32_t BlockSize = 4096;
    [[nodiscard]] auto open(st_device& backend) noexcept -> status_t {
        backend_ = &backend;
        const uint64_t blocks = backend.capacity() / BlockSize;
        if (blocks < 8 || blocks > UINT32_MAX)
            return STATUS_BAD_ARGS;
        config_ = {};
        config_.context = this;
        config_.read = read;
        config_.prog = program;
        config_.erase = erase;
        config_.sync = sync;
        config_.read_size = SectorSize;
        config_.prog_size = SectorSize;
        config_.block_size = BlockSize;
        config_.block_count = static_cast<lfs_size_t>(blocks);
        config_.block_cycles = -1;
        config_.cache_size = BlockSize;
        config_.lookahead_size = sizeof(lookahead_);
        config_.read_buffer = read_cache_;
        config_.prog_buffer = write_cache_;
        config_.lookahead_buffer = lookahead_;
        const int result = lfs_mount(&st_, &config_);
        if (failed_)
            return last_error_;
        if (result == LFS_ERR_CORRUPT || result == LFS_ERR_NOENT)
            return STATUS_OK;
        mounted_ = result == 0;
        return status(result);
    }

    [[nodiscard]] auto format(const uint8_t* id = nullptr) noexcept -> status_t {
        if (mounted_) {
            const int unmounted = lfs_unmount(&st_);
            if (unmounted < 0)
                return status(unmounted);
            mounted_ = false;
        }
        const int result = lfs_format(&st_, &config_);
        if (failed_)
            return last_error_;
        if (result < 0)
            return status(result);
        const int mounted = lfs_mount(&st_, &config_);
        if (failed_)
            return last_error_;
        mounted_ = mounted == 0;
        if (mounted < 0)
            return status(mounted);
        if (id == nullptr)
            return STATUS_OK;
        const int written = lfs_setattr(&st_, "/", IdentityAttribute, id, st_volume_id_size);
        if (written < 0)
            return error(written);
        const auto synced = backend_->flush();
        if (synced != STATUS_OK) {
            failed_ = true;
            last_error_ = synced;
        }
        return synced;
    }

    [[nodiscard]] auto identity(uint8_t (&id)[st_volume_id_size]) noexcept -> status_t {
        if (!mounted_)
            return STATUS_BACKING_FAILED;
        const auto size = lfs_getattr(&st_, "/", IdentityAttribute, id, sizeof(id));
        if (size == LFS_ERR_NOATTR)
            return STATUS_NOT_FOUND;
        if (size < 0)
            return error(size);
        return size == sizeof(id) ? STATUS_OK : STATUS_BACKING_FAILED;
    }

    // No cursor change or data-cache mutation. The server pins filesystem
    // mutation until all reads using these physical extents have completed.
    [[nodiscard]] auto extent(lfs_file_t& file, uint64_t offset, size_t size) noexcept
        -> std::expected<st_extent, status_t> {
        lfs_block_t block{};
        lfs_off_t off{};
        lfs_size_t bytes = size;
        const int result = lfs_file_map(&st_, &file, offset, &block, &off, &bytes);
        if (result < 0)
            return std::unexpected(error(result));
        const auto start = off & ~(SectorSize - 1);
        const auto end = (off + bytes + SectorSize - 1) & ~(SectorSize - 1);
        return st_extent{uint64_t{block} * BlockSize + start, end - start, off - start, bytes};
    }

    [[nodiscard]] auto mounted() const noexcept -> bool { return mounted_; }
    [[nodiscard]] auto failed() const noexcept -> bool { return failed_; }
    [[nodiscard]] auto error(int result) const noexcept -> status_t {
        return failed_ ? last_error_ : status(result);
    }
    [[nodiscard]] auto fs() noexcept -> lfs_t* { return &st_; }

  private:
    static constexpr uint32_t SectorSize = 512;
    static constexpr uint8_t IdentityAttribute = 1;

    [[nodiscard]] static auto status(int result) noexcept -> status_t {
        switch (result) {
        case LFS_ERR_OK:
            return STATUS_OK;
        case LFS_ERR_NOENT:
            return STATUS_NOT_FOUND;
        case LFS_ERR_NOSPC:
            return st_no_space;
        case LFS_ERR_NOMEM:
            return STATUS_NO_MEMORY;
        case LFS_ERR_EXIST:
            return st_exists;
        case LFS_ERR_INVAL:
        case LFS_ERR_BADF:
            return STATUS_BAD_ARGS;
        case LFS_ERR_NAMETOOLONG:
            return st_name_long;
        case LFS_ERR_FBIG:
            return st_too_large;
        case LFS_ERR_ISDIR:
            return st_is_dir;
        case LFS_ERR_NOTDIR:
            return st_not_dir;
        case LFS_ERR_NOTEMPTY:
            return st_not_empty;
        default:
            return STATUS_BACKING_FAILED;
        }
    }

    [[nodiscard]] auto transfer(st_op op, uint64_t offset, void* data, size_t size) noexcept
        -> int {
        if (failed_)
            return LFS_ERR_IO;
        const auto result = op == st_op::Read
                                ? backend_->read(offset, static_cast<uint8_t*>(data), size)
                                : program(offset, data, size);
        if (result == STATUS_OK)
            return 0;
        failed_ = true;
        last_error_ = result;
        return LFS_ERR_IO;
    }
    [[nodiscard]] auto program(uint64_t offset, void* data, size_t size) noexcept -> status_t {
        // littlefs still runs in one ordered execution. Independent physical
        // writes may remain in flight; overlapping writes/readback and its
        // sync callback establish their actual device dependencies.
        const auto dependency = backend_->dependencies(offset, size);
        if (dependency != STATUS_OK)
            return dependency;
        st_cmd request{.operation = st_op::Write,
                       .offset = offset,
                       .length = size};
        for (;;) {
            const auto status = backend_->submit(request, data);
            if (status != STATUS_BUSY)
                return status;
            st_done result{};
            const auto taken = backend_->wait(result);
            if (taken != STATUS_OK)
                return taken;
            if (result.status != STATUS_OK)
                return result.status;
        }
    }
    [[nodiscard]] static auto self(const lfs_config* config) noexcept -> st_vol& {
        return *static_cast<st_vol*>(config->context);
    }
    [[nodiscard]] static auto read(const lfs_config* config, lfs_block_t block, lfs_off_t off,
                                   void* buffer, lfs_size_t size) noexcept -> int {
        return self(config).transfer(st_op::Read, uint64_t{block} * BlockSize + off, buffer,
                                     size);
    }
    [[nodiscard]] static auto program(const lfs_config* config, lfs_block_t block, lfs_off_t off,
                                      const void* buffer, lfs_size_t size) noexcept -> int {
        return self(config).transfer(st_op::Write, uint64_t{block} * BlockSize + off,
                                     const_cast<void*>(buffer), size);
    }
    [[nodiscard]] static auto erase(const lfs_config* config, lfs_block_t block) noexcept -> int {
        auto& volume = self(config);
        // Erase is required by littlefs even though virtio-blk has no erase
        // operation. Writing the whole logical block enforces its contract.
        return volume.transfer(st_op::Write, uint64_t{block} * BlockSize, volume.erased_,
                               BlockSize);
    }
    [[nodiscard]] static auto sync(const lfs_config* config) noexcept -> int {
        auto& volume = self(config);
        if (volume.failed_)
            return LFS_ERR_IO;
        const auto status = volume.backend_->flush();
        if (status == STATUS_OK)
            return 0;
        volume.failed_ = true;
        volume.last_error_ = status;
        return LFS_ERR_IO;
    }

    st_device* backend_{};
    lfs_config config_{};
    lfs_t st_{};
    uint8_t read_cache_[BlockSize]{};
    uint8_t write_cache_[BlockSize]{};
    uint8_t lookahead_[SectorSize]{};
    uint8_t erased_[BlockSize]{};
    status_t last_error_{};
    bool mounted_{};
    bool failed_{};

  public:
    st_vol() noexcept {
        for (auto& byte : erased_)
            byte = 0xff;
    }
};
