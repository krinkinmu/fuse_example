#ifndef __CTREE_H__
#define __CTREE_H__

#include <stddef.h>
#include <stdint.h>


struct lsm;
struct lsm_key;
struct lsm_val;
struct ctree_node;

struct ctree_builder {
	struct lsm *lsm;

	struct ctree_node *node;
	int nodes;
	int max_nodes;

	/* These will be set by ctree_builder_finish. */
	uint64_t offs;
	uint64_t size;
	uint64_t csum;
	int hight;
};

void ctree_builder_setup(struct ctree_builder *builder, struct lsm *lsm);
void ctree_builder_release(struct ctree_builder *builder);
int ctree_builder_append(struct ctree_builder *builder, int deleted,
			const struct lsm_key *key, const struct lsm_val *val);
int ctree_builder_finish(struct ctree_builder *builder);

#endif /*__CTREE_H__*/
