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
};

struct ctree_node {
	struct lsm *lsm;

	char *buf;
	size_t bytes;
	size_t max_bytes;

	struct ctree_entry *entry;
	size_t entries;
	size_t max_entries;

	/* This will be set by ctree_node_write. */
	struct aulsmfs_ptr ptr;
	int level;
};


static const size_t MIN_FANOUT = 100;


static int __ctree_node_setup(struct lsm *lsm, struct ctree_node *node,
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
		node->buf = NULL;
		return -ENOMEM;
	}

	node->lsm = lsm;
	node->max_bytes = size;
	node->max_entries = count;
	return 0;
}

static void ctree_node_reset(struct ctree_node *node)
{
	assert(node->buf);
	assert(node->max_bytes >= sizeof(struct aulsmfs_node_header));

	memset(node->entry, 0, node->max_entries * sizeof(*node->entry));
	node->entries = 0;

	memset(node->buf, 0, node->max_bytes);
	node->bytes = sizeof(struct aulsmfs_node_header);
}

static int ctree_node_setup(struct lsm *lsm, struct ctree_node *node)
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

static struct ctree_node *ctree_node_create(struct lsm *lsm)
{
	struct ctree_node *node = malloc(sizeof(*node));

	memset(node, 0, sizeof(*node));
	node->lsm = lsm;
	return node;
}

static void ctree_node_destroy(struct ctree_node *node)
{
	if (node) {
		ctree_node_release(node);
		free(node);
	}
}

static int ctree_node_parse(struct ctree_node *node)
{
	const struct aulsmfs_node_header *header = (void *)node->buf;
	const int level = le64toh(header->level);
	const size_t bytes = le64toh(header->size);
	const size_t pages = le64toh(node->ptr.size);

	if (bytes > pages * node->lsm->io->page_size)
		return -EIO;

	if (level != node->level)
		return -EIO;

	size_t offs = sizeof(*header);

	while (offs != bytes) {
		struct aulsmfs_node_entry entry;

		if (offs + sizeof(entry) > bytes)
			return -EIO;

		memcpy(&entry, node->buf + offs, sizeof(entry));

		const size_t key_size = le16toh(entry.key_size);
		const size_t val_size = le16toh(entry.val_size);

		if (offs + sizeof(entry) + key_size + val_size > bytes)
			return -EIO;

		if (node->entries == node->max_entries) {
			const size_t entries = node->max_entries
						? node->max_entries * 2
						: MIN_FANOUT;
			struct ctree_entry *ptr = realloc(node->entry,
						sizeof(*ptr) * entries);

			if (!ptr)
				return -ENOMEM;

			node->max_entries = entries;
			node->entry = ptr;
		}

		struct ctree_entry *ptr = &node->entry[node->entries++];

		offs += sizeof(entry);

		ptr->key_offs = offs;
		ptr->key_size = key_size;
		offs += key_size;

		ptr->val_offs = offs;
		ptr->val_size = val_size;
		offs += val_size;
	}
	return 0;
}

static int ctree_node_read(struct ctree_node *node,
			const struct aulsmfs_ptr *ptr, int level)
{
	const struct lsm *lsm = node->lsm;
	const size_t page_size = lsm->io->page_size;
	const size_t pages = le64toh(ptr->size);
	const size_t buf_size = pages * page_size;	
	void *buf = malloc(buf_size);
	int rc;

	if (!buf)
		return -ENOMEM;

	node->buf = buf;
	node->max_bytes = buf_size;
	rc = io_read(lsm->io, buf, pages, le64toh(ptr->offs));
	if (rc < 0)
		return rc;

	if (crc64(buf, buf_size) != le64toh(ptr->csum))
		return -EIO;

	node->ptr = *ptr;
	node->level = level;
	rc = ctree_node_parse(node);
	if (rc < 0)
		return rc;
	return 0;
}

static size_t ctree_node_pages(const struct lsm *lsm,
			const struct ctree_node *node)
{
	const size_t page_size = lsm->io->page_size;

	return (node->bytes + page_size - 1) / page_size;
}

static void ctree_node_key(const struct ctree_node *node, size_t pos,
			struct lsm_key *key)
{
	key->ptr = node->buf + node->entry[pos].key_offs;
	key->size = node->entry[pos].key_size;
}

