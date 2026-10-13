#pragma once

#include <expected>

#include <third_party/littlefs/lfs.h>
#include <protocols/file.hpp>
#include <servers/read.hpp>

// Store-private littlefs extension; the caller pins mutation through I/O.
// One execution owns this queue. Requests retain independent payload slots
// until completion is taken; dependent operations wait for overlapping writes.
class blk_client final : private libk::noncopyable_nonmovable {
  public:
    [[nodiscard]] auto connect(cap_t directory, cap_t pool, cap_t cspace, cap_t events,
                               cap_t vspace, uintptr_t address) noexcept -> status_t {
        return session_.connect(directory, pool, cspace, events, vspace, address, capacity_, true);
    }
    [[nodiscard]] auto capacity() const noexcept -> uint64_t { return capacity_; }

    [[nodiscard]] auto submit(io_cmd& request, void* data = nullptr) noexcept -> status_t {
        const auto op = static_cast<io_operation>(request.operation);
        if (request.object || request.buffer || request.flags ||
            (request.length && !data && op != io_operation::Read))
            return STATUS_BAD_ARGS;
        if (op == io_operation::Flush) {
            if (request.offset || request.length)
                return STATUS_BAD_ARGS;
        } else if (op == io_operation::Identify) {
            if (request.offset || request.length != 20)
                return STATUS_BAD_ARGS;
        } else if ((op != io_operation::Read && op != io_operation::Write) || !request.length ||
                   request.length > io_buffer_size || request.length % 512 ||
                   request.offset % 512 || request.offset > capacity_ ||
                   request.length > capacity_ - request.offset)
            return STATUS_BAD_ARGS;
        return session_.submit(request, data, true);
    }
    [[nodiscard]] auto completion(io_done& result, uint64_t id = 0) noexcept -> status_t {
        return session_.completion(result, id);
    }
    [[nodiscard]] auto payload() const noexcept -> const uint8_t* { return session_.payload(); }
    [[nodiscard]] auto wait(io_done& result, uint64_t id = 0) noexcept -> status_t {
        return session_.wait(result, id);
    }
    [[nodiscard]] auto cancel(uint64_t id) noexcept -> status_t {
        if (session_.requests().error())
            return session_.requests().error();
        ch_msg request{.operation = static_cast<uint64_t>(io_control::Cancel), .value = id};
        return session_.exchange(request);
    }
    // Retire a specific dependency without consuming unrelated completions.
    [[nodiscard]] auto wait(uint64_t id) noexcept -> status_t {
        io_done result{};
        const auto status = wait(result, id);
        return status == STATUS_OK ? result.status : status;
    }
    // Read results belong to their reader; barriers retire only filesystem writes.
    [[nodiscard]] auto drain_writes() noexcept -> status_t {
        auto status = session_.requests().error();
        uint64_t after{};
        while (const auto* request = session_.requests().next(after)) {
            after = request->id;
            if (request->operation != static_cast<uint64_t>(io_operation::Write))
                continue;
            const auto result = wait(after);
            if (status == STATUS_OK)
                status = result;
        }
        return status;
    }
    [[nodiscard]] auto dependencies(uint64_t offset, size_t size) noexcept -> status_t {
        uint64_t after{};
        while (const auto* request = session_.requests().next(after)) {
            after = request->id;
            if (request->operation == static_cast<uint64_t>(io_operation::Write) &&
                request->offset < offset + size && offset < request->offset + request->length) {
                const auto status = wait(after);
                if (status != STATUS_OK)
                    return status;
            }
        }
        return session_.requests().error();
    }
    [[nodiscard]] auto read(uint64_t offset, uint8_t* data, size_t size) noexcept -> status_t {
        if ((!data && size) || size % 512 || offset % 512 || offset > capacity_ ||
            size > capacity_ - offset)
            return STATUS_BAD_ARGS;
        const auto dependency = dependencies(offset, size);
        if (dependency != STATUS_OK)
            return dependency;
        while (size) {
            const auto count = size < io_buffer_size ? size : io_buffer_size;
            const auto status = command(io_operation::Read, offset, data, count);
            if (status != STATUS_OK)
                return status;
            offset += count;
            data += count;
            size -= count;
        }
        return STATUS_OK;
    }
    [[nodiscard]] auto flush() noexcept -> status_t {
        const auto status = drain_writes();
        return status == STATUS_OK ? command(io_operation::Flush, 0, nullptr, 0) : status;
    }
    [[nodiscard]] auto identify(uint8_t (&id)[20]) noexcept -> status_t {
        const auto status = drain_writes();
        return status == STATUS_OK ? command(io_operation::Identify, 0, id, sizeof(id)) : status;
    }

  private:
    [[nodiscard]] auto command(io_operation op, uint64_t offset, void* data, size_t size) noexcept
        -> status_t {
        io_cmd request{.operation = static_cast<uint64_t>(op), .offset = offset, .length = size};
        const auto status = submit(request, data);
        return status == STATUS_OK ? wait(request.id) : status;
    }
    io_client session_{};
    uint64_t capacity_{};
};

