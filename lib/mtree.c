#include <mtree.h>
#include <lsm.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>


struct mtree_node {
	struct rb_node rb;
	struct lsm_key key;
	struct lsm_val val;
	int deleted;
};

static struct mtree_node *mtree_node_create(int deleted,
			const struct lsm_key *key, const struct lsm_val *val)
{
	const size_t size = key->size + val->size + sizeof(struct mtree_node);
	struct mtree_node * const new = malloc(size);

	if (!new)
		return NULL;

	char *const key_ptr = (char *)(new + 1);
	char *const val_ptr = key_ptr + key->size;

	new->key.ptr = key_ptr;
	new->key.size = key->size;
	memcpy(new->key.ptr, key->ptr, new->key.size);

	new->val.ptr = val_ptr;
	new->val.size = val->size;
	memcpy(new->val.ptr, val->ptr, new->val.size);

	new->deleted = deleted;
	return new;
}

static void mtree_node_destroy(struct mtree_node *node)
{
	free(node);
}

void mtree_setup(struct mtree *tree, struct lsm *lsm)
{
	tree->lsm = lsm;
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
	tree->lsm = NULL;
	tree->tree.root = NULL;
}

static void __mtree_insert(struct mtree *tree, struct mtree_node *new)
{
	struct lsm *const lsm = tree->lsm;
	struct rb_node **plink = &tree->tree.root;
	struct rb_node *parent = NULL;

	while (*plink) {
		struct mtree_node * const old = (struct mtree_node *)(*plink);
		const int cmp = lsm->cmp(&old->key, &new->key);

		if (!cmp) {
			rb_swap_nodes(&tree->tree, &old->rb, &new->rb);
			mtree_node_destroy(old);
			return;
		}

		parent = *plink;
		if (cmp < 0)
			plink = &parent->left;
		else
			plink = &parent->right;
	}

	rb_link(&new->rb, parent, plink);
	rb_insert(&new->rb, &tree->tree);
}

int mtree_add(struct mtree *tree, const struct lsm_key *key,
			const struct lsm_val *val)
{
	struct mtree_node *new = mtree_node_create(0, key, val);

	if (!new)
		return -ENOMEM;

	__mtree_insert(tree, new);
	return 0;
}

int mtree_del(struct mtree *tree, const struct lsm_key *key)
{
	static const struct lsm_val empty = { NULL, 0 };
	struct mtree_node *new = mtree_node_create(1, key, &empty);

	if (!new)
		return -ENOMEM;

	__mtree_insert(tree, new);
	return 0;
}

void mtree_iter_setup(struct mtree_iter *iter, struct mtree *tree)
{
	iter->lsm = tree->lsm;
	iter->tree = &tree->tree;
	iter->node = NULL;
}

void mtree_iter_release(struct mtree_iter *iter)
{
	iter->lsm = NULL;
	iter->tree = NULL;
	iter->node = NULL;
}

void mtree_lower_bound(struct mtree_iter *iter, const struct lsm_key *key)
{
	const struct lsm * const lsm = iter->lsm;
	struct rb_node *p = iter->tree->root;
	struct mtree_node *lower = NULL;

	while (p) {
		struct mtree_node * const node = (struct mtree_node *)p;
		const int cmp = lsm->cmp(&node->key, key);

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
	const struct lsm * const lsm = iter->lsm;
	struct rb_node *p = iter->tree->root;
	struct mtree_node *upper = NULL;

	while (p) {
		struct mtree_node * const node = (struct mtree_node *)p;
		const int cmp = lsm->cmp(&node->key, key);

		if (cmp <= 0) {
			p = p->right;
			upper = node;
		} else {
			p = p->left;
		}
	}
	iter->node = upper;
}

int mtree_lookup(struct mtree_iter *iter, const struct lsm_key *key)
{
	const struct lsm * const lsm = iter->lsm;

	mtree_lower_bound(iter, key);
	if (iter->node && lsm->cmp(&iter->node->key, key))
		iter->node = NULL;
	return iter->node ? 1 : 0;
}

int mtree_next(struct mtree_iter *iter)
{
	if (!iter->node)
		iter->node = (struct mtree_node *)rb_leftmost(iter->tree);
	else
		iter->node = (struct mtree_node *)rb_next(&iter->node->rb);
	return iter->node ? 1 : 0;
}

int mtree_prev(struct mtree_iter *iter)
{
	if (!iter->node)
		iter->node = (struct mtree_node *)rb_rightmost(iter->tree);
	else
		iter->node = (struct mtree_node *)rb_prev(&iter->node->rb);
	return iter->node ? 1 : 0;
}