static void ctree_node_val(const struct ctree_node *node, size_t pos,
			struct lsm_val *val)
{
	val->ptr = node->buf + node->entry[pos].val_offs;
	val->size = node->entry[pos].val_size;
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
			const struct lsm_key *key, const struct lsm_val *val)
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

	memcpy(nentry_ptr, &nentry, sizeof(nentry));
	memcpy(key_ptr, key->ptr, key->size);
	memcpy(val_ptr, val->ptr, val->size);

	struct ctree_entry *centry = &node->entry[node->entries];

	centry->key_offs = key_ptr - node->buf;
	centry->key_size = key->size;

	centry->val_offs = val_ptr - node->buf;
	centry->val_size = val->size;

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

	node->ptr.offs = htole64(offs);
	node->ptr.size = htole64(size);
	node->ptr.csum = htole64(crc64(node->buf, size * lsm->io->page_size));
	node->level = level;
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
		ctree_node_destroy(builder->node[i]);
	free(builder->node);
	free(builder->reserved);
	memset(builder, 0, sizeof(*builder));
}

static int ctree_builder_alloc(struct ctree_builder *builder, uint64_t size,
			uint64_t *offs)
{
	if (builder->ranges == builder->max_ranges) {
		const size_t ranges = builder->ranges
					? builder->ranges * 2 : 16;
		struct range *range = realloc(builder->reserved,
					ranges * sizeof(*range));

		if (!range)
			return -ENOMEM;

		builder->reserved = range;
		builder->max_ranges = ranges;
	}

	const int rc = lsm_reserve(builder->lsm, size, offs);

	if (rc < 0)
		return rc;

	if (builder->ranges) {
		struct range *last = &builder->reserved[builder->ranges - 1];

		if (last->end == *offs) {
			last->end = *offs + size;
			return 0;
		}
	}

	struct range *new = &builder->reserved[builder->ranges++];

	new->begin = *offs;
	new->end = *offs + size;
	return 0;
}

static void ctree_builder_free(struct ctree_builder *builder, uint64_t offs,
			uint64_t size)
{
	lsm_cancel(builder->lsm, offs, size);
}

static int __ctree_builder_append(struct ctree_builder *builder, int level,
			const struct lsm_key *key, const struct lsm_val *val);

static struct ctree_node *ctree_builder_node(
			const struct ctree_builder *builder,
			int level)
{
	return builder->node[level];
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

	rc = ctree_builder_alloc(builder, size, &offs);
	if (rc < 0)
		return rc;

	rc = ctree_node_write(lsm, node, offs, level);
	if (rc < 0)
		return rc;

	const struct lsm_val val = {
		.ptr = &node->ptr,
		.size = sizeof(node->ptr)
	};
	struct lsm_key key;

	ctree_node_key(node, 0, &key);
	rc = __ctree_builder_append(builder, level + 1, &key, &val);
	if (rc < 0)
		return rc;

	ctree_node_reset(node);
	return 0;
}

static int ctree_builder_ensure_level(struct ctree_builder *builder, int level)
{
	struct lsm * const lsm = builder->lsm;

	if (level < builder->nodes)
		return 0;

	if (level >= builder->max_nodes) {
		const int max_nodes = builder->max_nodes * 2 < level + 1
					? level + 1 : builder->max_nodes * 2;
		struct ctree_node **node = realloc(builder->node,
					max_nodes * sizeof(*node));

		if (!node)
			return -ENOMEM;
		builder->node = node;
		builder->max_nodes = max_nodes;
	}

	for (int i = builder->nodes; i <= level; ++i) {
		struct ctree_node *node = ctree_node_create(lsm);
		const int rc = ctree_node_setup(lsm, node);

		if (rc < 0) {
			ctree_node_destroy(node);
			return rc;
		}
		builder->node[builder->nodes++] = node;
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
			const struct lsm_key *key, const struct lsm_val *val)
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

	return ctree_node_append(lsm, node, key, val);
}

int ctree_builder_append(struct ctree_builder *builder,
			const struct lsm_key *key, const struct lsm_val *val)
{
	return __ctree_builder_append(builder, 0, key, val);
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

	rc = ctree_builder_alloc(builder, size, &offs);
	if (rc < 0)
		return rc;

	rc = ctree_node_write(lsm, root, offs, level);
	if (rc < 0)
		return rc;

	builder->ptr = root->ptr;
	builder->height = level + 1;
	ctree_node_reset(root);
	return 0;
}

