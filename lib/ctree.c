#include <ctree.h>
#include <crc64.h>
#include <lsm_fwd.h>

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


static int __ctree_node_setup(struct io *io, struct ctree_node *node,
			size_t size, size_t count)
{
	assert(io_align(io, size) == size);

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

static int ctree_node_setup(struct io *io, struct ctree_node *node)
{
	const int rc = __ctree_node_setup(io, node,
				/* size */ io_align(io, 4096),
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

static struct ctree_node *ctree_node_create(void)
{
	struct ctree_node *node = malloc(sizeof(*node));

	memset(node, 0, sizeof(*node));
	return node;
}

static void ctree_node_destroy(struct ctree_node *node)
{
	if (node) {
		ctree_node_release(node);
		free(node);
	}
}

static int ctree_node_parse(struct io *io, struct ctree_node *node)
{
	const struct aulsmfs_node_header *header = (void *)node->buf;
	const int level = le64toh(header->level);
	const size_t bytes = le64toh(header->size);
	const size_t pages = le64toh(node->ptr.size);

	if (bytes > io_bytes(io, pages))
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

static int ctree_node_read(struct io *io, struct ctree_node *node,
			const struct aulsmfs_ptr *ptr, int level)
{
	const size_t pages = le64toh(ptr->size);
	const size_t buf_size = io_bytes(io, pages);
	void *buf = malloc(buf_size);
	int rc;

	if (!buf)
		return -ENOMEM;

	node->buf = buf;
	node->max_bytes = buf_size;
	rc = io_read(io, buf, pages, le64toh(ptr->offs));
	if (rc < 0)
		return rc;

	if (crc64(buf, buf_size) != le64toh(ptr->csum))
		return -EIO;

	node->ptr = *ptr;
	node->level = level;
	rc = ctree_node_parse(io, node);
	if (rc < 0)
		return rc;
	return 0;
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

static int ctree_node_can_append(const struct io *io,
			const struct ctree_node *node,
			size_t count, size_t size)
{
	const size_t bytes = count * sizeof(struct aulsmfs_node_entry) + size;

	if (node->entries + count <= MIN_FANOUT)
		return 1;

	if (io_pages(io, node->bytes + bytes) == io_pages(io, node->bytes))
		return 1;

	return 0;
}

static int ctree_ensure_entries(const struct io *io, struct ctree_node *node,
			size_t count, size_t size)
{
	const size_t entries = node->entries + count;
	const size_t pages = io_pages(io, node->bytes + size);
	const size_t bytes = io_bytes(io, pages);

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

static int ctree_node_append(const struct io *io, struct ctree_node *node,
			const struct lsm_key *key, const struct lsm_val *val)
{
	struct aulsmfs_node_entry nentry;
	const size_t size = key->size + val->size + sizeof(nentry);
	const int rc = ctree_ensure_entries(io, node, 1, size);

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

static int ctree_node_write(struct io *io, struct ctree_node *node,
			uint64_t offs, int level)
{
	const size_t size = io_pages(io, node->bytes);
	struct aulsmfs_node_header *header = (void *)node->buf;

	header->size = htole64(node->bytes);
	header->level = htole64(level);

	const int rc = io_write(io, node->buf, size, offs);

	if (rc < 0)
		return rc;

	node->ptr.offs = htole64(offs);
	node->ptr.size = htole64(size);
	node->ptr.csum = htole64(crc64(node->buf, io_bytes(io, size)));
	node->level = level;
	return 0;
}


void ctree_builder_setup(struct ctree_builder *builder, struct io *io,
			struct alloc *alloc)
{
	memset(builder, 0, sizeof(*builder));
	builder->io = io;
	builder->alloc = alloc;
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

	struct alloc *alloc = builder->alloc;

	const int rc = alloc_reserve(alloc, size, offs);

	if (rc < 0)
		return rc;

	builder->pages += size;
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

static void ctree_builder_free(struct ctree_builder *builder, uint64_t size,
			uint64_t offs)
{
	struct alloc *alloc = builder->alloc;

	alloc_cancel(alloc, size, offs);
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
	struct io *io = builder->io;
	struct ctree_node *node = ctree_builder_node(builder, level);
	uint64_t size = io_pages(io, node->bytes);
	uint64_t offs;
	int rc;

	if (!node->entries)
		return 0;

	rc = ctree_builder_alloc(builder, size, &offs);
	if (rc < 0)
		return rc;

	rc = ctree_node_write(io, node, offs, level);
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
		struct ctree_node *node = ctree_node_create();

		if (!node)
			return -ENOMEM;

		const int rc = ctree_node_setup(builder->io, node);

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
	const struct io *io = builder->io;
	const struct ctree_node *node = ctree_builder_node(builder,
				level);

	return ctree_node_can_append(io, node, count, size);
}

static int __ctree_builder_append(struct ctree_builder *builder, int level,
			const struct lsm_key *key, const struct lsm_val *val)
{
	struct io *io = builder->io;
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

	return ctree_node_append(io, node, key, val);
}

int ctree_builder_append(struct ctree_builder *builder,
			const struct lsm_key *key, const struct lsm_val *val)
{
	return __ctree_builder_append(builder, 0, key, val);
}

int ctree_builder_finish(struct ctree_builder *builder)
{
	struct io *io = builder->io;
	int level = 0;
	int rc;

	while (level < builder->nodes - 1) {
		rc = ctree_builder_flush(builder, level);
		if (rc < 0)
			return rc;
		++level;
	}

	if (!level) {
		memset(&builder->ptr, 0, sizeof(builder->ptr));
		builder->height = 0;
		return 0;
	}

	/* We have written all but last level, the node will be root of the
	 * ctree. */
	struct ctree_node *root = ctree_builder_node(builder, level);
	const size_t size = io_pages(io, root->bytes);
	uint64_t offs;

	rc = ctree_builder_alloc(builder, size, &offs);
	if (rc < 0)
		return rc;

	rc = ctree_node_write(io, root, offs, level);
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

		ctree_builder_free(builder, size, offs);
	}
}


void ctree_setup(struct ctree *ctree, struct io *io, ctree_cmp_t cmp)
{
	memset(ctree, 0, sizeof(*ctree));
	ctree->io = io;
	ctree->cmp = cmp;
}

void ctree_release(struct ctree *ctree)
{
	memset(ctree, 0, sizeof(*ctree));
}

int ctree_reset(struct ctree *ctree, const struct aulsmfs_ptr *ptr,
			size_t height, size_t pages)
{
	if (!ptr) {
		ctree->height = 0;
		ctree->pages = pages;
		memset(&ctree->ptr, 0, sizeof(ctree->ptr));
		return 0;
	}
	ctree->ptr = *ptr;
	ctree->height = height;
	ctree->pages = pages;
	return 0;
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
	le32_t height;
	le32_t pages;

	/* Teoritically ondisk might be unaligned, thus this mess. */
	memcpy(&ctree->ptr, &ondisk->ptr, sizeof(ctree->ptr));
	memcpy(&height, &ondisk->height, sizeof(height));
	memcpy(&pages, &ondisk->pages, sizeof(pages));

	ctree->height = le32toh(height);
	ctree->pages = le32toh(pages);
}

void ctree_dump(const struct ctree *ctree, struct aulsmfs_ctree *ondisk)
{
	const le32_t height = htole32(ctree->height);
	const le32_t pages = htole32(ctree->pages);

	memcpy(&ondisk->ptr, &ctree->ptr, sizeof(ctree->ptr));
	memcpy(&ondisk->height, &height, sizeof(height));
	memcpy(&ondisk->pages, &pages, sizeof(pages));
}


void ctree_iter_setup(struct ctree_iter *iter, struct ctree *ctree)
{
	memset(iter, 0, sizeof(*iter));
	iter->io = ctree->io;
	iter->cmp = ctree->cmp;
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

	free(iter->node);
	free(iter->pos);
	memset(iter, 0, sizeof(*iter));
}

static int ctree_iter_prepare(struct ctree_iter *iter)
{
	if (iter->node && iter->pos)
		return 0;

	assert(!iter->node && !iter->pos);
	iter->node = calloc(iter->height, sizeof(*iter->node));
	if (!iter->node)
		return -ENOMEM;

	iter->pos = calloc(iter->height, sizeof(*iter->pos));
	if (!iter->pos) {
		free(iter->node);
		iter->node = NULL;
		return -ENOMEM;
	}
	return 0;
}

static size_t __ctree_node_lower_bound(const struct ctree_node *node,
			const struct lsm_key *key, ctree_cmp_t cmp)
{
	for (size_t i = 0; i != node->entries; ++i) {
		struct lsm_key node_key;

		ctree_node_key(node, i, &node_key);

		const int res = cmp(&node_key, key);

		if (res >= 0)
			return i;
	}
	return node->entries;
}

static size_t __ctree_node_upper_bound(const struct ctree_node *node,
			const struct lsm_key *key, ctree_cmp_t cmp)
{
	for (size_t i = 0; i != node->entries; ++i) {
		struct lsm_key node_key;

		ctree_node_key(node, i, &node_key);

		const int res = cmp(&node_key, key);

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
		node = ctree_node_create();
		if (!node)
			return -ENOMEM;
		rc = ctree_node_read(iter->io, node, ptr, level);
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

	if (!iter->height)
		return 0;

	struct ctree_node *node;
	size_t pos;
	struct aulsmfs_ptr ptr = iter->ptr;

	for (int level = iter->height - 1; level; --level) {
		rc = __ctree_get_node(iter, &ptr, level);
		if (rc < 0)
			return rc;

		node = iter->node[level];
		pos = __ctree_node_upper_bound(node, key, iter->cmp);
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
	pos = __ctree_node_lower_bound(node, key, iter->cmp);
	iter->pos[0] = pos;
	return 0;
}

int ctree_next(struct ctree_iter *iter)
{
	int level = -1;
	int rc;

	for (int i = 0; i != iter->height; ++i) {
		assert(iter->node[i]);

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

		assert(parent);

		struct ctree_node *child;
		struct aulsmfs_ptr ptr;

		if (ctree_node_ptr(parent, pos, &ptr) < 0)
			return -EIO;
		child = ctree_node_create();
		if (!child)
			return -ENOMEM;
		rc = ctree_node_read(iter->io, child, &ptr, i - 1);
		if (rc < 0) {
			ctree_node_destroy(child);
			return rc;
		}
		iter->node[i - 1] = child;
		iter->pos[i - 1] = 0;
	}
	return 0;
}

int ctree_prev(struct ctree_iter *iter)
{
	int level = -1;
	int rc;

	for (int i = 0; i != iter->height; ++i) {
		const size_t pos = iter->pos[i];

		assert(iter->node[i]);
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

		assert(parent);

		struct ctree_node *child;
		struct aulsmfs_ptr ptr;

		if (ctree_node_ptr(parent, pos, &ptr) < 0)
			return -EIO;
		child = ctree_node_create();
		if (!child)
			return -ENOMEM;
		rc = ctree_node_read(iter->io, child, &ptr, i - 1);
		if (rc < 0) {
			ctree_node_destroy(child);
			return rc;
		}
		iter->node[i - 1] = child;
		iter->pos[i - 1] = child->entries - 1;
	}
	return 0;
}

int ctree_lower_bound(struct ctree_iter *iter, const struct lsm_key *key)
{
	if (!iter->height)
		return 0;

	int rc = __ctree_lookup(iter, key);

	if (rc < 0)
		return rc;

	assert(iter->node[0]);
	if (iter->pos[0] == iter->node[0]->entries) {
		rc = ctree_next(iter);
		if (rc == -ENOENT)
			return 0;
		return rc;
	}
	return 0;
}

int ctree_upper_bound(struct ctree_iter *iter, const struct lsm_key *key)
{
	if (!iter->height)
		return 0;

	int rc = ctree_lower_bound(iter, key);

	if (rc < 0)
		return rc;

	assert(iter->node[0]);
	if (iter->pos[0] == iter->node[0]->entries)
		return 0;

	struct lsm_key node_key;

	ctree_node_key(iter->node[0], iter->pos[0], &node_key);
	if (iter->cmp(&node_key, key) > 0)
		return 0;

	return ctree_next(iter);
}

int ctree_lookup(struct ctree_iter *iter, const struct lsm_key *key)
{
	if (!iter->height)
		return 0;

	int rc = ctree_lower_bound(iter, key);

	if (rc < 0)
		return rc;

	assert(iter->node[0]);
	if (iter->pos[0] == iter->node[0]->entries)
		return 0;

	struct lsm_key node_key;

	ctree_node_key(iter->node[0], iter->pos[0], &node_key);

	if (!iter->cmp(&node_key, key))
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
		node = ctree_node_create();
		if (!node)
			return -ENOMEM;

		rc = ctree_node_read(iter->io, node, &ptr, level);
		if (rc < 0) {
			ctree_node_destroy(node);
			return rc;
		}

		iter->node[level] = node;
		iter->pos[level] = 0;

		if (level && ctree_node_ptr(node, 0, &ptr) < 0)
			return -EIO;
	}
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
		node = ctree_node_create();
		if (!node)
			return -ENOMEM;

		rc = ctree_node_read(iter->io, node, &ptr, level);
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
	if (key)
		memset(key, 0, sizeof(*key));
	if (!iter->height || iter->pos[0] == iter->node[0]->entries)
		return -ENOENT;
	if (key)
		ctree_node_key(iter->node[0], iter->pos[0], key);
	return 0;
}

int ctree_val(const struct ctree_iter *iter, struct lsm_val *val)
{
	if (val)
		memset(val, 0, sizeof(*val));
	if (!iter->height || iter->pos[0] == iter->node[0]->entries)
		return -ENOENT;
	if (val)
		ctree_node_val(iter->node[0], iter->pos[0], val);
	return 0;
}
