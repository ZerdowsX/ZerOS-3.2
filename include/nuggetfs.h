#ifndef NUGGET_FS_H
#define NUGGET_FS_H
#include "types.h"

#define NFS_MAGIC        0x4E554747  /* "NUGG" */
#define NFS_BLOCK_SIZE   4096         /* 8 ATA sectors per block */
#define NFS_SECTORS_PER_BLOCK (NFS_BLOCK_SIZE / 512)

#define NFS_MAX_INODES   256
#define NFS_MAX_NAME     32
#define NFS_MAX_PATH     160
#define NFS_DIRECT_BLOCKS 10
#define NFS_PTRS_PER_INDIRECT_BLOCK (NFS_BLOCK_SIZE / sizeof(uint32_t))
/* 10 direct blocks + one indirect block (holding up to 1024 more block
   pointers) = ~4 MiB per file. Comfortably covers a saved Paint canvas or
   a full-screen wallpaper BMP, which the original 40 KiB direct-only limit
   did not. */
#define NFS_MAX_FILE_SIZE ((NFS_DIRECT_BLOCKS + NFS_PTRS_PER_INDIRECT_BLOCK) * NFS_BLOCK_SIZE)

#define NFS_TYPE_FREE    0
#define NFS_TYPE_FILE    1
#define NFS_TYPE_DIR     2

typedef struct PACKED {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t bitmap_start_block;
    uint32_t bitmap_blocks;
    uint32_t inode_table_start_block;
    uint32_t inode_table_blocks;
    uint32_t data_start_block;
    uint32_t root_inode;
} nfs_superblock_t;

typedef struct PACKED {
    uint8_t  type;             /* NFS_TYPE_FILE / NFS_TYPE_DIR / NFS_TYPE_FREE */
    uint32_t size;             /* size in bytes (files); unused for dirs */
    uint32_t blocks[NFS_DIRECT_BLOCKS];
    uint32_t indirect_block;   /* 0 = none; else a block full of up to 1024 more block numbers */
} nfs_inode_t;

typedef struct PACKED {
    char     name[NFS_MAX_NAME];
    uint32_t inode;
    uint8_t  used;
} nfs_dirent_t;

/* Directory contents = array of nfs_dirent_t packed into its data block(s) */
#define NFS_DIRENTS_PER_BLOCK (NFS_BLOCK_SIZE / sizeof(nfs_dirent_t))

bool nfs_format(uint32_t total_disk_sectors);
bool nfs_mount(void);

/* Path-based API. Paths are '/'-separated, relative to the root directory
   (a leading '/' is optional and ignored) - e.g. "Documents/notes.txt".
   No ".." or "." support, and no symlinks - this is a small hobby FS. */
int  nfs_create_path(const char *path);                              /* creates an empty file, returns inode # or -1 */
int  nfs_mkdir_path(const char *path);                                /* creates a directory, returns inode # or -1 */
int  nfs_write_path(const char *path, const void *data, uint32_t len); /* creates the file if needed */
int  nfs_read_path(const char *path, void *buf, uint32_t maxlen);      /* -1 if not found */
int  nfs_delete_path(const char *path);                                /* files always; dirs only if empty */
bool nfs_rename_path(const char *dir_path, const char *old_name, const char *new_name);
/* Moves an entry from one directory to another (used by the Recycle Bin) - keeps the same inode/data. */
bool nfs_move_path(const char *src_dir_path, const char *name, const char *dst_dir_path);
bool nfs_is_dir_path(const char *path);
bool nfs_exists_path(const char *path);

/* Lists the contents of a directory (pass "" or "/" for the root). */
void nfs_list_path(const char *dir_path, void (*callback)(const char *name, uint32_t size, bool is_dir));

#endif
