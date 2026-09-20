#include "nuggetfs.h"
#include "ata.h"
#include "heap.h"
#include "string.h"
#include "serial.h"
#include "vga.h"

static nfs_superblock_t sb;
static uint8_t *bitmap = NULL;

/* --- low-level block helpers (a "block" = NFS_SECTORS_PER_BLOCK ATA sectors) --- */

static bool block_read(uint32_t block_num, void *buf) {
    return ata_read_sectors(block_num * NFS_SECTORS_PER_BLOCK, NFS_SECTORS_PER_BLOCK, buf);
}
static bool block_write(uint32_t block_num, const void *buf) {
    return ata_write_sectors(block_num * NFS_SECTORS_PER_BLOCK, NFS_SECTORS_PER_BLOCK, buf);
}

static void bitmap_set(uint32_t bit)   { bitmap[bit/8] |= (1 << (bit%8)); }
static void bitmap_clear(uint32_t bit) { bitmap[bit/8] &= ~(1 << (bit%8)); }
static int  bitmap_test(uint32_t bit)  { return bitmap[bit/8] & (1 << (bit%8)); }

static void bitmap_flush(void) {
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++) {
        block_write(sb.bitmap_start_block + i, bitmap + i * NFS_BLOCK_SIZE);
    }
}

/* Remembers where the last free-bit search left off, so allocating many
   blocks in a row (like writing a large file) doesn't re-scan from the
   very start of the bitmap every single time - that turned what should be
   a quick O(n) allocation pass into an O(n^2) one as more of the disk
   filled up during the operation. */
static uint32_t bitmap_search_cursor = 0;

static int32_t alloc_data_block_ex(bool flush) {
    uint32_t total_data_blocks = sb.total_blocks - sb.data_start_block;
    for (uint32_t k = 0; k < total_data_blocks; k++) {
        uint32_t i = (bitmap_search_cursor + k) % total_data_blocks;
        if (!bitmap_test(i)) {
            bitmap_set(i);
            bitmap_search_cursor = i + 1;
            if (flush) bitmap_flush();
            return sb.data_start_block + i;
        }
    }
    return -1;
}

static int32_t alloc_data_block(void) { return alloc_data_block_ex(true); }

static void free_data_block_ex(uint32_t block_num, bool flush) {
    uint32_t bit = block_num - sb.data_start_block;
    bitmap_clear(bit);
    if (flush) bitmap_flush();
}
static void free_data_block(uint32_t block_num) { free_data_block_ex(block_num, true); }

/* --- inode table helpers --- */

static bool inode_read(uint32_t idx, nfs_inode_t *out) {
    uint32_t inodes_per_block = NFS_BLOCK_SIZE / sizeof(nfs_inode_t);
    uint32_t block = sb.inode_table_start_block + (idx / inodes_per_block);
    uint32_t offset = (idx % inodes_per_block) * sizeof(nfs_inode_t);

    uint8_t tmp[NFS_BLOCK_SIZE];
    if (!block_read(block, tmp)) return false;
    memcpy(out, tmp + offset, sizeof(nfs_inode_t));
    return true;
}

static bool inode_write(uint32_t idx, const nfs_inode_t *in) {
    uint32_t inodes_per_block = NFS_BLOCK_SIZE / sizeof(nfs_inode_t);
    uint32_t block = sb.inode_table_start_block + (idx / inodes_per_block);
    uint32_t offset = (idx % inodes_per_block) * sizeof(nfs_inode_t);

    uint8_t tmp[NFS_BLOCK_SIZE];
    if (!block_read(block, tmp)) return false;
    memcpy(tmp + offset, in, sizeof(nfs_inode_t));
    return block_write(block, tmp);
}

/* Same fix as the data-block bitmap search: remember where the last
   allocation left off instead of rescanning from inode 1 every time -
   this one is worse than the block-bitmap case since each candidate
   costs a real disk read (inode_read), not just a memory check. */
static uint32_t inode_search_cursor = 1;

