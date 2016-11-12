#include <lsm.h>
#include <crc64.h>
#include <mtree.h>

#include <endian.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>


static size_t aligndown(size_t val, size_t align)
{
	assert((align & (align - 1)) == 0);
	return val & (align - 1);
}

static size_t alignup(size_t val, size_t align)
{
	return aligndown(val + align - 1, align);
}

int lsm_add(struct lsm *lsm, const struct lsm_key *key,
			const struct lsm_val *val)
{
	return mtree_add(&lsm->c0, key, val);
}

int lsm_del(struct lsm *lsm, const struct lsm_key *key)
{
	return mtree_del(&lsm->c0, key);
}

static struct lsm_node *__lsm_node_create(size_t size, size_t entries)
{
	struct lsm_node *node = malloc(sizeof(*node) + size ? size - 1 : 0);

	if (!node)
		return NULL;

	memset(node, 0, sizeof(*node));
	node->entry = calloc(entries, sizeof(*node->entry));
	if (entries && !node->entry) {
		free(node);
		return NULL;
	}

	node->capacity = entries;
	node->data = node->_data;
	node->data_capacity = size;

	return node; 
}

struct lsm_node *lsm_node_create(struct lsm *lsm)
{
	static const size_t min_size = 4096;
	static const size_t min_fanout = 20;
	static const size_t per_entry = min_size / min_fanout;

	const size_t page_size = lsm->io->page_size;
	const size_t size = page_size > min_size ? page_size : min_size;
	const size_t fanout = size / per_entry;

	assert(aligndown(size, page_size) == size);

	struct lsm_node *node =  __lsm_node_create(size, fanout);

	if (!node)
		return NULL;

	node->lsm = lsm;
	node->data_size = sizeof(struct aulsmfs_node_header);
	return node;
}

void lsm_node_destroy(struct lsm_node *node)
{
	free(node->entry);
	if (node->data != node->_data)
		free(node->data);
	free(node);
}

static int lsm_node_more_entries(struct lsm_node *node, size_t needed)
{
	const size_t required = node->entries + needed;

	if (required < node->entries)
		return 0;

	const size_t new_capacity = node->capacity * 2 >= required
				? node->capacity * 2
				: required;
	struct lsm_entry_pos *entry = realloc(node->entry,
				new_capacity * sizeof(*entry));

	if (!entry)
		return -1;

	node->entry = entry;
	node->capacity = new_capacity;
	return 0;
}

static int lsm_node_more_data(struct lsm_node *node, size_t needed)
{
	const size_t required = node->data_size + needed;
	const size_t new_capacity = alignup(node->data_capacity * 2 >= required
				? node->data_capacity * 2
				: required, node->lsm->io->page_size);
	void *data;

	if (required < node->data_capacity)
		return 0;

	if (node->data == node->_data) {
		if (!(data = malloc(new_capacity)))
			return -1;

		memcpy(data, node->data, node->data_size);
	} else {
		if (!(data = realloc(node->data, new_capacity)))
			return -1;
	}

	node->data = data;
	node->data_capacity = new_capacity;
	return 0;
}

int lsm_node_append(struct lsm_node *node, int deleted,
			const struct lsm_key *key, const struct lsm_val *val)
{
	const size_t size = key->size + val->size +
				sizeof(struct aulsmfs_node_entry);

	if (lsm_node_more_entries(node, 1) < 0)
		return -1;

	if (lsm_node_more_data(node, size) < 0)
		return -1;

	struct aulsmfs_node_entry entry = {
		.key_size = key->size,
		.val_size = val->size,
		.deleted = deleted ? 1 : 0
	};
	struct lsm_entry_pos *pos = &node->entry[node->entries++];
	char *entry_ptr = node->data + node->data_size;
	char *key_ptr = entry_ptr + sizeof(entry);
	char *val_ptr = key_ptr + key->size;

	memcpy(entry_ptr, &entry, sizeof(entry));
	memcpy(key_ptr, key->ptr, key->size);
	memcpy(val_ptr, val->ptr, val->size);
	node->data_size += size;

	pos->key_pos = key_ptr - node->data;
	pos->key_size = key->size;
	pos->val_pos = val_ptr - node->data;
	pos->val_size = val->size;
	return 0;
}

static size_t lsm_node_pages(const struct lsm_node *node)
{
	return node->data_size / node->lsm->io->page_size;
}

int lsm_node_write(struct lsm_node *node)
{
	struct io *const io = node->lsm->io;
	struct aulsmfs_node_header *header =
				(struct aulsmfs_node_header *)node->data;
	const size_t pages = lsm_node_pages(node);
	const size_t bytes = pages * io->page_size;

	assert(bytes <= node->data_capacity);

	memset(node->data + node->data_size, 0, bytes - node->data_size);
	header->size = htole64(node->data_size);
	header->level = htole64(node->level);
	node->csum = crc64(node->data, node->data_size);

	if (io_write(io, node->data, pages, node->offs) < 0)
		return -1;
	return 0;
}
