/* Game file access.
 *
 * Files are looked up (case-insensitively) in the save overlay, then in the
 * loose game folder, then in the pack (game.pak), whose entries are decoded
 * on the fly back to the exact original bytes:
 *   - plain files: LZMA blocks, with 16-bit sound data stored as FLAC
 *   - movies (.ECM): decoded video + recon records, rebuilt chunk by chunk
 *   - music: FLAC tracks, and near-duplicate tracks stored as edits of another */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef struct VFile VFile;

/* Opens the pack at `path`, whose data starts `base` bytes into the file
 * (non-zero when it is stored inside the APK).  Returns 0 on success. */
int vfs_pack_open(const char *path, uint64_t base);
int vfs_have_pack(void);
/* Locates an uncompressed member of a zip (APK); returns 0 and its data offset/size. */
int vfs_zip_find_stored(const char *zip, const char *name, uint64_t *off, uint64_t *size);

/* rel: '/'-separated path relative to the game root.  Returns NULL if missing. */
VFile *vfs_open(const char *rel);
/* Wraps an already opened stdio file (read/write); vfs_close closes it. */
VFile *vfs_from_stdio(FILE *fp);
/* 0 if rel exists; *is_dir set accordingly. */
int vfs_stat(const char *rel, int *is_dir, int64_t *size);
/* Case-insensitive lookup of rel below root; writes the real path into out. */
int vfs_ci_lookup(const char *root, const char *rel, char *out, size_t n);
/* Lists a directory (overlay, loose folder and pack merged); returns -1 if missing. */
int vfs_list_dir(const char *rel, void (*cb)(void *ctx, const char *name, int is_dir), void *ctx);
/* Real path of a loose (non-pack) file, for code that needs one. */
int vfs_loose_path(const char *rel, char *out, size_t outsz);

size_t vfs_read(VFile *f, void *buf, size_t n);
size_t vfs_write(VFile *f, const void *buf, size_t n);
int64_t vfs_seek(VFile *f, int64_t off, int whence);  /* returns new position or -1 */
int64_t vfs_tell(VFile *f);
int64_t vfs_size(VFile *f);
FILE *vfs_stdio(VFile *f);                             /* NULL unless stdio-backed */
void vfs_close(VFile *f);