static int32_t alloc_inode(void) {
    nfs_inode_t tmp;
    for (uint32_t k = 0; k < NFS_MAX_INODES - 1; k++) {
        uint32_t i = 1 + ((inode_search_cursor - 1 + k) % (NFS_MAX_INODES - 1));
        inode_read(i, &tmp);
        if (tmp.type == NFS_TYPE_FREE) {
            inode_search_cursor = i + 1;
            return (int32_t)i;
        }
    }
    return -1;
}

/* --- directory helpers, generalized to operate on ANY directory inode --- */

static bool dir_find_in(uint32_t dir_inode_num, const char *name, nfs_dirent_t *out_entry) {
    nfs_inode_t dir_inode;
    if (!inode_read(dir_inode_num, &dir_inode)) return false;
    uint8_t buf[NFS_BLOCK_SIZE];

    for (int b = 0; b < NFS_DIRECT_BLOCKS; b++) {
        uint32_t blk = dir_inode.blocks[b];
        if (blk == 0) continue;
        block_read(blk, buf);
        nfs_dirent_t *entries = (nfs_dirent_t*)buf;
        for (uint32_t s = 0; s < NFS_DIRENTS_PER_BLOCK; s++) {
            if (entries[s].used && strcmp(entries[s].name, name) == 0) {
                if (out_entry) *out_entry = entries[s];
                return true;
            }
        }
    }
    return false;
}

static bool dir_add_entry_in(uint32_t dir_inode_num, const char *name, uint32_t inode_num) {
    nfs_inode_t dir_inode;
    if (!inode_read(dir_inode_num, &dir_inode)) return false;
    uint8_t buf[NFS_BLOCK_SIZE];

    for (int b = 0; b < NFS_DIRECT_BLOCKS; b++) {
        uint32_t blk = dir_inode.blocks[b];
        if (blk == 0) {
            int32_t nb = alloc_data_block();
            if (nb < 0) return false;
            memset(buf, 0, NFS_BLOCK_SIZE);
            dir_inode.blocks[b] = (uint32_t)nb;
            block_write((uint32_t)nb, buf);
            inode_write(dir_inode_num, &dir_inode);
            blk = (uint32_t)nb;
        }
        block_read(blk, buf);
        nfs_dirent_t *entries = (nfs_dirent_t*)buf;
        for (uint32_t s = 0; s < NFS_DIRENTS_PER_BLOCK; s++) {
            if (!entries[s].used) {
                strncpy(entries[s].name, name, NFS_MAX_NAME - 1);
                entries[s].name[NFS_MAX_NAME-1] = '\0';
                entries[s].inode = inode_num;
                entries[s].used = 1;
                block_write(blk, buf);
                return true;
            }
        }
    }
    return false; /* directory full */
}

static bool dir_remove_entry_in(uint32_t dir_inode_num, const char *name) {
    nfs_inode_t dir_inode;
    if (!inode_read(dir_inode_num, &dir_inode)) return false;
    uint8_t buf[NFS_BLOCK_SIZE];

    for (int b = 0; b < NFS_DIRECT_BLOCKS; b++) {
        uint32_t blk = dir_inode.blocks[b];
        if (blk == 0) continue;
        block_read(blk, buf);
        nfs_dirent_t *entries = (nfs_dirent_t*)buf;
        for (uint32_t s = 0; s < NFS_DIRENTS_PER_BLOCK; s++) {
            if (entries[s].used && strcmp(entries[s].name, name) == 0) {
                entries[s].used = 0;
                block_write(blk, buf);
                return true;
            }
        }
    }
    return false;
}

static bool dir_is_empty(uint32_t dir_inode_num) {
    nfs_inode_t dir_inode;
    if (!inode_read(dir_inode_num, &dir_inode)) return false;
    uint8_t buf[NFS_BLOCK_SIZE];

    for (int b = 0; b < NFS_DIRECT_BLOCKS; b++) {
        uint32_t blk = dir_inode.blocks[b];
        if (blk == 0) continue;
        block_read(blk, buf);
        nfs_dirent_t *entries = (nfs_dirent_t*)buf;
        for (uint32_t s = 0; s < NFS_DIRENTS_PER_BLOCK; s++) {
            if (entries[s].used) return false;
        }
    }
    return true;
}

