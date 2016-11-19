#ifndef __LSM_H__
#define __LSM_H__

#include <aulsmfs.h>
#include <mtree.h>
#include <ctree.h>
#include <alloc.h>
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


struct lsm {
	struct io *io;
	struct alloc *alloc;

	/* Key comparision function. */
	int (*cmp)(const struct lsm_key *, const struct lsm_key *);

	struct aulsmfs_ptr ptr;

	/* Two in memory trees, all inserts/deletes go to c0, c1 is a temporary
	 * tree that contains fixed state of c0 during merge. */
	struct mtree c0;
	struct mtree c1;

	/* Descriptors of disk trees. */
	struct ctree ci[AULSMFS_MAX_DISK_TREES];
};

static inline int lsm_reserve(struct lsm *lsm, uint64_t size, uint64_t *offs)
{
	return alloc_reserve(lsm->alloc, size, offs);
}

static inline int lsm_persist(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	return alloc_commit(lsm->alloc, offs, size);
}

static inline int lsm_cancel(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	return alloc_cancel(lsm->alloc, offs, size);
}

static inline int lsm_free(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	return alloc_free(lsm->alloc, offs, size);
}

void lsm_setup(struct lsm *lsm, struct io *io, struct alloc *alloc,
		int (*cmp)(const struct lsm_key *, const struct lsm_key *));
void lsm_release(struct lsm *lsm);

int lsm_read(struct lsm *lsm, struct aulsmfs_ptr ptr);
int lsm_write(struct lsm *lsm);

int lsm_add(struct lsm *lsm, const struct lsm_key *key,
			const struct lsm_val *val);

#endif /*__LSM_H__*/
