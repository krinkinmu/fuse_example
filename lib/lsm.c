#include <lsm.h>
#include <crc64.h>

#include <endian.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>


struct mtree_node {
	struct rb_node rb;
	lsm_key key;
	lsm_val val;
	int deleted;
};


static uint64_t aligndown(uint64_t addr, uint64_t align)
{
	assert((align & (align - 1)) == 0 && "alignment must be power of two");
	return addr & ~(uint64_t)(align - 1);
}

static uint64_t alignup(uint64_t addr, uint64_t align)
{
	return aligndown(addr + align - 1, align);
}


int mtree_setup(struct mtree *mtree, struct lsm *lsm, uint64_t gen,
			uint64_t offs, uint64_t size)
{
	memset(mtree, 0, sizeof(*mtree));
	mtree->lsm = lsm;
	mtree->offs = offs;
	mtree->size = size;
	mtree->start_gen = mtree->next_gen = gen;
	mtree->next_size = sizeof(struct aulsmfs_log_header);

	if (!(mtree->buf = malloc(size * lsm->io->page_size)))
		return -1;
	return 0;
}

int mtree_parse(struct mtree *tree, uint64_t offs, uint64_t size)
{
	/* Proper log parsing is going to be painful, but for now we can
	 * just read the whole log space in memory and parse it at once. */
	(void) tree;
	(void) offs;
	(void) size;
	return 0;
}

void mtree_release(struct mtree *tree)
{
	free(tree->buf);
	memset(tree, 0, sizeof(*tree));
}

static size_t mtree_log_remains(const struct mtree *tree)
{
	const size_t page_size = tree->lsm->io->page_size;
	const size_t current_size = tree->next_size;

	if (tree->size == tree->next_pos)
		return 0;

	return (tree->size - tree->next_pos) * page_size - current_size;
}

size_t mtree_log_size(const struct mtree *tree)
{
	return alignup(tree->next_size, tree->lsm->io->page_size);
}

static struct aulsmfs_node_entry *mtree_next_entry(struct mtree *tree)
{
	return (struct aulsmfs_node_entry *)(tree->buf + tree->next_size);
}

static struct mtree_node *mtree_node_create(int deleted,
			const lsm_key *key, const lsm_val *val)
{
	struct mtree_node *new = malloc(sizeof(*new) + key->size + val->size);
	char *key_ptr = (char *)(new + 1);
	char *val_ptr = key_ptr + key->size;

	if (!new)
		return NULL;

	new->deleted = deleted;

	new->key.size = key->size;
	new->key.ptr = key_ptr;
	memcpy(new->key.ptr, key->ptr, key->size);

	new->val.size = val->size;
	new->val.ptr = val_ptr;
	memcpy(new->val.ptr, val->ptr, val->size);

	return new;
}

static void mtree_node_destroy(struct mtree_node *node)
{
	free(node);
}

static void __mtree_tree_add(struct mtree *tree, struct mtree_node *new)
{
	struct lsm *lsm = tree->lsm;
	struct rb_node **plink = &tree->tree.root;
	struct rb_node *parent = 0;

	while (*plink) {
		struct mtree_node *old = (struct mtree_node *)(*plink);
		const int cmp = lsm->cmp(&old->key, &new->key);

		if (!cmp) {
			rb_swap_nodes(&tree->tree, &old->rb, &new->rb);
			mtree_node_destroy(old);
			return;
		}

		parent = *plink;
		if (cmp < 0) plink = &parent->left;
		else plink = &parent->right;
	}

	rb_link(&new->rb, parent, plink);
	rb_insert(&new->rb, &tree->tree);
}

static int __mtree_log_add(struct mtree *tree, int deleted,
			const lsm_key *key, const lsm_val *val)
{
	const size_t size = key->size + val->size + sizeof(struct aulsmfs_node_entry);
 
	if (size > mtree_log_remains(tree))
		return -1;

	struct aulsmfs_node_entry *entry = mtree_next_entry(tree);
	char *key_ptr = (char *)(entry + 1);
	char *val_ptr = key_ptr + key->size;

	entry->key_size = htole64(key->size);
	entry->val_size = htole64(val->size);
	entry->deleted = deleted ? 1 : 0;
	memcpy(key_ptr, key->ptr, key->size);
	memcpy(val_ptr, val->ptr, val->size);
	tree->next_size += size;
	return 0;
}

int mtree_add(struct mtree *tree, const lsm_key *key, const lsm_val *val)
{
	struct mtree_node *new = mtree_node_create(0, key, val);

	if (!new)
		return -1;

	if (__mtree_log_add(tree, 0, key, val) < 0) {
		mtree_node_destroy(new);
		return -1;
	}

	__mtree_tree_add(tree, new);
	return 0;
}

int mtree_del(struct mtree *tree, const lsm_key *key)
{
	const lsm_val val = { .ptr = NULL, .size = 0 };
	struct mtree_node *new = mtree_node_create(1, key, &val);

	if (!new)
		return -1;

	if (__mtree_log_add(tree, 1, key, &val) < 0) {
		mtree_node_destroy(new);
		return -1;
	}

	__mtree_tree_add(tree, new);
	return 0;
}

int mtree_checkpoint(struct mtree *tree)
{
	struct io * const io = tree->lsm->io;
	const size_t page_size = io->page_size;

	const size_t size = alignup(tree->next_size, page_size);
	const uint64_t offs = tree->offs + tree->next_pos;

	struct aulsmfs_log_header *header;

	header = (struct aulsmfs_log_header *)tree->buf;
	header->gen = htole64(tree->next_gen);
	header->size = htole64(tree->next_size);
	header->csum = 0;

	memset((char *)tree->buf + tree->next_size, 0, size - tree->next_size);
	header->csum = htole64(crc64(tree->buf, size));

	if (io_write(io, tree->buf, size / page_size, offs) < 0)
		return -1;

	tree->next_pos += size / page_size;
	tree->next_size = tree->next_pos != tree->size ? sizeof(*header) : 0;
	++tree->next_gen;
	return 0;
}
