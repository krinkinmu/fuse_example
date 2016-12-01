#include <mtree.h>
#include <lsm_fwd.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>


struct mtree_node {
	struct rb_node rb;
	struct lsm_key key;
	struct lsm_val val;
};

static struct mtree_node *mtree_node_create(const struct lsm_key *key,
			const struct lsm_val *val)
{
	const size_t size = key->size + val->size + sizeof(struct mtree_node);
	struct mtree_node * const new = malloc(size);

	if (!new)
		return NULL;

	char *const key_ptr = (char *)(new + 1);
	char *const val_ptr = key_ptr + key->size;

	new->key.ptr = key_ptr;
	new->key.size = key->size;

	if (key->size) {
		assert(key->ptr);
		memcpy(new->key.ptr, key->ptr, new->key.size);
	}

	new->val.ptr = val_ptr;
	new->val.size = val->size;

	if (val->size) {
		assert(val->ptr);
		memcpy(new->val.ptr, val->ptr, new->val.size);
	}
	return new;
}

static void mtree_node_destroy(struct mtree_node *node)
{
	free(node);
}

void mtree_setup(struct mtree *tree, mtree_cmp_t cmp)
{
	tree->cmp = cmp;
	tree->bytes = 0;
	tree->tree.root = NULL;
}

static void __mtree_release(struct rb_node *node)
{
	while (node) {
		struct rb_node *to_free = node;

		__mtree_release(node->right);
		node = node->left;
		mtree_node_destroy((struct mtree_node *)to_free);
	}
}

void mtree_release(struct mtree *tree)
{
	__mtree_release(tree->tree.root);
	tree->tree.root = NULL;
	tree->bytes = 0;
}

void mtree_reset(struct mtree *tree)
{
	struct rb_node *root = tree->tree.root;

	tree->tree.root = NULL;
	tree->bytes = 0;
	__mtree_release(root);
}

int mtree_is_empty(const struct mtree *tree)
{
	return tree->tree.root ? 0 : 1;
}

void mtree_swap(struct mtree *l, struct mtree *r)
{
	const struct mtree tmp = *l;

	*l = *r;
	*r = tmp;
}

static void __mtree_insert(struct mtree *tree, struct mtree_node *new)
{
	struct rb_node **plink = &tree->tree.root;
	struct rb_node *parent = NULL;

	while (*plink) {
		struct mtree_node * const old = (struct mtree_node *)(*plink);
		const int cmp = tree->cmp(&old->key, &new->key);

		if (!cmp) {
			rb_swap_nodes(&tree->tree, &old->rb, &new->rb);
			mtree_node_destroy(old);
			return;
		}

		parent = *plink;
		if (cmp < 0)
			plink = &parent->right;
		else
			plink = &parent->left;
	}

	rb_link(&new->rb, parent, plink);
	rb_insert(&new->rb, &tree->tree);
}

int mtree_add(struct mtree *tree, const struct lsm_key *key,
			const struct lsm_val *val)
{
	struct mtree_node *new = mtree_node_create(key, val);

	if (!new)
		return -ENOMEM;

	__mtree_insert(tree, new);
	tree->bytes += key->size + val->size;
	return 0;
}

void mtree_iter_setup(struct mtree_iter *iter, struct mtree *tree)
{
	iter->cmp = tree->cmp;
	iter->tree = &tree->tree;
	iter->node = NULL;
}

void mtree_iter_release(struct mtree_iter *iter)
{
	iter->tree = NULL;
	iter->node = NULL;
}

void mtree_lower_bound(struct mtree_iter *iter, const struct lsm_key *key)
{
	struct rb_node *p = iter->tree->root;
	struct mtree_node *lower = NULL;

	while (p) {
		struct mtree_node * const node = (struct mtree_node *)p;
		const int cmp = iter->cmp(&node->key, key);

		if (cmp >= 0) {
			p = p->left;
			lower = node;
		} else {
			p = p->right;
		}
	}
	iter->node = lower;
}

void mtree_upper_bound(struct mtree_iter *iter, const struct lsm_key *key)
{
	struct rb_node *p = iter->tree->root;
	struct mtree_node *upper = NULL;

	while (p) {
		struct mtree_node * const node = (struct mtree_node *)p;
		const int cmp = iter->cmp(&node->key, key);

		if (cmp <= 0) {
			p = p->right;
			upper = node;
		} else {
			p = p->left;
		}
	}
	iter->node = upper;
}

void mtree_begin(struct mtree_iter *iter)
{
	iter->node = (struct mtree_node *)rb_leftmost(iter->tree);
}

void mtree_end(struct mtree_iter *iter)
{
	iter->node = NULL;
}

int mtree_lookup(struct mtree_iter *iter, const struct lsm_key *key)
{
	mtree_lower_bound(iter, key);
	if (iter->node && iter->cmp(&iter->node->key, key))
		iter->node = NULL;
	return iter->node ? 1 : 0;
}

int mtree_next(struct mtree_iter *iter)
{
	if (!iter->node)
		return -ENOENT;

	iter->node = (struct mtree_node *)rb_next(&iter->node->rb);
	return 0;
}

int mtree_prev(struct mtree_iter *iter)
{
	if (iter->node == (const struct mtree_node *)rb_leftmost(iter->tree))
		return -ENOENT;

	if (!iter->node)
		iter->node = (struct mtree_node *)rb_rightmost(iter->tree);
	else
		iter->node = (struct mtree_node *)rb_prev(&iter->node->rb);
	return 0;
}

int mtree_key(const struct mtree_iter *iter, struct lsm_key *key)
{
	if (key)
		memset(key, 0, sizeof(*key));
	if (!iter->node)
		return -ENOENT;
	if (key)
		*key = iter->node->key;
	return 0;
}

int mtree_val(const struct mtree_iter *iter, struct lsm_val *val)
{
	if (val)
		memset(val, 0, sizeof(*val));
	if (!iter->node)
		return -ENOENT;
	if (val)
		*val = iter->node->val;
	return 0;
}