void ctree_builder_cancel(struct ctree_builder *builder)
{
	for (size_t i = 0; i != builder->ranges; ++i) {
		struct range *range = &builder->reserved[i];
		const uint64_t offs = range->begin;
		const uint64_t size = range->end - range->begin;

		ctree_builder_free(builder, offs, size);
	}
}


void ctree_setup(struct ctree *ctree, struct lsm *lsm)
{
	memset(ctree, 0, sizeof(*ctree));
	ctree->lsm = lsm;
}

void ctree_release(struct ctree *ctree)
{
	memset(ctree, 0, sizeof(*ctree));
}

int ctree_is_empty(const struct ctree *ctree)
{
	return ctree->height ? 0 : 1;
}

void ctree_swap(struct ctree *l, struct ctree *r)
{
	const struct ctree tmp = *l;

	*l = *r;
	*r = tmp;
}

void ctree_parse(struct ctree *ctree, const struct aulsmfs_ctree *ondisk)
{
	le16_t height;

	/* Teoritically ondisk might be unaligned, thus this mess. */
	memcpy(&ctree->ptr, &ondisk->ptr, sizeof(ctree->ptr));
	memcpy(&height, &ondisk->height, sizeof(height));
	ctree->height = le16toh(height);
}

void ctree_dump(const struct ctree *ctree, struct aulsmfs_ctree *ondisk)
{
	const le16_t height = htole16(ctree->height);

	memcpy(&ondisk->ptr, &ctree->ptr, sizeof(ctree->ptr));
	memcpy(&ondisk->height, &height, sizeof(height));
}


void ctree_iter_setup(struct ctree_iter *iter, struct ctree *ctree)
{
	memset(iter, 0, sizeof(*iter));
	iter->lsm = ctree->lsm;
	iter->ptr = ctree->ptr;
	iter->height = ctree->height;
}

void ctree_iter_release(struct ctree_iter *iter)
{
	if (iter->node) {
		for (int i = 0; i != iter->height; ++i) {
			if (iter->node[i])
				ctree_node_release(iter->node[i]);
			free(iter->node[i]);
		}
	}

	free(iter->buf);
	if (iter->node != iter->_node)
		free(iter->node);
	if (iter->pos != iter->_pos)
		free(iter->pos);
	memset(iter, 0, sizeof(*iter));
}

static int ctree_iter_prepare(struct ctree_iter *iter)
{
	if (iter->node && iter->pos)
		return 0;

	assert(!iter->node && !iter->pos);
	if (iter->height > CTREE_ITER_INLINE_HEIGHT) {
		iter->node = calloc(iter->height, sizeof(*iter->node));
		if (!iter->node)
			return -ENOMEM;

		iter->pos = calloc(iter->height, sizeof(*iter->pos));
		if (!iter->pos) {
			free(iter->node);
			iter->node = NULL;
			return -ENOMEM;
		}

		memset(iter->node, 0, sizeof(*iter->node) * iter->height);
		memset(iter->pos, 0, sizeof(*iter->pos) * iter->height);
		return 0;
	}

	iter->node = iter->_node;
	iter->pos = iter->_pos;
	
	return 0;
}

static size_t __ctree_node_lower_bound(const struct ctree_node *node,
			const struct lsm_key *key)
{
	for (size_t i = 0; i != node->entries; ++i) {
		struct lsm_key node_key;

		ctree_node_key(node, i, &node_key);

		const int res = node->lsm->cmp(&node_key, key);

		if (res >= 0)
			return i;
	}
	return node->entries;
}

static size_t __ctree_node_upper_bound(const struct ctree_node *node,
			const struct lsm_key *key)
{
	for (size_t i = 0; i != node->entries; ++i) {
		struct lsm_key node_key;

		ctree_node_key(node, i, &node_key);

		const int res = node->lsm->cmp(&node_key, key);

		if (res > 0)
			return i;
	}
	return node->entries;
}

static int ctree_node_ptr(const struct ctree_node *node, size_t pos,
			struct aulsmfs_ptr *ptr)
{
	struct lsm_val val;

	ctree_node_val(node, pos, &val);
	if (val.size != sizeof(*ptr))
		return -ENOBUFS;
	memcpy(ptr, val.ptr, sizeof(*ptr));
	return 0;
}

