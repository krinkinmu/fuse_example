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


int mtree_setup(struct mtree *mtree, struct lsm *lsm)
{
	memset(mtree, 0, sizeof(*mtree));
	mtree->lsm = lsm;
	return 0;
}

void mtree_release(struct mtree *tree)
{
	memset(tree, 0, sizeof(*tree));
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

static void __mtree_add(struct mtree *tree, struct mtree_node *new)
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

int mtree_add(struct mtree *tree, const lsm_key *key, const lsm_val *val)
{
	struct mtree_node *new = mtree_node_create(0, key, val);

	if (!new)
		return -1;

	__mtree_add(tree, new);
	return 0;
}

int mtree_del(struct mtree *tree, const lsm_key *key)
{
	const lsm_val val = { .ptr = NULL, .size = 0 };
	struct mtree_node *new = mtree_node_create(1, key, &val);

	if (!new)
		return -1;

	__mtree_add(tree, new);
	return 0;
}
