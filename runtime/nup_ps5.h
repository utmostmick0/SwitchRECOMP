#ifndef NUP_PS5_H
#define NUP_PS5_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open a NUP container (format 1.0). Returns 1 on success, 0 on failure.
   Parses the superblock + table of contents into memory. The TOC SHA-256 is
   NOT verified yet (the container is a local, trusted dump); bounds are
   checked so a malformed file cannot make us read out of bounds. */
int nup_open(const char* path);
void nup_close(void);

/* Index of the base package's romfs directory root, or -1 if none.
   Falls back to the first package that has a romfs directory. */
int nup_base_romfs_dir(void);

/* Total number of nodes (directories + files) in the container. Useful for
   preallocating the RomFS index without touching the tiny PS5 CRT heap. */
int nup_node_count(void);

/* Size of the string pool (bytes) referenced by node names. */
uint64_t nup_pool_size(void);

/* Sum over every node of (name_len + 1). This is exactly how many bytes the
   RomFS name arena needs, so the caller can size it once instead of guessing
   (a too-small arena used to truncate names - see romfs_build_nup). */
uint64_t nup_name_bytes(void);

/* Directory tree traversal. A "node" is an index into the container's combined
   node space: 0 .. ndirs-1 are directories, ndirs .. ndirs+nfiles-1 are files. */
int  nup_dir_child_count(int dir_node);
int  nup_dir_child(int dir_node, int i);
int  nup_node_is_dir(int node);
/* Copy the node's name (NUL-terminated) into buf (at most n bytes). Returns the
   name length (excluding the terminator). */
int  nup_node_name(int node, char* buf, size_t n);

/* File extent info for a file node (>= ndirs). Returns 0 on success. */
int  nup_file_size(int file_node, uint64_t* size);
int  nup_file_extent(int file_node, uint64_t* extent_offset, uint64_t* extent_size, int* codec);

/* Read [offset, offset+size) of a file's *uncompressed* data into dst.
   Raw extents are read directly (pread into the blob); zstd extents are
   decompressed once and cached under an LRU with a memory cap.
   Returns 0 on success, -1 on error. */
int  nup_read_file(int file_node, uint64_t offset, void* dst, uint64_t size);

#ifdef __cplusplus
}
#endif
#endif /* NUP_PS5_H */