static int __ctree_get_node(struct ctree_iter *iter,
			const struct aulsmfs_ptr *ptr, int level)
{
	struct ctree_node *node;
	int rc;

	if (!iter->node[level] || memcmp(&iter->node[level]->ptr, ptr,
				sizeof(*ptr))) {
		node = ctree_node_create(iter->lsm);
		if (!node)
			return -ENOMEM;
		rc = ctree_node_read(node, ptr, level);
		if (rc < 0) {
			ctree_node_destroy(node);
			return rc;
		}
		ctree_node_destroy(iter->node[level]);
		iter->node[level] = node;
	}
	return 0;
}

static int __ctree_lookup(struct ctree_iter *iter, const struct lsm_key *key)
{
	int rc = ctree_iter_prepare(iter);

	if (rc < 0)
		return rc;

	struct ctree_node *node;
	size_t pos;
	struct aulsmfs_ptr ptr = iter->ptr;

	for (int level = iter->height - 1; level; --level) {
		rc = __ctree_get_node(iter, &ptr, level);
		if (rc < 0)
			return rc;

		node = iter->node[level];
		pos = __ctree_node_upper_bound(node, key);
		if (pos)
			--pos;
		iter->pos[level] = pos;
		if (ctree_node_ptr(node, pos, &ptr) < 0)
			return -EIO;
	}

	rc = __ctree_get_node(iter, &ptr, 0);
	if (rc < 0)
		return rc;

	node = iter->node[0];
	pos = __ctree_node_lower_bound(node, key);
	iter->pos[0] = pos;
	return 0;
}

static int ctree_copy_item(struct ctree_iter *iter)
{
	struct lsm_key key_;
	struct lsm_val val_;

	ctree_node_key(iter->node[0], iter->pos[0], &key_);
	ctree_node_val(iter->node[0], iter->pos[0], &val_);

	if (iter->buf_size < key_.size + val_.size) {
		const size_t size = key_.size + val_.size;
		void *buf = malloc(size);

		if (!buf)
			return -ENOMEM;

		free(iter->buf);
		iter->buf = buf;
		iter->buf_size = size;
	}

	char *key_ptr = iter->buf;
	char *val_ptr = key_ptr + key_.size;

	memcpy(key_ptr, key_.ptr, key_.size);
	iter->key.ptr = key_ptr;
	iter->key.size = key_.size;


	memcpy(val_ptr, val_.ptr, val_.size);
	iter->val.ptr = val_ptr;
	iter->val.size = val_.size;

	return 0;
}

int ctree_next(struct ctree_iter *iter)
{
	int level = -1;
	int rc;

	for (int i = 0; i != iter->height; ++i) {
		const struct ctree_node *const node = iter->node[i];
		const size_t pos = iter->pos[i];

		if (pos + 1 < node->entries) {
			level = i;
			break;
		}
	}

	if (level == -1) {
		if (iter->height && iter->pos[0] != iter->node[0]->entries)
			++iter->pos[0];

		return -ENOENT;
	}

	for (int i = 0; i != level; ++i) {
		ctree_node_destroy(iter->node[i]);
		iter->node[i] = NULL;
	}

	++iter->pos[level];

	for (int i = level; i != 0; --i) {
		const struct ctree_node *const parent = iter->node[i];
		const size_t pos = iter->pos[i];

		struct ctree_node *child;
		struct aulsmfs_ptr ptr;

		if (ctree_node_ptr(parent, pos, &ptr) < 0)
			return -EIO;
		child = ctree_node_create(iter->lsm);
		if (!child)
			return -ENOMEM;
		rc = ctree_node_read(child, &ptr, i - 1);
		if (rc < 0) {
			ctree_node_destroy(child);
			return rc;
		}
		iter->node[i - 1] = child;
		iter->pos[i - 1] = 0;
	}

	if (ctree_copy_item(iter) < 0)
		return -ENOMEM;
	return 0;
}

