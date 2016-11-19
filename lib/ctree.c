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
		const int deleted = entry.deleted;

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

		ptr->deleted = deleted;
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

static int ctree_node_deleted(const struct ctree_node *node, size_t pos)
{
	return node->entry[pos].deleted;
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
	memset(builder, 0, sizeof(*builder));
}

static int __ctree_builder_append(struct ctree_builder *builder, int level,
			int deleted, const struct lsm_key *key,
			const struct lsm_val *val);

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

	rc = lsm_reserve(lsm, size, &offs);
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
	rc = __ctree_builder_append(builder, level + 1, 0, &key, &val);
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

	builder->ptr = root->ptr;
	builder->height = level + 1;
	ctree_node_reset(root);
	return 0;
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

	if (iter->node != iter->_node)
		free(iter->node);
	if (iter->pos != iter->_pos)
		free(iter->pos);
	memset(iter, 0, sizeof(*iter));
}

static int ctree_iter_prepare(struct ctree_iter *iter)
{
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

static int __ctree_lookup(struct ctree_iter *iter, const struct lsm_key *key)
{
	int rc = ctree_iter_prepare(iter);

	if (rc < 0)
		return rc;

	struct ctree_node *node;
	size_t pos;
	struct aulsmfs_ptr ptr = iter->ptr;

	for (int level = iter->height - 1; level; --level) {
		node = ctree_node_create(iter->lsm);
		if (!node)
			return -ENOMEM;

		rc = ctree_node_read(node, &ptr, level);
		if (rc < 0) {
			ctree_node_destroy(node);
			return rc;
		}

		pos = __ctree_node_upper_bound(node, key);
		if (pos)
			--pos;

		iter->node[level] = node;
		iter->pos[level] = pos;

		struct lsm_val val;

		ctree_node_val(node, pos, &val);
		if (val.size != sizeof(ptr))
			return -EIO;

		memcpy(&ptr, val.ptr, val.size);
	}

	node = ctree_node_create(iter->lsm);
	if (!node)
		return -ENOMEM;

	rc = ctree_node_read(node, &ptr, 0);
	if (rc < 0) {
		ctree_node_destroy(node);
		return rc;
	}

	pos = __ctree_node_lower_bound(node, key);
	iter->node[0] = node;
	iter->pos[0] = pos;
	return 0;
}

static int ctree_last(const struct ctree_iter *iter)
{
	for (int i = 0; i != iter->height; ++i) {
		if (iter->pos[i] != iter->node[i]->entries - 1)
			return 0;
	}
	return 1;	
}

int ctree_next(struct ctree_iter *iter)
{
	if (ctree_end(iter))
		return -ENOENT;

	/* Special case, end iterator points at the last entries in every
	 * internal node and past last entry in the last leaf. */
	if (ctree_last(iter)) {
		iter->pos[0]++;
		return 0;
	}

	int level;

	for (level = 0; level != iter->height; ++level) {
		struct ctree_node *node = iter->node[level];
		size_t pos = iter->pos[level];

		if (pos + 1 < node->entries) {
			iter->pos[level] = ++pos;
			break;
		}
		iter->node[level] = NULL;
		ctree_node_destroy(node);
	}

	if (level == 0)
		return 0;

	for (; level > 0; --level) {
		const struct ctree_node *parent = iter->node[level];
		const size_t pos = iter->pos[level];

		struct aulsmfs_ptr ptr;
		struct lsm_val val;

		ctree_node_val(parent, pos, &val);
		if (val.size != sizeof(ptr))
			return -EIO;

		memcpy(&ptr, val.ptr, val.size);

		struct ctree_node *child = ctree_node_create(iter->lsm);

		if (!child)
			return -ENOMEM;

		const int rc = ctree_node_read(child, &ptr, level - 1);

		if (rc < 0) {
			ctree_node_destroy(child);
			return rc;
		}
		iter->node[level - 1] = child;
		iter->pos[level - 1] = 0;
	}
	return 0;
}

int ctree_prev(struct ctree_iter *iter)
{
	if (ctree_begin(iter))
		return -ENOENT;

	int level;

	for (level = 0; level != iter->height; ++level) {
		struct ctree_node *node = iter->node[level];
		size_t pos = iter->pos[level];

		if (pos > 0) {
			iter->pos[level] = --pos;
			break;
		}
		iter->node[level] = NULL;
		ctree_node_destroy(node);
	}

	if (level == 0)
		return 0;

	if (level == iter->height)
		return -ENOENT;


	for (; level > 0; --level) {
		const struct ctree_node *parent = iter->node[level];
		const size_t pos = iter->pos[level];

		struct aulsmfs_ptr ptr;
		struct lsm_val val;

		ctree_node_val(parent, pos, &val);
		if (val.size != sizeof(ptr))
			return -EIO;

		memcpy(&ptr, val.ptr, val.size);

		struct ctree_node *child = ctree_node_create(iter->lsm);

		if (!child)
			return -ENOMEM;

		const int rc = ctree_node_read(child, &ptr, level - 1);

		if (rc < 0) {
			ctree_node_destroy(child);
			return rc;
		}
		iter->node[level - 1] = child;
		iter->pos[level - 1] = child->entries - 1;
	}
	return 0;
}

int ctree_end(const struct ctree_iter *iter)
{
	if (!iter->height)
		return 1;

	if (iter->pos[0] != iter->node[0]->entries)
		return 0;

	for (int i = 1; i != iter->height; ++i) {
		if (iter->pos[i] != iter->node[i]->entries - 1)
			return 0;
	}
	return 1;
}

int ctree_begin(const struct ctree_iter *iter)
{
	for (int i = 0; i != iter->height; ++i) {
		if (iter->pos[i])
			return 0;
	}
	return 1;
}

int ctree_equal(const struct ctree_iter *l, const struct ctree_iter *r)
{
	if (memcmp(&l->ptr, &r->ptr, sizeof(l->ptr)))
		return 0;

	if (l->height != r->height)
		return 0;

	for (int i = 0; i != l->height; ++i) {
		const struct ctree_node *left = l->node[i];
		const struct ctree_node *right = r->node[i];

		if (memcmp(&left->ptr, &right->ptr, sizeof(left->ptr)))
			return 0;

		if (l->pos[i] != r->pos[i])
			return 0;
	}
	return 1;
}

int ctree_lower_bound(struct ctree_iter *iter, const struct lsm_key *key)
{
	int rc = __ctree_lookup(iter, key);

	if (rc < 0)
		return rc;

	if (iter->pos[0] == iter->node[0]->entries) {
		if (ctree_end(iter))
			return 0;
		rc = ctree_next(iter);
		if (rc < 0)
			return rc;
	}
	return 0;
}

int ctree_upper_bound(struct ctree_iter *iter, const struct lsm_key *key)
{
	int rc = ctree_lower_bound(iter, key);

	if (rc < 0)
		return rc;

	if (ctree_end(iter))
		return 0;

	struct lsm_key node_key;

	ctree_key(iter, &node_key);
	if (iter->lsm->cmp(&node_key, key) > 0)
		return 0;

	return ctree_next(iter);
}

int ctree_lookup(struct ctree_iter *iter, const struct lsm_key *key)
{
	int rc = ctree_lower_bound(iter, key);

	if (rc < 0)
		return rc;

	if (ctree_end(iter))
		return 0;

	struct lsm_key node_key;

	ctree_key(iter, &node_key);
	return iter->lsm->cmp(&node_key, key) ? 0 : 1;
}

void ctree_key(const struct ctree_iter *iter, struct lsm_key *key)
{
	ctree_node_key(iter->node[0], iter->pos[0], key);
}

void ctree_val(const struct ctree_iter *iter, struct lsm_val *val)
{
	ctree_node_val(iter->node[0], iter->pos[0], val);
}

int ctree_deleted(const struct ctree_iter *iter)
{
	return ctree_node_deleted(iter->node[0], iter->pos[0]);
}