/* --- path resolution --- */

/* Resolves every component of `path` except the last, returning the inode
   number of the containing directory plus the final component's name.
   Used by create/write/delete/rename (which all need "the parent dir" plus
   "what to call/find within it"). */
static bool resolve_parent(const char *path, uint32_t *out_dir_inode, char *out_name, size_t out_name_size) {
    if (*path == '/') path++;
    if (*path == '\0') return false; /* can't operate on the root itself this way */

    uint32_t current = sb.root_inode;
    const char *p = path;

    while (1) {
        int len = 0;
        while (p[len] && p[len] != '/') len++;
        if (len == 0) return false;
        if ((size_t)len >= NFS_MAX_NAME) return false; /* path component too long - safer to fail than silently truncate into a different name */

        char component[NFS_MAX_NAME];
        memcpy(component, p, (size_t)len);
        component[len] = '\0';

        if (p[len] == '\0') {
            *out_dir_inode = current;
            strncpy(out_name, component, out_name_size - 1);
            out_name[out_name_size - 1] = '\0';
            return true;
        }

        nfs_dirent_t entry;
        if (!dir_find_in(current, component, &entry)) return false;
        nfs_inode_t inode;
        if (!inode_read(entry.inode, &inode)) return false;
        if (inode.type != NFS_TYPE_DIR) return false; /* a path component that isn't a dir */
        current = entry.inode;
        p += len + 1;
    }
}

/* Resolves the FULL path (including the last component) to an inode number -
   used for listing/entering a directory, or checking what something is. */
static bool resolve_full(const char *path, uint32_t *out_inode) {
    if (path == NULL || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        *out_inode = sb.root_inode;
        return true;
    }
    if (*path == '/') path++;

    uint32_t current = sb.root_inode;
    const char *p = path;
    while (*p) {
        int len = 0;
        while (p[len] && p[len] != '/') len++;
        if ((size_t)len >= NFS_MAX_NAME) return false; /* path component too long - safer to fail than silently truncate into a different name */

        char component[NFS_MAX_NAME];
        memcpy(component, p, (size_t)len);
        component[len] = '\0';

        nfs_dirent_t entry;
        if (!dir_find_in(current, component, &entry)) return false;
        current = entry.inode;

        p += len;
        if (*p == '/') p++;
    }
    *out_inode = current;
    return true;
}

static int32_t create_in_dir(uint32_t parent_inode, const char *name, uint8_t type) {
    if (dir_find_in(parent_inode, name, NULL)) return -1;

    int32_t idx = alloc_inode();
    if (idx < 0) return -1;

    nfs_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.type = type;
    inode.size = 0;
    inode_write((uint32_t)idx, &inode);

    if (!dir_add_entry_in(parent_inode, name, (uint32_t)idx)) {
        /* Roll back the inode we just allocated so it isn't leaked as a
           permanently-non-FREE slot with nothing pointing to it. */
        memset(&inode, 0, sizeof(inode));
        inode.type = NFS_TYPE_FREE;
        inode_write((uint32_t)idx, &inode);
        return -1;
    }
    return idx;
}

/* --- public API --- */