int ctree_prev(struct ctree_iter *iter)
{
	int level = -1;
	int rc;

	for (int i = 0; i != iter->height; ++i) {
		const size_t pos = iter->pos[i];

		if (pos) {
			level = i;
			break;
		}
	}

	if (level == -1)
		return -ENOENT;

	for (int i = 0; i != level; ++i) {
		ctree_node_destroy(iter->node[i]);
		iter->node[i] = NULL;
	}

	--iter->pos[level];

	for (int i = level; i != 0; --i) {
		const struct ctree_node *const parent = iter->node[i];
		const size_t pos = iter->pos[i];

		struct ctree_node *child;
		struct aulsmfs_ptr ptr;

		if (ctree_node_ptr(parent, pos, &ptr) < 0)
			return -EIO;
		child = ctree_node_create(iter->lsm);
		if (!child)
			return -ENOMEM;
		rc = ctree_node_read(child, &ptr, i - 1);
		if (rc < 0) {
			ctree_node_destroy(child);
			return rc;
		}
		iter->node[i - 1] = child;
		iter->pos[i - 1] = child->entries - 1;
	}

	if (ctree_copy_item(iter) < 0)
		return -ENOMEM;

	return 0;
}

int ctree_lower_bound(struct ctree_iter *iter, const struct lsm_key *key)
{
	int rc = __ctree_lookup(iter, key);

	if (rc < 0)
		return rc;

	if (!iter->height)
		return 0;

	if (iter->pos[0] == iter->node[0]->entries) {
		rc = ctree_next(iter);
		if (rc == -ENOENT)
			return 0;
		return rc;
	}
	return ctree_copy_item(iter);
}

int ctree_upper_bound(struct ctree_iter *iter, const struct lsm_key *key)
{
	int rc = ctree_lower_bound(iter, key);

	if (rc < 0)
		return rc;

	if (!iter->height)
		return 0;

	if (iter->pos[0] == iter->node[0]->entries)
		return 0;

	struct lsm_key node_key;

	ctree_node_key(iter->node[0], iter->pos[0], &node_key);
	if (iter->lsm->cmp(&node_key, key) > 0)
		return 0;

	return ctree_next(iter);
}

int ctree_lookup(struct ctree_iter *iter, const struct lsm_key *key)
{
	int rc = ctree_lower_bound(iter, key);

	if (rc < 0)
		return rc;

	if (!iter->height)
		return 0;

	if (iter->pos[0] == iter->node[0]->entries)
		return 0;

	struct lsm_key node_key;

	ctree_node_key(iter->node[0], iter->pos[0], &node_key);

	if (!iter->lsm->cmp(&node_key, key))
		return 1;
	return 0;
}

int ctree_begin(struct ctree_iter *iter)
{
	int rc = ctree_iter_prepare(iter);

	if (rc < 0)
		return rc;

	struct ctree_node *node;
	struct aulsmfs_ptr ptr = iter->ptr;

	for (int level = iter->height - 1; level >= 0; --level) {
		node = ctree_node_create(iter->lsm);
		if (!node)
			return -ENOMEM;

		rc = ctree_node_read(node, &ptr, level);
		if (rc < 0) {
			ctree_node_destroy(node);
			return rc;
		}

		iter->node[level] = node;
		iter->pos[level] = 0;

		if (level && ctree_node_ptr(node, 0, &ptr) < 0)
			return -EIO;
	}

	if (iter->height)
		return ctree_copy_item(iter);
	return 0;
}

int ctree_end(struct ctree_iter *iter)
{
	int rc = ctree_iter_prepare(iter);

	if (rc < 0)
		return rc;

	struct ctree_node *node;
	struct aulsmfs_ptr ptr = iter->ptr;

	for (int level = iter->height - 1; level >= 0; --level) {
		node = ctree_node_create(iter->lsm);
		if (!node)
			return -ENOMEM;

		rc = ctree_node_read(node, &ptr, level);
		if (rc < 0) {
			ctree_node_destroy(node);
			return rc;
		}

		iter->node[level] = node;
		iter->pos[level] = node->entries - 1;

		if (level && ctree_node_ptr(node, node->entries - 1, &ptr) < 0)
			return -EIO;
	}

	if (iter->height)
		++iter->pos[0];
	return 0;
}

int ctree_key(const struct ctree_iter *iter, struct lsm_key *key)
{
	if (!iter->height || iter->pos[0] == iter->node[0]->entries)
		return -ENOENT;
	if (key)
		*key = iter->key;
	return 0;
}

int ctree_val(const struct ctree_iter *iter, struct lsm_val *val)
{
	if (!iter->height || iter->pos[0] == iter->node[0]->entries)
		return -ENOENT;
	if (val)
		*val = iter->val;
	return 0;
}
