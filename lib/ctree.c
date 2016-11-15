#include <ctree.h>
#include <crc64.h>
#include <lsm.h>
#include <io.h>

#include <endian.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>


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
	uint64_t offs;
	uint64_t size;
	uint64_t csum;
	int level;
};


static const size_t MIN_FANOUT = 100;


static int __ctree_node_setup(const struct lsm *lsm, struct ctree_node *node,
			size_t size, size_t count)
{
	const size_t page_size = lsm->io->page_size;

	assert(size % page_size == 0);

	memset(node, 0, sizeof(*node));

	node->buf = malloc(size);
	if (!node->buf)
		return -ENOMEM;

	node->entry = calloc(count, sizeof(*node->entry));
	if (!node->entry) {
		free(node->buf);
		return -ENOMEM;
	}

	node->max_bytes = size;
	node->max_entries = count;
	return 0;
}

static void ctree_node_reset(struct ctree_node *node)
{
	assert(node->buf);
	assert(node->max_bytes >= sizeof(struct aulsmfs_node_header));

	memset(node->entry, 0, node->entries * sizeof(*node->entry));
	node->entries = 0;

	memset(node->buf, 0, node->bytes);
	node->bytes = sizeof(struct aulsmfs_node_header);
}

static int ctree_node_setup(const struct lsm *lsm, struct ctree_node *node)
{
	const size_t page_size = lsm->io->page_size;
	const int rc = __ctree_node_setup(lsm, node,
				/* size */ 4096 < page_size ? page_size : 4096,
				/* count */ MIN_FANOUT);

	if (rc < 0)
		return rc;
	ctree_node_reset(node);
	return 0;
}

static void ctree_node_release(struct ctree_node *node)
{
	free(node->buf);
	free(node->entry);
	memset(node, 0, sizeof(*node));
}

static size_t ctree_node_pages(const struct lsm *lsm,
			const struct ctree_node *node)
{
	const size_t page_size = lsm->io->page_size;

	return (node->bytes + page_size - 1) / page_size;
}

static int ctree_node_can_append(const struct lsm *lsm,
			const struct ctree_node *node,
			size_t count, size_t size)
{
	const size_t bytes = count * sizeof(struct aulsmfs_node_entry) + size;
	const size_t page_size = lsm->io->page_size;
	const size_t page_mask = page_size - 1;

	if (node->entries + count <= MIN_FANOUT)
		return 1;

	if (((node->bytes + bytes) & ~page_mask) == (node->bytes & ~page_mask))
		return 1;

	return 0;
}

static int ctree_ensure_entries(const struct lsm *lsm, struct ctree_node *node,
			size_t count, size_t size)
{
	const size_t page_size = lsm->io->page_size;
	const size_t page_mask = page_size - 1;
	const size_t entries = node->entries + count;
	const size_t bytes = (node->bytes + size + page_mask) & ~page_mask;

	if (node->max_entries < entries) {
		const size_t prev_entries = node->max_entries;
		const size_t next_entries = prev_entries * 2 < entries
					? prev_entries * 2 : entries;
		const size_t new = next_entries - prev_entries;

		struct ctree_entry *entry = realloc(node->entry,
					next_entries * sizeof(*node->entry));

		if (!entry)
			return -ENOMEM;

		memset(entry + prev_entries, 0, new * sizeof(*node->entry));
		node->entry = entry;
		node->max_entries = next_entries;
	}

	if (node->max_bytes < bytes) {
		const size_t prev_bytes = node->max_bytes;
		const size_t next_bytes = prev_bytes * 2 < bytes
					? prev_bytes * 2 : bytes;
		const size_t new = next_bytes - prev_bytes;
		char *buf = realloc(node->buf, next_bytes);

		if (!buf)
			return -ENOMEM;

		memset(buf + prev_bytes, 0, new);
		node->buf = buf;
		node->max_bytes = next_bytes;
	}

	return 0;
}

static int ctree_node_append(const struct lsm *lsm, struct ctree_node *node,
			int deleted, const struct lsm_key *key,
			const struct lsm_val *val)
{
	struct aulsmfs_node_entry nentry;
	const size_t size = key->size + val->size + sizeof(nentry);
	const int rc = ctree_ensure_entries(lsm, node, 1, size);

	if (rc < 0)
		return rc;

	char *nentry_ptr = node->buf + node->bytes;
	char *key_ptr = nentry_ptr + sizeof(nentry);
	char *val_ptr = key_ptr + key->size;

	nentry.key_size = htole16(key->size);
	nentry.val_size = htole16(val->size);
	nentry.deleted = deleted ? 1 : 0;

	memcpy(nentry_ptr, &nentry, sizeof(nentry));
	memcpy(key_ptr, key->ptr, key->size);
	memcpy(val_ptr, val->ptr, val->size);

	struct ctree_entry *centry = &node->entry[node->entries];

	centry->key_offs = key_ptr - node->buf;
	centry->key_size = key->size;

	centry->val_offs = val_ptr - node->buf;
	centry->val_size = val->size;

	centry->deleted = deleted ? 1 : 0;

	node->bytes += size;
	++node->entries;
	return 0;
}