bool nfs_format(uint32_t total_disk_sectors) {
    uint32_t total_blocks = total_disk_sectors / NFS_SECTORS_PER_BLOCK;
    uint32_t inode_table_blocks = (NFS_MAX_INODES * sizeof(nfs_inode_t) + NFS_BLOCK_SIZE - 1) / NFS_BLOCK_SIZE;
    /* Size the bitmap to cover every block on the disk, not a guessed
       data-block count. The real data-block count depends on
       bitmap_blocks itself (via data_start_block), so estimating it with
       a fixed fudge factor could under-size the bitmap by a couple of
       bits in edge cases and let a later allocation set a bit past the
       buffer. Covering all total_blocks is a strict superset and wastes
       only a few bytes. */
    uint32_t bitmap_blocks = ((total_blocks + 7) / 8 + NFS_BLOCK_SIZE - 1) / NFS_BLOCK_SIZE;
    if (bitmap_blocks == 0) bitmap_blocks = 1;

    sb.magic = NFS_MAGIC;
    sb.total_blocks = total_blocks;
    sb.bitmap_start_block = 1;
    sb.bitmap_blocks = bitmap_blocks;
    sb.inode_table_start_block = sb.bitmap_start_block + bitmap_blocks;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_start_block = sb.inode_table_start_block + inode_table_blocks;
    sb.root_inode = 0;

    uint8_t block_buf[NFS_BLOCK_SIZE];
    memset(block_buf, 0, NFS_BLOCK_SIZE);
    memcpy(block_buf, &sb, sizeof(sb));
    block_write(0, block_buf);

    if (bitmap) { kfree(bitmap); bitmap = NULL; } /* avoid leaking if format runs more than once */
    bitmap = (uint8_t*)kmalloc(sb.bitmap_blocks * NFS_BLOCK_SIZE);
    if (!bitmap) {
        serial_write("[nfs] out of memory allocating bitmap\n");
        return false;
    }
    memset(bitmap, 0, sb.bitmap_blocks * NFS_BLOCK_SIZE);
    bitmap_flush();

    /* Zero the entire inode table directly, block by block - NFS_TYPE_FREE
       is 0, so an all-zero block already means "all inodes free". */
    memset(block_buf, 0, NFS_BLOCK_SIZE);
    for (uint32_t b = 0; b < sb.inode_table_blocks; b++) {
        block_write(sb.inode_table_start_block + b, block_buf);
    }

    /* Create root directory as inode 0 */
    nfs_inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.type = NFS_TYPE_DIR;
    inode_write(0, &root_inode);

    serial_write("[nfs] filesystem formatted\n");
    kprintf("[nfs] %u total blocks, %u data blocks, %u max inodes\n",
            total_blocks, total_blocks - sb.data_start_block, NFS_MAX_INODES);
    return true;
}

bool nfs_mount(void) {
    uint8_t block_buf[NFS_BLOCK_SIZE];
    if (!block_read(0, block_buf)) return false;
    memcpy(&sb, block_buf, sizeof(sb));

    if (sb.magic != NFS_MAGIC) {
        serial_write("[nfs] no valid filesystem found, needs format\n");
        return false;
    }

    if (bitmap) { kfree(bitmap); bitmap = NULL; } /* avoid leaking if mount runs more than once */
    bitmap = (uint8_t*)kmalloc(sb.bitmap_blocks * NFS_BLOCK_SIZE);
    if (!bitmap) {
        serial_write("[nfs] out of memory allocating bitmap\n");
        return false;
    }
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++) {
        block_read(sb.bitmap_start_block + i, bitmap + i * NFS_BLOCK_SIZE);
    }

    serial_write("[nfs] mounted existing filesystem\n");
    return true;
}

int nfs_create_path(const char *path) {
    uint32_t parent; char name[NFS_MAX_NAME];
    if (!resolve_parent(path, &parent, name, sizeof(name))) return -1;
    return create_in_dir(parent, name, NFS_TYPE_FILE);
}

int nfs_mkdir_path(const char *path) {
    uint32_t parent; char name[NFS_MAX_NAME];
    if (!resolve_parent(path, &parent, name, sizeof(name))) return -1;
    return create_in_dir(parent, name, NFS_TYPE_DIR);
}

/* Returns the data block number for logical block `index` within a file
   (0 if none allocated yet). Transparently handles the direct-blocks vs
   indirect-block cases so callers don't need to know the difference. */
static uint32_t get_inode_block(const nfs_inode_t *inode, uint32_t index) {
    if (index < NFS_DIRECT_BLOCKS) return inode->blocks[index];
    if (inode->indirect_block == 0) return 0;
    uint32_t ptrs[NFS_PTRS_PER_INDIRECT_BLOCK];
    if (!block_read(inode->indirect_block, ptrs)) {
        serial_write("[nfs] error reading indirect block!\n");
        return 0;
    }
    uint32_t ind_index = index - NFS_DIRECT_BLOCKS;
    if (ind_index >= NFS_PTRS_PER_INDIRECT_BLOCK) return 0;
    return ptrs[ind_index];
}

