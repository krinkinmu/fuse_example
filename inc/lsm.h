#ifndef __LSM_H__
#define __LSM_H__

#include <aulsmfs.h>
#include <lsm_fwd.h>
#include <mtree.h>
#include <ctree.h>
#include <alloc.h>
#include <io.h>

#include <stdint.h>
#include <stddef.h>


struct lsm {
	struct io *io;
	struct alloc *alloc;

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

void lsm_parse(struct lsm *lsm, const struct aulsmfs_tree *ondisk);
void lsm_dump(const struct lsm *lsm, struct aulsmfs_tree *ondisk);

int lsm_add(struct lsm *lsm, const struct lsm_key *key,
			const struct lsm_val *val);


struct lsm_iter {
	struct lsm *lsm;
	int from, to;

	struct mtree_iter it0;
	struct mtree_iter it1;
	struct ctree_iter iti[AULSMFS_MAX_DISK_TREES];

	struct lsm_key keyi[AULSMFS_MAX_DISK_TREES + 2];
	struct lsm_val vali[AULSMFS_MAX_DISK_TREES + 2];

	struct lsm_key key;
	struct lsm_val val;

	void *buf;
	size_t buf_size;
};

void lsm_iter_setup(struct lsm_iter *iter, struct lsm *lsm);
void lsm_iter_release(struct lsm_iter *iter);
int lsm_begin(struct lsm_iter *iter);
int lsm_end(struct lsm_iter *iter);
int lsm_next(struct lsm_iter *iter);
int lsm_prev(struct lsm_iter *iter);
int lsm_has_item(const struct lsm_iter *iter);


struct lsm_merge_state {
	struct lsm *lsm;
	struct lsm_iter iter;
	struct ctree_builder builder;

	int drop_deleted;
	int tree;

	int (*deleted)(struct lsm_merge_state *, const struct lsm_key *,
				const struct lsm_val *);
	int (*before_finish)(struct lsm_merge_state *);
	void (*after_finish)(struct lsm_merge_state *);
};

int lsm_merge(struct lsm *lsm, int tree, struct lsm_merge_state *state);


#endif /*__LSM_H__*/