static int ctree_node_write(struct lsm *lsm, struct ctree_node *node,
			uint64_t offs, int level)
{
	const size_t size = ctree_node_pages(lsm, node);
	struct aulsmfs_node_header *header = (void *)node->buf;

	header->size = htole64(node->bytes);
	header->level = htole64(level);

	const int rc = io_write(lsm->io, node->buf, size, offs);

	if (rc < 0)
		return rc;

	node->offs = offs;
	node->size = size;
	node->level = level;
	node->csum = crc64(node->buf, size * lsm->io->page_size);
	return 0;
}


void ctree_builder_setup(struct ctree_builder *builder, struct lsm *lsm)
{
	memset(builder, 0, sizeof(*builder));
	builder->lsm = lsm;
}

void ctree_builder_release(struct ctree_builder *builder)
{
	for (int i = 0; i != builder->nodes; ++i)
		ctree_node_release(&builder->node[i]);
	free(builder->node);
	memset(builder, 0, sizeof(*builder));
}

static int __ctree_builder_append(struct ctree_builder *builder, int level,
			int deleted, const struct lsm_key *key,
			const struct lsm_val *val);

static struct ctree_node *ctree_builder_node(
			const struct ctree_builder *builder,
			int level)
{
	return &builder->node[level];
}

static int ctree_builder_flush(struct ctree_builder *builder, int level)
{
	struct lsm * const lsm = builder->lsm;
	struct ctree_node *node = ctree_builder_node(builder, level);
	uint64_t size = ctree_node_pages(lsm, node);
	uint64_t offs;
	int rc;

	if (!node->entries)
		return 0;

	rc = lsm_reserve(lsm, size, &offs);
	if (rc < 0)
		return rc;

	rc = ctree_node_write(lsm, node, offs, level);
	if (rc < 0)
		return rc;

	struct aulsmfs_ptr ptr = {
		.offs = htole64(offs),
		.size = htole64(size),
		.csum = htole64(node->csum)
	};

	const struct lsm_key key = {
		.ptr = node->buf + node->entry[0].key_offs,
		.size = node->entry[0].key_size
	};
	const struct lsm_val val = {
		.ptr = &ptr,
		.size = sizeof(ptr)
	};

	rc = __ctree_builder_append(builder, level + 1, 0, &key, &val);
	if (rc < 0)
		return rc;

	ctree_node_reset(node);
	return 0;
}

static int ctree_builder_ensure_level(struct ctree_builder *builder, int level)
{
	const struct lsm * const lsm = builder->lsm;

	if (level < builder->nodes)
		return 0;

	if (level >= builder->max_nodes) {
		const int max_nodes = builder->max_nodes * 2 < level + 1
					? level + 1 : builder->max_nodes * 2;
		struct ctree_node *node = realloc(builder->node,
					max_nodes * sizeof(*node));

		if (!node)
			return -ENOMEM;
		builder->node = node;
		builder->max_nodes = max_nodes;
	}

	for (int i = builder->nodes; i <= level; ++i) {
		struct ctree_node *node = ctree_builder_node(builder, i);
		const int rc = ctree_node_setup(lsm, node);

		if (rc < 0)
			return rc;
		++builder->nodes;
	}
	return 0;
}

static int ctree_builder_can_append(const struct ctree_builder *builder,
			int level, size_t count, size_t size)
{
	const struct lsm *lsm = builder->lsm;
	const struct ctree_node *node = ctree_builder_node(builder,
				level);

	return ctree_node_can_append(lsm, node, count, size);
}

static int __ctree_builder_append(struct ctree_builder *builder, int level,
			int deleted, const struct lsm_key *key,
			const struct lsm_val *val)
{
	struct lsm * const lsm = builder->lsm;
	const size_t size = key->size + val->size;
	const int rc = ctree_builder_ensure_level(builder, level);

	if (rc < 0)
		return rc;

	if (!ctree_builder_can_append(builder, level, 1, size)) {
		const int rc = ctree_builder_flush(builder, level);

		if (rc < 0)
			return rc;
	}

	struct ctree_node *node = ctree_builder_node(builder, level);

	assert(ctree_node_can_append(lsm, node, 1, size));
	return ctree_node_append(lsm, node, deleted, key, val);
}

int ctree_builder_append(struct ctree_builder *builder, int deleted,
			const struct lsm_key *key, const struct lsm_val *val)
{
	return __ctree_builder_append(builder, 0, deleted, key, val);
}

int ctree_builder_finish(struct ctree_builder *builder)
{
	struct lsm * const lsm = builder->lsm;
	int level = 0;
	int rc;

	while (level < builder->nodes - 1) {
		rc = ctree_builder_flush(builder, level);
		if (rc < 0)
			return rc;
		++level;
	}

	/* We have written all but last level, the node will be root of the
	 * ctree. */
	struct ctree_node *root = ctree_builder_node(builder, level);
	const size_t size = ctree_node_pages(lsm, root);
	uint64_t offs;

	rc = lsm_reserve(lsm, size, &offs);
	if (rc < 0)
		return rc;

	rc = ctree_node_write(lsm, root, offs, level);
	if (rc < 0)
		return rc;

	builder->offs = offs;
	builder->size = size;
	builder->csum = root->csum;
	builder->hight = level + 1;
	ctree_node_reset(root);
	return 0;
}