/* Sets the data block number for logical block `index`, allocating the
   indirect block itself on first use beyond the direct blocks. Returns
   false if a needed allocation fails (disk full). */
int nfs_write_path(const char *path, const void *data, uint32_t len) {
    if (len > NFS_MAX_FILE_SIZE) return -1;

    uint32_t parent; char name[NFS_MAX_NAME];
    if (!resolve_parent(path, &parent, name, sizeof(name))) return -1;

    nfs_dirent_t entry;
    if (!dir_find_in(parent, name, &entry)) {
        int32_t created = create_in_dir(parent, name, NFS_TYPE_FILE);
        if (created < 0) return -1;
        entry.inode = (uint32_t)created;
    }

    nfs_inode_t inode;
    if (!inode_read(entry.inode, &inode)) return -1;
    if (inode.type != NFS_TYPE_FILE) return -1;

    uint32_t blocks_needed = (len + NFS_BLOCK_SIZE - 1) / NFS_BLOCK_SIZE;
    const uint8_t *src = (const uint8_t*)data;

    /* The indirect block (if this write needs one) is read once here and
       kept in memory for the whole loop, then written back once at the
       end - instead of doing a full read-modify-write of it on every
       single block, which made large files agonizingly slow to save. */
    uint32_t indirect_ptrs[NFS_PTRS_PER_INDIRECT_BLOCK];
    bool indirect_loaded = false;
    bool indirect_dirty = false;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint32_t blk;
        if (b < NFS_DIRECT_BLOCKS) {
            blk = inode.blocks[b];
            if (blk == 0) {
                int32_t nb = alloc_data_block_ex(false);
                if (nb < 0) return -1;
                inode.blocks[b] = (uint32_t)nb;
                blk = (uint32_t)nb;
            }
        } else {
            if (!indirect_loaded) {
                if (inode.indirect_block == 0) {
                    int32_t nb = alloc_data_block_ex(false);
                    if (nb < 0) return -1;
                    inode.indirect_block = (uint32_t)nb;
                    memset(indirect_ptrs, 0, sizeof(indirect_ptrs));
                } else {
                    if (!block_read(inode.indirect_block, indirect_ptrs)) return -1;
                }
                indirect_loaded = true;
            }
            uint32_t ind_index = b - NFS_DIRECT_BLOCKS;
            if (ind_index >= NFS_PTRS_PER_INDIRECT_BLOCK) return -1;
            blk = indirect_ptrs[ind_index];
            if (blk == 0) {
                int32_t nb = alloc_data_block_ex(false);
                if (nb < 0) return -1;
                indirect_ptrs[ind_index] = (uint32_t)nb;
                indirect_dirty = true;
                blk = (uint32_t)nb;
            }
        }

        uint8_t buf[NFS_BLOCK_SIZE];
        memset(buf, 0, NFS_BLOCK_SIZE);
        uint32_t remaining = len - b * NFS_BLOCK_SIZE;
        uint32_t chunk = remaining < NFS_BLOCK_SIZE ? remaining : NFS_BLOCK_SIZE;
        memcpy(buf, src + b * NFS_BLOCK_SIZE, chunk);
        block_write(blk, buf);
    }

    /* Truncate: this write replaces the whole file from offset 0, so any
       block that belonged to a (possibly larger) previous version and now
       lies past the new end must be freed here - otherwise those blocks
       leak permanently, since delete_inode_recursive only ever walks up
       to inode.size when eventually deleting the file. inode.size still
       holds the OLD size at this point, since we haven't overwritten it
       yet. */
    uint32_t old_blocks_needed = (inode.size + NFS_BLOCK_SIZE - 1) / NFS_BLOCK_SIZE;
    if (old_blocks_needed > blocks_needed) {
        for (uint32_t b = blocks_needed; b < old_blocks_needed && b < NFS_DIRECT_BLOCKS; b++) {
            if (inode.blocks[b]) {
                free_data_block_ex(inode.blocks[b], false);
                inode.blocks[b] = 0;
            }
        }
        if (blocks_needed <= NFS_DIRECT_BLOCKS) {
            /* New file fits entirely in direct blocks - drop the whole
               indirect region. It may not have been touched by the write
               loop above (if this write never needed it), so read it
               fresh rather than assuming indirect_ptrs is valid. */
            if (inode.indirect_block) {
                uint32_t old_ptrs[NFS_PTRS_PER_INDIRECT_BLOCK];
                bool have_ptrs = indirect_loaded;
                if (!have_ptrs) have_ptrs = block_read(inode.indirect_block, old_ptrs);
                if (have_ptrs) {
                    const uint32_t *src_ptrs = indirect_loaded ? indirect_ptrs : old_ptrs;
                    for (uint32_t i = 0; i < NFS_PTRS_PER_INDIRECT_BLOCK; i++) {
                        if (src_ptrs[i]) free_data_block_ex(src_ptrs[i], false);
                    }
                }
                free_data_block_ex(inode.indirect_block, false);
                inode.indirect_block = 0;
                indirect_dirty = false; /* the block itself is gone, nothing left to write back */
            }
        } else if (indirect_loaded) {
            /* New file still uses the indirect region - free only the
               tail entries beyond it (the loop above always loads
               indirect_ptrs whenever blocks_needed reaches this far). */
            for (uint32_t i = blocks_needed - NFS_DIRECT_BLOCKS; i < NFS_PTRS_PER_INDIRECT_BLOCK; i++) {
                if (indirect_ptrs[i]) {
                    free_data_block_ex(indirect_ptrs[i], false);
                    indirect_ptrs[i] = 0;
                    indirect_dirty = true;
                }
            }
        }
    }

    if (indirect_dirty) block_write(inode.indirect_block, indirect_ptrs);
    bitmap_flush();

    inode.size = len;
    inode_write(entry.inode, &inode);
    return (int)len;
}

