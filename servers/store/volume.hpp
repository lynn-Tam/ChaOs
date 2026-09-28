#pragma once

#include <third_party/littlefs/lfs.h>
#include <user/lib/block_client.hpp>
#include <user/lib/store_protocol.hpp>

namespace myos::store {

// This adapter is the sole owner of the disk geometry and littlefs state.
// A 4 KiB logical erase is a real full-block overwrite on a sector device;
// sync is a real virtio-blk FLUSH. No implicit formatting occurs on mount.
class Volume final : private libk::noncopyable_nonmovable {
public:
    static constexpr uint32_t BlockSize = 4096;
    [[nodiscard]] auto open(block::Client& backend) noexcept -> myos_status_t {
        backend_ = &backend;
        const uint64_t blocks = backend.capacity() / BlockSize;
        if (blocks < 8 || blocks > UINT32_MAX) return MYOS_STATUS_BAD_ARGS;
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
        if (failed_) return last_error_;
        if (result == LFS_ERR_CORRUPT || result == LFS_ERR_NOENT) return MYOS_STATUS_OK;
        mounted_ = result == 0;
        return status(result);
    }

    [[nodiscard]] auto format(const uint8_t* id = nullptr) noexcept -> myos_status_t {
        if (mounted_) {
            const int unmounted = lfs_unmount(&fs_);
            if (unmounted < 0) return status(unmounted);
            mounted_ = false;
        }
        const int result = lfs_format(&fs_, &config_);
        if (failed_) return last_error_;
        if (result < 0) return status(result);
        const int mounted = lfs_mount(&fs_, &config_);
        if (failed_) return last_error_;
        mounted_ = mounted == 0;
        if (mounted < 0) return status(mounted);
        if (id == nullptr) return MYOS_STATUS_OK;
        const int written = lfs_setattr(&fs_, "/", IdentityAttribute, id, VolumeIdSize);
        if (written < 0) return error(written);
        const auto synced = backend_->flush();
        if (synced != MYOS_STATUS_OK) {
            failed_ = true;
            last_error_ = synced;
        }
        return synced;
    }

    [[nodiscard]] auto identity(uint8_t (&id)[VolumeIdSize]) noexcept -> myos_status_t {
        if (!mounted_) return MYOS_STATUS_BACKING_FAILED;
        const auto size = lfs_getattr(&fs_, "/", IdentityAttribute, id, sizeof(id));
        if (size == LFS_ERR_NOATTR) return MYOS_STATUS_NOT_FOUND;
        if (size < 0) return error(size);
        return size == sizeof(id) ? MYOS_STATUS_OK : MYOS_STATUS_BACKING_FAILED;
    }

    [[nodiscard]] auto mounted() const noexcept -> bool { return mounted_; }
    [[nodiscard]] auto failed() const noexcept -> bool { return failed_; }
    [[nodiscard]] auto error(int result) const noexcept -> myos_status_t {
        return failed_ ? last_error_ : status(result);
    }
    [[nodiscard]] auto fs() noexcept -> lfs_t* { return &fs_; }

private:
    static constexpr uint32_t SectorSize = 512;
    static constexpr uint8_t IdentityAttribute = 1;

    [[nodiscard]] static auto status(int result) noexcept -> myos_status_t {
        switch (result) {
        case LFS_ERR_OK: return MYOS_STATUS_OK;
        case LFS_ERR_NOENT: return MYOS_STATUS_NOT_FOUND;
        case LFS_ERR_NOSPC: case LFS_ERR_NOMEM: return MYOS_STATUS_NO_MEMORY;
        case LFS_ERR_EXIST: return MYOS_STATUS_BUSY;
        case LFS_ERR_INVAL: case LFS_ERR_NAMETOOLONG: case LFS_ERR_FBIG:
        case LFS_ERR_BADF: case LFS_ERR_ISDIR: case LFS_ERR_NOTDIR:
        case LFS_ERR_NOTEMPTY: return MYOS_STATUS_BAD_ARGS;
        default: return MYOS_STATUS_BACKING_FAILED;
        }
    }

    [[nodiscard]] auto transfer(io::Operation op, uint64_t offset,
        void* data, size_t size) noexcept -> int {
        if (failed_) return LFS_ERR_IO;
        const auto result = op == io::Operation::Read
            ? backend_->read(offset, static_cast<uint8_t*>(data), size)
            : backend_->write(offset, static_cast<const uint8_t*>(data), size);
        if (result == MYOS_STATUS_OK) return 0;
        failed_ = true;
        last_error_ = result;
        return LFS_ERR_IO;
    }
    [[nodiscard]] static auto self(const lfs_config* config) noexcept -> Volume& {
        return *static_cast<Volume*>(config->context);
    }
    [[nodiscard]] static auto read(const lfs_config* config, lfs_block_t block,
        lfs_off_t off, void* buffer, lfs_size_t size) noexcept -> int {
        return self(config).transfer(io::Operation::Read,
            uint64_t{block} * BlockSize + off, buffer, size);
    }
    [[nodiscard]] static auto program(const lfs_config* config, lfs_block_t block,
        lfs_off_t off, const void* buffer, lfs_size_t size) noexcept -> int {
        return self(config).transfer(io::Operation::Write,
            uint64_t{block} * BlockSize + off, const_cast<void*>(buffer), size);
    }
    [[nodiscard]] static auto erase(const lfs_config* config, lfs_block_t block) noexcept -> int {
        auto& volume = self(config);
        // Erase is required by littlefs even though virtio-blk has no erase
        // operation. Writing the whole logical block enforces its contract.
        return volume.transfer(io::Operation::Write, uint64_t{block} * BlockSize,
            volume.erased_, BlockSize);
    }
    [[nodiscard]] static auto sync(const lfs_config* config) noexcept -> int {
        auto& volume = self(config);
        if (volume.failed_) return LFS_ERR_IO;
        const auto status = volume.backend_->flush();
        if (status == MYOS_STATUS_OK) return 0;
        volume.failed_ = true;
        volume.last_error_ = status;
        return LFS_ERR_IO;
    }

    block::Client* backend_{};
    lfs_config config_{};
    lfs_t fs_{};
    uint8_t read_cache_[BlockSize]{};
    uint8_t write_cache_[BlockSize]{};
    uint8_t lookahead_[SectorSize]{};
    uint8_t erased_[BlockSize]{};
    myos_status_t last_error_{};
    bool mounted_{};
    bool failed_{};
public:
    Volume() noexcept { for (auto& byte : erased_) byte = 0xff; }
};

} // namespace myos::store