extern "C" int lfs_file_map(lfs_t*, lfs_file_t*, lfs_off_t, lfs_block_t*, lfs_off_t*, lfs_size_t*);
extern "C" void lfs_file_drop(lfs_t*, lfs_file_t*);

// This adapter is the sole owner of the disk geometry and littlefs state.
// A 4 KiB logical erase is a real full-block overwrite on a sector device;
// sync is a real virtio-blk FLUSH. No implicit formatting occurs on mount.
class st_vol final : private libk::noncopyable_nonmovable {
  public:
    static constexpr uint32_t BlockSize = 4096;
    [[nodiscard]] auto open(blk_client& backend) noexcept -> status_t {
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
        const int result = lfs_mount(&fs_, &config_);
        if (failed_)
            return last_error_;
        if (result == LFS_ERR_CORRUPT || result == LFS_ERR_NOENT)
            return STATUS_OK;
        mounted_ = result == 0;
        return status(result);
    }

    [[nodiscard]] auto format(const uint8_t* id = nullptr) noexcept -> status_t {
        if (mounted_) {
            const int unmounted = lfs_unmount(&fs_);
            if (unmounted < 0)
                return status(unmounted);
            mounted_ = false;
        }
        const int result = lfs_format(&fs_, &config_);
        if (failed_)
            return last_error_;
        if (result < 0)
            return status(result);
        const int mounted = lfs_mount(&fs_, &config_);
        if (failed_)
            return last_error_;
        mounted_ = mounted == 0;
        if (mounted < 0)
            return status(mounted);
        if (id == nullptr)
            return STATUS_OK;
        const int written = lfs_setattr(&fs_, "/", IdentityAttribute, id, st_volume_id_size);
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
        const auto size = lfs_getattr(&fs_, "/", IdentityAttribute, id, sizeof(id));
        if (size == LFS_ERR_NOATTR)
            return STATUS_NOT_FOUND;
        if (size < 0)
            return error(size);
        return size == sizeof(id) ? STATUS_OK : STATUS_BACKING_FAILED;
    }

    // No cursor change or data-cache mutation. The server pins filesystem
    // mutation until all reads using these physical extents have completed.
    [[nodiscard]] auto extent(lfs_file_t& file, uint64_t offset, size_t size) noexcept
        -> std::expected<io_extent, status_t> {
        lfs_block_t block{};
        lfs_off_t off{};
        lfs_size_t bytes = size;
        const int result = lfs_file_map(&fs_, &file, offset, &block, &off, &bytes);
        if (result < 0)
            return std::unexpected(error(result));
        const auto start = off & ~(SectorSize - 1);
        const auto end = (off + bytes + SectorSize - 1) & ~(SectorSize - 1);
        return io_extent{uint64_t{block} * BlockSize + start, end - start, off - start, bytes};
    }

    [[nodiscard]] auto mounted() const noexcept -> bool { return mounted_; }
    [[nodiscard]] auto failed() const noexcept -> bool { return failed_; }
    [[nodiscard]] auto error(int result) const noexcept -> status_t {
        return failed_ ? last_error_ : status(result);
    }
    [[nodiscard]] auto fs() noexcept -> lfs_t* { return &fs_; }

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
            return file_no_space;
        case LFS_ERR_NOMEM:
            return STATUS_NO_MEMORY;
        case LFS_ERR_EXIST:
            return file_exists;
        case LFS_ERR_INVAL:
        case LFS_ERR_BADF:
            return STATUS_BAD_ARGS;
        case LFS_ERR_NAMETOOLONG:
            return file_name_long;
        case LFS_ERR_FBIG:
            return file_too_large;
        case LFS_ERR_ISDIR:
            return file_is_dir;
        case LFS_ERR_NOTDIR:
            return file_not_dir;
        case LFS_ERR_NOTEMPTY:
            return file_not_empty;
        default:
            return STATUS_BACKING_FAILED;
        }
    }

    [[nodiscard]] auto transfer(io_operation op, uint64_t offset, void* data, size_t size) noexcept
        -> int {
        if (failed_)
            return LFS_ERR_IO;
        const auto result = op == io_operation::Read
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
        io_cmd request{.operation = static_cast<uint64_t>(io_operation::Write),
                       .offset = offset,
                       .length = size};
        for (;;) {
            const auto status = backend_->submit(request, data);
            if (status != STATUS_BUSY)
                return status;
            io_done result{};
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
        return self(config).transfer(io_operation::Read, uint64_t{block} * BlockSize + off, buffer,
                                     size);
    }
    [[nodiscard]] static auto program(const lfs_config* config, lfs_block_t block, lfs_off_t off,
                                      const void* buffer, lfs_size_t size) noexcept -> int {
        return self(config).transfer(io_operation::Write, uint64_t{block} * BlockSize + off,
                                     const_cast<void*>(buffer), size);
    }
    [[nodiscard]] static auto erase(const lfs_config* config, lfs_block_t block) noexcept -> int {
        auto& volume = self(config);
        // Erase is required by littlefs even though virtio-blk has no erase
        // operation. Writing the whole logical block enforces its contract.
        return volume.transfer(io_operation::Write, uint64_t{block} * BlockSize, volume.erased_,
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

    blk_client* backend_{};
    lfs_config config_{};
    lfs_t fs_{};
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