int nfs_read_path(const char *path, void *buf, uint32_t maxlen) {
    uint32_t inode_num;
    if (!resolve_full(path, &inode_num)) return -1;

    nfs_inode_t inode;
    if (!inode_read(inode_num, &inode)) return -1;
    if (inode.type != NFS_TYPE_FILE) return -1;

    uint32_t to_read = inode.size < maxlen ? inode.size : maxlen;
    uint32_t blocks_needed = (to_read + NFS_BLOCK_SIZE - 1) / NFS_BLOCK_SIZE;
    uint8_t *dst = (uint8_t*)buf;

    for (uint32_t b = 0; b < blocks_needed; b++) {
        uint32_t blk = get_inode_block(&inode, b);
        if (blk == 0) break;
        uint8_t blk_buf[NFS_BLOCK_SIZE];
        block_read(blk, blk_buf);
        uint32_t remaining = to_read - b * NFS_BLOCK_SIZE;
        uint32_t chunk = remaining < NFS_BLOCK_SIZE ? remaining : NFS_BLOCK_SIZE;
        memcpy(dst + b * NFS_BLOCK_SIZE, blk_buf, chunk);
    }
    return (int)to_read;
}

/* Recursively deletes everything inside a directory inode (files and
   nested subdirectories), then frees the directory's own blocks and
   marks its inode free. Used by nfs_delete_path so deleting a folder
   works the way people expect, instead of refusing non-empty ones. */
