#ifndef __CTREE_H__
#define __CTREE_H__

#include <aulsmfs.h>

#include <stddef.h>
#include <stdint.h>


struct lsm;
struct lsm_key;
struct lsm_val;
struct ctree_node;

struct range {
	uint64_t begin;
	uint64_t end;
};

struct ctree_builder {
	struct lsm *lsm;

	struct ctree_node **node;
	int nodes;
	int max_nodes;

	/* These will be set by ctree_builder_finish. */
	struct range *reserved;
	size_t ranges;
	size_t max_ranges;

	struct aulsmfs_ptr ptr;
	int height;
};

void ctree_builder_setup(struct ctree_builder *builder, struct lsm *lsm);
void ctree_builder_release(struct ctree_builder *builder);
int ctree_builder_append(struct ctree_builder *builder,
			const struct lsm_key *key, const struct lsm_val *val);
int ctree_builder_finish(struct ctree_builder *builder);
void ctree_builder_cancel(struct ctree_builder *builder);


struct ctree {
	struct lsm *lsm;

	struct aulsmfs_ptr ptr;
	int height;
};

void ctree_setup(struct ctree *ctree, struct lsm *lsm);
void ctree_release(struct ctree *ctree);
void ctree_parse(struct ctree *ctree, const struct aulsmfs_ctree *ondisk);
void ctree_dump(const struct ctree *ctree, struct aulsmfs_ctree *ondisk);


#define CTREE_ITER_INLINE_HEIGHT	16

struct ctree_iter {
	struct lsm *lsm;

	struct aulsmfs_ptr ptr;
	int height;

	struct ctree_node **node;
	size_t *pos;

	struct ctree_node *_node[CTREE_ITER_INLINE_HEIGHT];
	size_t _pos[CTREE_ITER_INLINE_HEIGHT];
};

void ctree_iter_setup(struct ctree_iter *iter, struct ctree *ctree);
void ctree_iter_release(struct ctree_iter *iter);

int ctree_lookup(struct ctree_iter *iter, const struct lsm_key *key);
int ctree_lower_bound(struct ctree_iter *iter, const struct lsm_key *key);
int ctree_upper_bound(struct ctree_iter *iter, const struct lsm_key *key);
int ctree_first(struct ctree_iter *iter);

int ctree_begin(const struct ctree_iter *iter);
int ctree_end(const struct ctree_iter *iter);
int ctree_equal(const struct ctree_iter *l, const struct ctree_iter *r);
int ctree_next(struct ctree_iter *iter);
int ctree_prev(struct ctree_iter *iter);

void ctree_key(const struct ctree_iter *iter, struct lsm_key *key);
void ctree_val(const struct ctree_iter *iter, struct lsm_val *val);
int ctree_deleted(const struct ctree_iter *iter);

#endif /*__CTREE_H__*/
