#ifndef __AULSMFS_H__
#define __AULSMFS_H__

#include <stdint.h>


#define AULSMFS_MAGIC	0x0A0153F5
#define AULSMFS_MAJOR	0
#define AULSMFS_MINOR	1
#define AULSMFS_VERSION	(((uint64_t)AULSMFS_MAJOR << 32) | (AULSMFS_MINOR))

#define AULSMFS_GET_MINOR(version) ((version) & 0xfffffffful)
#define AULSMFS_GET_MAJOR(version) ((version) >> 32)

#define AULSMFS_MAX_DISK_TREES	3


/* We are using little endian since it's native byte order
 * for the majority of existing architectures. */
typedef uint8_t		le8_t;	/* just for the sake of consistency */
typedef uint16_t	le16_t;
typedef uint32_t	le32_t;
typedef uint64_t	le64_t;


/* All offsets, sizes and so on are always given in pages unless
 * explicitly stated otherwise. Zero offset and size means a "NULL"
 * pointer. */
struct aulsmfs_ptr {
	le64_t offs;
	le64_t size;
	le64_t csum;
} __attribute__((packed));

struct aulsmfs_log_entry {
	le16_t size;
} __attribute__((packed));

/* For every transaction we have log header. Every log header contains array of
 * aulsmfs_ptr structures that points to chunks. For small transactions we would
 * have only one such chunk (so it's a bit ineffective), for large transactions
 * we could have more.
 * Each transaction can be in one of two states: registered and replayed.
 * Registered transaction could be not fully replayed, so we can't remove it.
 * Replayed transaction can be removed after next merge of all the trees.
 * All in all transactions work as follows, for every operation we at first
 * create transaction log, then we register it and then we replay it and mark
 * it as replayed. Perioadically we merge all memory trees and after we merged
 * them we can make commit and drop replayed transactions.
 */
struct aulsmfs_log_header {
	le32_t chunks;
	le32_t pages;
} __attribute__((packed));

/* Both key size and value size are given in bytes. */
struct aulsmfs_node_entry {
	le16_t key_size;
	le16_t val_size;
} __attribute__((packed));

struct aulsmfs_node_header {
	/* How many bytes this tree node really contains. */
	le64_t size;
	/* Level of this node in the tree, 0 - leaf node. */
	le64_t level;
} __attribute__((packed));

struct aulsmfs_ctree {
	struct aulsmfs_ptr ptr;
	le32_t pages;
	le32_t height;
};

struct aulsmfs_tree {
	/* These point on the root nodes of the B+ trees. Zero offset
	 * and size means that tree is empty. */
	struct aulsmfs_ctree ci[AULSMFS_MAX_DISK_TREES];
} __attribute__((packed));

struct aulsmfs_super {
	le64_t magic;
	le64_t version;
	le64_t page_size;
	le64_t pages;

	/* Stores information about used blocks. Information includes
	 * snapshot id when block has been allocated and snapshot id
	 * when block has been freed (if it has been freed at all).
	 *
	 * Space used by rootmap and blockmap itself is a special case
	 * since these maps are shared between snapshots and so should
	 * be mangaed in a bit different way, so we are going to mark
	 * somehow space used by blockmap and rootmap in the blockmap. */
	struct aulsmfs_tree blockmap;

	/* Stores all the roots (snapshots) of the filesystem, root
	 * with the largest id is current root of the filesystem. */
	struct aulsmfs_tree rootmap;

	struct aulsmfs_ptr registered_logs;
	struct aulsmfs_ptr replayed_logs;

	le64_t csum;
} __attribute__((packed));

struct aulsmfs_root {
	le64_t id;

	/* Maps parent inode id and name to child inode id. */
	struct aulsmfs_tree namemap;

	/* Maps node id to metadata (size, flags, owner, blocks of
	 * file, and so on). */
	struct aulsmfs_tree nodemap;

	/* Just a set of nodes to be deleted, we add node id to the set
	 * when nlink reaches zero but file/dir is stil open so that we
	 * can actually delete it later. */
	struct aulsmfs_tree todelmap;
} __attribute__((packed));

/* We can put more information here, for example additional metadata that
 * says what is this extent used for and/or checksum so that we can check
 * filesystem consistency just traversing blockmap once, though we hardly
 * could fix something if we found an inconsistency. */
struct aulsmfs_used_extent {
	le64_t offs;
	le64_t size;
	le64_t allocated;
	le64_t released;
} __attribute__((packed));

struct aulsmfs_node {
	le64_t id;
	le64_t uid;
	le64_t gid;
	le64_t perm;

	/* Dir/regular file/etc. */
	le64_t type;
	/* Number of active references. */
	le64_t nlink;
	/* Size given in bytes/entries. */
	le64_t size;
} __attribute__((packed));

struct aulsmfs_delayed_node {
	le64_t id;
} __attribute__((packed));

struct aulsmfs_entry {
	le64_t parent_id;
	le64_t child_id;

	le16_t size;
	char name[1];
} __attribute__((packed));

#endif /*__AULSMFS_H__*/
