#ifndef __LSM_H__
#define __LSM_H__

#include <aulsmfs.h>
#include <mtree.h>
#include <ctree.h>
#include <io.h>

#include <stdint.h>
#include <stddef.h>


struct lsm;

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
	int (*reserve)(struct lsm *, uint64_t /* size */,
				uint64_t * /* returned offset */);
	int (*persist)(struct lsm *, uint64_t /* offset */,
				uint64_t /* size */);
	void (*cancel)(struct lsm *, uint64_t /* offset */,
				uint64_t /* size */);
	int (*free)(struct lsm *, uint64_t /* offset */,
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

static inline int lsm_reserve(struct lsm *lsm, uint64_t size, uint64_t *offs)
{
	return lsm->alloc->reserve(lsm, size, offs);
}

static inline int lsm_persist(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	return lsm->alloc->persist(lsm, offs, size);
}

static inline void lsm_cancel(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	lsm->alloc->cancel(lsm, offs, size);
}

static inline int lsm_free(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	return lsm->alloc->free(lsm, offs, size);
}

#endif /*__LSM_H__*/
