// Compile upstream once, keeping this small CTZ projection beside the
// implementation it depends on. No second filesystem or on-disk decoder.
#include <third_party/littlefs/lfs.c>

int lfs_file_map(lfs_t *lfs, lfs_file_t *file, lfs_off_t pos,
        lfs_block_t *block, lfs_off_t *off, lfs_size_t *size) {
    int err = LFS_LOCK(lfs->cfg);
    if (err) {
        return err;
    }
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
    if ((file->flags & LFS_F_INLINE)
#ifndef LFS_READONLY
            || (file->flags & LFS_F_WRITING)
#endif
            ) {
        err = LFS_ERR_INVAL;
    } else if (pos >= file->ctz.size || *size == 0) {
        *size = 0;
        *block = LFS_BLOCK_NULL;
        *off = 0;
    } else {
        // Mapping must not alter the file cache without its READING flag.
        // littlefs uses that flag to invalidate the cache before a write.
        err = lfs_ctz_find(lfs, NULL, &lfs->rcache,
                file->ctz.head, file->ctz.size, pos, block, off);
        if (!err) {
            *size = lfs_min(*size, lfs_min(file->ctz.size-pos,
                    lfs->cfg->block_size-*off));
        }
    }
    LFS_UNLOCK(lfs->cfg);
    return err;
}

// The filesystem cache owns dirty files past the last open reference. Only
// its owner may discard this state; detaching itself performs no writeback.
void lfs_file_drop(lfs_t *lfs, lfs_file_t *file) {
    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
    lfs_mlist_remove(lfs, (struct lfs_mlist*)file);
    if (!file->cfg->buffer) {
        lfs_free(file->cache.buffer);
    }
}
