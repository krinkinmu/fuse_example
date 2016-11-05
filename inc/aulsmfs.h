#ifndef __AULSMFS_H__
#define __AULSMFS_H__

#include <stdint.h>


#define AULSMFS_MAGIC	0x0A0153F5
#define AULSMFS_MAJOR	0
#define AULSMFS_MINOR	1
#define AULSMFS_VERSION	(((uint64_t)AULSMFS_MAJOR << 32) | (AULSMFS_MINOR))

#define AULSMFS_GET_MINOR(version) ((version) & 0xfffffffful)
#define AULSMFS_GET_MAJOR(version) ((version) >> 32)


/* We are using little endian since it's native byte order
 * for the majority of existing architectures. */
typedef uint8_t		le8_t;
typedef uint16_t	le16_t;
typedef uint32_t	le32_t;
typedef uint64_t	le64_t;


/* All offsets, sizes and so on are always given in pages unless
 * explicitly stated otherwise. */
struct aulsmfs_ptr {
	le64_t offs;
	le64_t size;
	le64_t csum;
} __attribute__((packed));

struct aulsmfs_super {
	le64_t magic;
	le64_t version;
	le64_t page_size;
	le64_t pages;

	/* Points at the "root" of the filesystem */
	struct aulsmfs_ptr root;

	le64_t csum;
} __attribute__((packed));

struct aulsmfs_root {
	le64_t dummy;
} __attribute__((packed));

#endif /*__AULSMFS_H__*/
