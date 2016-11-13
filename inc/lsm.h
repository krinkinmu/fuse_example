#ifndef __LSM_H__
#define __LSM_H__

#include <aulsmfs.h>
#include <mtree.h>
#include <io.h>

#include <stdint.h>
#include <stddef.h>


struct lsm;

struct ctree {
	/* For now just root node offset and size, if size == 0 then tree is
	 * empty. */
	uint64_t offs, size;
};

struct lsm_key {
	void *ptr;
	size_t size;
};

struct lsm_val {
	void *ptr;
	size_t size;
};


struct lsm_alloc {
	/* Two phase allocation routines:
	 *  - reserve space allocates free space and reserves it, but never
	 *    writes anything, so reservation should be done purely in memory
	 *  - persist marks previously reserved range as busy (commits)
	 * cancel just cancels reservation, for example if we allocated too
	 * much, we can release unused space. */
	int (*reserve)(struct lsm_alloc *, uint64_t /* size */,
				uint64_t * /* returned offset */);
	int (*persist)(struct lsm_alloc *, uint64_t /* offset */,
				uint64_t /* size */);
	void (*cancel)(struct lsm_alloc *, uint64_t /* offset */,
				uint64_t /* size */);
};

struct lsm {
	struct io *io;
	struct lsm_alloc *alloc;

	/* Key comparision function. */
	int (*cmp)(const struct lsm_key *, const struct lsm_key *);

	/* Two in memory trees, all inserts/deletes go to c0, c1 is a temporary
	 * tree that contains fixed state of c0 during merge. */
	struct mtree c0;
	struct mtree c1;

	/* Descriptors of disk trees. */
	struct ctree ci[AULSMFS_MAX_DISK_TREES];
};

inline int lsm_reserve(struct lsm *lsm, uint64_t size, uint64_t *offs)
{
	return lsm->alloc->reserve(lsm->alloc, size, offs);
}

inline int lsm_persist(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	return lsm->alloc->persist(lsm->alloc, offs, size);
}

inline void lsm_cancel(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	lsm->alloc->cancel(lsm->alloc, offs, size);
}

int lsm_add(struct lsm *lsm, const struct lsm_key *key,
			const struct lsm_val *val);
int lsm_del(struct lsm *lsm, const struct lsm_key *key);


/* Structure points in the buffer inside lsm_node structure, so we can't use
 * lsm_key and lsm_val since we might need to resize buffer and render pointers
 * invalid. */
struct lsm_entry_pos {
	size_t key_pos;
	size_t key_size;
	size_t val_pos;
	size_t val_size;
};

struct lsm_node {
	struct lsm *lsm;

	struct lsm_entry_pos *entry;
	size_t entries;
	size_t capacity;

	char *data;
	size_t data_size;
	size_t data_capacity;

	uint64_t csum;

	/* These three values must be set before writing node by caller. */
	uint64_t offs;
	uint64_t size;
	int level;

	char _data[1];
};

struct lsm_merge_state {
	struct lsm_node **node;
	size_t levels, capacity;
	struct lsm_node *_node[16];
};

#endif /*__LSM_H__*/
