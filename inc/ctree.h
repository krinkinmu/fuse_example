#ifndef __CTREE_H__
#define __CTREE_H__

#include <stddef.h>
#include <stdint.h>


static const size_t MIN_FANOUT = 100;

struct lsm;
struct lsm_key;
struct lsm_val;

struct ctree_entry {
	size_t key_offs;
	size_t key_size;
	size_t val_offs;
	size_t val_size;
	int deleted;
};

struct ctree_node {
	char *buf;
	size_t bytes;
	size_t max_bytes;

	struct ctree_entry *entry;
	size_t entries;
	size_t max_entries;

	/* This will be set by ctree_node_write. */
	uint64_t csum;
};

struct ctree_builder {
	struct lsm *lsm;
};

void ctree_builder_setup(struct ctree_builder *builder, struct lsm *lsm);
void ctree_builder_release(struct ctree_builder *builder);

int ctree_node_setup(const struct lsm *lsm, struct ctree_node *node);
void ctree_node_reset(struct ctree_node *node);
void ctree_node_release(struct ctree_node *node);

size_t ctree_node_pages(const struct lsm *lsm, const struct ctree_node *node);
int ctree_node_can_append(const struct lsm *lsm, const struct ctree_node *node,
			size_t count, size_t size);
int ctree_node_append(const struct lsm *lsm, struct ctree_node *node,
			int deleted, const struct lsm_key *key,
			const struct lsm_val *val);
int ctree_node_write(struct lsm *lsm, struct ctree_node *node,
			uint64_t offs, int level);

#endif /*__CTREE_H__*/