static void delete_inode_recursive(uint32_t inode_num) {
    nfs_inode_t inode;
    if (!inode_read(inode_num, &inode)) return;

    if (inode.type == NFS_TYPE_DIR) {
        uint8_t buf[NFS_BLOCK_SIZE];
        for (int b = 0; b < NFS_DIRECT_BLOCKS; b++) {
            uint32_t blk = inode.blocks[b];
            if (blk == 0) continue;
            block_read(blk, buf);
            nfs_dirent_t *entries = (nfs_dirent_t*)buf;
            for (uint32_t s = 0; s < NFS_DIRENTS_PER_BLOCK; s++) {
                if (entries[s].used) delete_inode_recursive(entries[s].inode);
            }
        }
    }

    /* Directories only ever use direct blocks in this implementation and
       don't track a byte size, so bound their loop by NFS_DIRECT_BLOCKS;
       files use their real size to know how many blocks (direct+indirect)
       they actually have. */
    uint32_t max_blocks = (inode.type == NFS_TYPE_DIR)
                          ? NFS_DIRECT_BLOCKS
                          : (inode.size + NFS_BLOCK_SIZE - 1) / NFS_BLOCK_SIZE;
    for (uint32_t b = 0; b < max_blocks; b++) {
        uint32_t blk = get_inode_block(&inode, b);
        if (blk) free_data_block_ex(blk, false);
    }
    if (inode.indirect_block) free_data_block_ex(inode.indirect_block, false);
    bitmap_flush();
    memset(&inode, 0, sizeof(inode));
    inode.type = NFS_TYPE_FREE;
    inode_write(inode_num, &inode);
}

int nfs_delete_path(const char *path) {
    uint32_t parent; char name[NFS_MAX_NAME];
    if (!resolve_parent(path, &parent, name, sizeof(name))) return -1;

    nfs_dirent_t entry;
    if (!dir_find_in(parent, name, &entry)) return -1;

    delete_inode_recursive(entry.inode);
    dir_remove_entry_in(parent, name);
    return 0;
}

bool nfs_rename_path(const char *dir_path, const char *old_name, const char *new_name) {
    uint32_t dir_inode;
    if (!resolve_full(dir_path, &dir_inode)) return false;
    if (dir_find_in(dir_inode, new_name, NULL)) return false; /* name taken */

    nfs_dirent_t entry;
    if (!dir_find_in(dir_inode, old_name, &entry)) return false;

    dir_remove_entry_in(dir_inode, old_name);
    return dir_add_entry_in(dir_inode, new_name, entry.inode);
}

bool nfs_move_path(const char *src_dir_path, const char *name, const char *dst_dir_path) {
    uint32_t src_dir, dst_dir;
    if (!resolve_full(src_dir_path, &src_dir)) return false;
    if (!resolve_full(dst_dir_path, &dst_dir)) return false;
    if (dir_find_in(dst_dir, name, NULL)) return false; /* name collision at destination */

    nfs_dirent_t entry;
    if (!dir_find_in(src_dir, name, &entry)) return false;

    dir_remove_entry_in(src_dir, name);
    return dir_add_entry_in(dst_dir, name, entry.inode);
}

bool nfs_is_dir_path(const char *path) {
    uint32_t inode_num;
    if (!resolve_full(path, &inode_num)) return false;
    nfs_inode_t inode;
    if (!inode_read(inode_num, &inode)) return false;
    return inode.type == NFS_TYPE_DIR;
}

bool nfs_exists_path(const char *path) {
    uint32_t inode_num;
    return resolve_full(path, &inode_num);
}

void nfs_list_path(const char *dir_path, void (*callback)(const char *name, uint32_t size, bool is_dir)) {
    uint32_t dir_inode_num;
    if (!resolve_full(dir_path, &dir_inode_num)) return;

    nfs_inode_t dir_inode;
    if (!inode_read(dir_inode_num, &dir_inode)) return;
    uint8_t buf[NFS_BLOCK_SIZE];

    for (int b = 0; b < NFS_DIRECT_BLOCKS; b++) {
        uint32_t blk = dir_inode.blocks[b];
        if (blk == 0) continue;
        block_read(blk, buf);
        nfs_dirent_t *entries = (nfs_dirent_t*)buf;
        for (uint32_t s = 0; s < NFS_DIRENTS_PER_BLOCK; s++) {
            if (entries[s].used) {
                nfs_inode_t inode;
                if (!inode_read(entries[s].inode, &inode)) continue;
                callback(entries[s].name, inode.size, inode.type == NFS_TYPE_DIR);
            }
        }
    }
}
