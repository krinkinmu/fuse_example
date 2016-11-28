#ifndef __MTREE_H__
#define __MTREE_H__

#include <rbtree.h>


struct lsm;
struct lsm_key;
struct lsm_val;
struct mtree_node;


struct mtree {
	struct lsm *lsm;

	/* For now it's just a rb tree, but we probably need a persistent
	 * ordered map so that we can make snapshot of the structure so
	 * that iterations doesn't block concurrent updates. Of course
	 * assuming that the caller have taken all appropriate locks. For
	 * on-disk trees it happens naturally, we only need to make sure
	 * that on-disk tree won't be freed until we finished iteration. */
	struct rb_tree tree;
};

struct mtree_iter {
	struct lsm *lsm;
	struct rb_tree *tree;

	/* NULL means end */
	struct mtree_node *node;
};

void mtree_setup(struct mtree *tree, struct lsm *lsm);
void mtree_release(struct mtree *mtree);
void mtree_reset(struct mtree *mtree);
int mtree_is_empty(const struct mtree *mtree);
void mtree_swap(struct mtree *l, struct mtree *r);

void mtree_iter_setup(struct mtree_iter *iter, struct mtree *tree);
void mtree_iter_release(struct mtree_iter *iter);

int mtree_add(struct mtree *tree, const struct lsm_key *key,
			const struct lsm_val *val);

/* Returns positive value if lookup found required key. */
int mtree_lookup(struct mtree_iter *iter, const struct lsm_key *key);

void mtree_lower_bound(struct mtree_iter *iter, const struct lsm_key *key);
void mtree_upper_bound(struct mtree_iter *iter, const struct lsm_key *key);

void mtree_begin(struct mtree_iter *iter);
void mtree_end(struct mtree_iter *iter);

int mtree_next(struct mtree_iter *iter);
int mtree_prev(struct mtree_iter *iter);
int mtree_key(const struct mtree_iter *iter, struct lsm_key *key);
int mtree_val(const struct mtree_iter *iter, struct lsm_val *val);

#endif /*__MTREE_H__*/
