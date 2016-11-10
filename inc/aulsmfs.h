#ifndef __AULSMFS_H__
#define __AULSMFS_H__

#include <stdint.h>


#define AULSMFS_MAGIC	0x0A0153F5
#define AULSMFS_MAJOR	0
#define AULSMFS_MINOR	1
#define AULSMFS_VERSION	(((uint64_t)AULSMFS_MAJOR << 32) | (AULSMFS_MINOR))

#define AULSMFS_GET_MINOR(version) ((version) & 0xfffffffful)
#define AULSMFS_GET_MAJOR(version) ((version) >> 32)

#define AULSMFS_MAX_DISK_TREES	8


/* We are using little endian since it's native byte order
 * for the majority of existing architectures. */
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

/* Tree log maintains checksum per log record/chunk/etc, but instead of csum
 * we need a generation value, see explanation below. */
struct aulsmfs_tree_log {
	le64_t offs;
	le64_t size;
	le64_t gen;
} __attribute__((packed));

/* Log is a sequence of log entries, every log entry is page aligned and has a
 * "header", this structure describes footer format. */
struct aulsmfs_log_entry {
	/* We allocate large contigous range of disk space for our log, and if
	 * we haven't filled the whole log than there may be uninitialized
	 * areas, we need to differ uninitialized areas from corrupted entries.
	 * In order to do that we use autoincremented generation value. */
	le64_t gen;

	/* Bytes of user data in the log, since every log entry is page aligned,
	 * we need somehow to track how many bytes of user data are really in
	 * the log. */
	le64_t size;

	/* Checksum of the whole log entry with user data/paddings/etc. */
	le64_t csum;
} __attribute__((packed));

struct aulsmfs_tree {
	/* С0 is generally in-memory tree, but we log all C0 keys in a
	 * journal so that we don't need to merge C0 with the next tree
	 * to commit. */
	struct aulsmfs_tree_log c0_log;

	/* C1, like C0, is generally in-memory tree. This tree is used
	 * when we merge C0. We don't want to stop updates while we are
	 * merging C0 so we move C0 to C1 thus fixate C0 state in C1,
	 * and while we are merging C1 with the next tree we still can
	 * update C0. And while merge is still in progress we retain
	 * reference to the log. */
	struct aulsmfs_tree_log c1_log;

	/* These point on the root nodes of the B+ trees. Zero offset
	 * and size means that tree is empty. */
	struct aulsmfs_ptr ci[AULSMFS_MAX_DISK_TREES];
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
