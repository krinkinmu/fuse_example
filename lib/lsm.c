#include <lsm.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>

void lsm_setup(struct lsm *lsm, struct io *io, struct alloc *alloc,
		int (*cmp)(const struct lsm_key *, const struct lsm_key *))
{
	memset(lsm, 0, sizeof(*lsm));
	lsm->io = io;
	lsm->alloc = alloc;
	lsm->cmp = cmp;

	mtree_setup(&lsm->c0, cmp);
	mtree_setup(&lsm->c1, cmp);

	for (int i = 0; i != AULSMFS_MAX_DISK_TREES; ++i)
		ctree_setup(&lsm->ci[i], io, cmp);
}

void lsm_release(struct lsm *lsm)
{
	for (int i = 0; i != AULSMFS_MAX_DISK_TREES; ++i)
		ctree_release(&lsm->ci[i]);

	mtree_release(&lsm->c1);
	mtree_release(&lsm->c0);
	memset(lsm, 0, sizeof(*lsm));
}

void lsm_parse(struct lsm *lsm, const struct aulsmfs_tree *ondisk)
{
	for (int i = 0; i != AULSMFS_MAX_DISK_TREES; ++i)
		ctree_parse(&lsm->ci[i], &ondisk->ci[i]);
}

void lsm_dump(const struct lsm *lsm, struct aulsmfs_tree *ondisk)
{
	for (int i = 0; i != AULSMFS_MAX_DISK_TREES; ++i)
		ctree_dump(&lsm->ci[i], &ondisk->ci[i]);
}

int lsm_add(struct lsm *lsm, const struct lsm_key *key,
			const struct lsm_val *val)
{
	return mtree_add(&lsm->c0, key, val);
}

static int lsm_build_default(struct lsm_merge_policy *policy)
{
	const int drop = policy->drop_deleted;

	struct lsm_iter *iter = &policy->iter;
	struct ctree_builder *builder = &policy->builder;

	while (lsm_has_item(iter)) {
		const struct lsm_key key = iter->key;
		const struct lsm_val val = iter->val;
		int rc;

		if (!drop || !policy->deleted(policy, &key, &val)) {
			rc = ctree_builder_append(builder, &key, &val);
			if (rc < 0)
				return rc;
		}

		rc = lsm_next(iter);
		if (rc < 0 && rc != -ENOENT)
			return rc;
	}
	return 0;
}

static int lsm_call_build(struct lsm_merge_policy *policy)
{
	struct ctree_builder *builder = &policy->builder;
	struct lsm_iter *iter = &policy->iter;
	int rc = lsm_begin(iter);

	if (rc < 0)
		return rc;

	rc = policy->build(policy);
	if (rc < 0)
		return rc;

	return ctree_builder_finish(builder);
}

static int lsm_drop_deleted(struct lsm_merge_policy *policy)
{
	struct lsm *lsm = policy->lsm;
	const int from = policy->tree + 2;
	const int to = AULSMFS_MAX_DISK_TREES + 2;

	for (int i = from; i < to; ++i) {
		if (!ctree_is_empty(&lsm->ci[i - 2]))
			return 0;
	}
	return 1;
}

static int __lsm_merge(struct lsm_merge_policy *policy)
{
	struct lsm *lsm = policy->lsm;
	struct lsm_iter *iter = &policy->iter;
	struct ctree_builder *builder = &policy->builder;

	const int from = policy->tree;
	const int to = policy->tree + 1;
	int rc;

	policy->drop_deleted = lsm_drop_deleted(policy);
	ctree_builder_setup(builder, lsm->io, lsm->alloc);
	lsm_iter_setup(iter, lsm);
	iter->from = from;
	iter->to = to;

	rc = lsm_call_build(policy);
	lsm_iter_release(iter);
	if (rc < 0) {
		ctree_builder_cancel(builder);
		ctree_builder_release(builder);
		return rc;
	}

	rc = ctree_reset(&lsm->ci[to - 2], &builder->ptr, builder->height,
				builder->pages);
	if (rc < 0) {
		ctree_builder_cancel(builder);
		ctree_builder_release(builder);
		return rc;
	}
	ctree_builder_release(builder);

	for (int i = to - 1; i >= from; --i) {
		assert(i > 0);

		if (i == 1) {
			mtree_reset(&lsm->c1);
			continue;
		}
		ctree_reset(&lsm->ci[i - 2], NULL, 0, 0);
	}
	return 0;
}

int lsm_merge(struct lsm *lsm, int tree, struct lsm_merge_policy *policy)
{
	policy->lsm = lsm;
	policy->tree = tree;

	if (!policy->tree) {
		assert(mtree_is_empty(&lsm->c1));
		mtree_swap(&lsm->c0, &lsm->c1);
		policy->tree = 1;

		const int rc = __lsm_merge(policy);

		/* I don't know what can we do here if __lsm_merge failed,
		 * we can't just drop the c1, and we can't return it back
		 * to c0, since it might not be empty by this time. All
		 * in all, we, probably, can only mark this tree as read
		 * only and whole filesystem as well, or just try again
		 * later, i'll go with trying again later. */
		return rc;
	}

	/* The next tree is empty, we can just swap these threes. */
	if (ctree_is_empty(&lsm->ci[tree - 1])) {
		ctree_swap(&lsm->ci[tree - 1], &lsm->ci[tree - 2]);
		return 0;
	}

	return __lsm_merge(policy);
}

static int lsm_no_delete(struct lsm_merge_policy *policy,
			const struct lsm_key *key, const struct lsm_val *val)
{
	(void) policy;
	(void) key;
	(void) val;
	return 0;
}

void lsm_merge_policy_setup(struct lsm_merge_policy *policy)
{
	memset(policy, 0, sizeof(*policy));
	policy->deleted = &lsm_no_delete;
	policy->build = &lsm_build_default;
}

void lsm_merge_policy_release(struct lsm_merge_policy *policy)
{
	memset(policy, 0, sizeof(*policy));
}


void lsm_iter_setup(struct lsm_iter *iter, struct lsm *lsm)
{
	memset(iter, 0, sizeof(*iter));
	iter->lsm = lsm;
	iter->from = 0;
	iter->to = AULSMFS_MAX_DISK_TREES + 1;

	mtree_iter_setup(&iter->it0, &lsm->c0);
	mtree_iter_setup(&iter->it1, &lsm->c1);

	for (int i = 0; i != AULSMFS_MAX_DISK_TREES; ++i)
		ctree_iter_setup(&iter->iti[i], &lsm->ci[i]);
}

void lsm_iter_release(struct lsm_iter *iter)
{
	for (int i = 0; i != AULSMFS_MAX_DISK_TREES; ++i)
		ctree_iter_release(&iter->iti[i]);

	mtree_iter_release(&iter->it1);
	mtree_iter_release(&iter->it0);
	free(iter->buf);
	memset(iter, 0, sizeof(*iter));
}

static void lsm_set_items(struct lsm_iter *iter)
{
	if (!iter->from) {
		mtree_key(&iter->it0, &iter->keyi[0]);
		mtree_val(&iter->it0, &iter->vali[0]);
	}

	if (iter->from <= 1 && iter->to >= 1) {
		mtree_key(&iter->it1, &iter->keyi[1]);
		mtree_val(&iter->it1, &iter->vali[1]);
	}

	const int from = iter->from < 2 ? 0 : iter->from - 2;
	const int to = iter->to < 2 ? 0 : iter->to - 1;

	for (int i = from; i < to; ++i) {
		ctree_key(&iter->iti[i], &iter->keyi[i + 2]);
		ctree_val(&iter->iti[i], &iter->vali[i + 2]);
	}
}

static int lsm_set_the_smallest(struct lsm_iter *iter)
{
	const struct lsm *const lsm = iter->lsm;

	struct lsm_key key = { NULL, 0 };
	struct lsm_val val = { NULL, 0 };

	memset(&iter->val, 0, sizeof(iter->val));
	memset(&iter->key, 0, sizeof(iter->key));

	for (int i = iter->from; i <= iter->to; ++i) {
		if (!iter->keyi[i].ptr)
			continue;

		if (!key.ptr) {
			key = iter->keyi[i];
			val = iter->vali[i];
			continue;
		}

		if (lsm->cmp(&key, &iter->keyi[i]) > 0) {
			key = iter->keyi[i];
			val = iter->vali[i];
		}
	}

	if (key.ptr) {
		const size_t size = key.size + val.size;

		if (size > iter->buf_size) {
			void *buf = realloc(iter->buf, size);

			if (!buf)
				return -ENOMEM;
			iter->buf = buf;
			iter->buf_size = size;
		}

		char *key_ptr = iter->buf;
		char *val_ptr = key_ptr + key.size;

		memcpy(key_ptr, key.ptr, key.size);
		memcpy(val_ptr, val.ptr, val.size);

		iter->key.ptr = key_ptr;
		iter->key.size = key.size;

		iter->val.ptr = val_ptr;
		iter->val.size = val.size;
	}
	return 0;
}

int lsm_set_prev(struct lsm_iter *iter)
{
	const struct lsm *const lsm = iter->lsm;
	const struct lsm_key last = iter->key;

	struct lsm_key key = { NULL, 0 };
	struct lsm_val val = { NULL, 0 };

	memset(&iter->val, 0, sizeof(iter->val));
	memset(&iter->key, 0, sizeof(iter->key));

	for (int i = iter->from; i <= iter->to; ++i) {
		if (!iter->keyi[i].ptr)
			continue;

		/* Ignore keys that are larger than previous visited
		 * key, since it might be the smallest key in the
		 * respective tree and we just can't move further back. */
		if (last.ptr && lsm->cmp(&last, &iter->keyi[i]) <= 0)
			continue;

		if (!key.ptr) {
			key = iter->keyi[i];
			val = iter->vali[i];
			continue;
		}

		if (lsm->cmp(&key, &iter->keyi[i]) < 0) {
			key = iter->keyi[i];
			val = iter->vali[i];
		}
	}

	if (key.ptr) {
		const size_t size = key.size + val.size;

		if (size > iter->buf_size) {
			void *buf = realloc(iter->buf, size);

			if (!buf)
				return -ENOMEM;
			iter->buf = buf;
			iter->buf_size = size;
		}

		char *key_ptr = iter->buf;
		char *val_ptr = key_ptr + key.size;

		memcpy(key_ptr, key.ptr, key.size);
		memcpy(val_ptr, val.ptr, val.size);

		iter->key.ptr = key_ptr;
		iter->key.size = key.size;

		iter->val.ptr = val_ptr;
		iter->val.size = val.size;
	}
	return 0;
}

int lsm_begin(struct lsm_iter *iter)
{
	if (!iter->from)
		mtree_begin(&iter->it0);

	if (iter->from <= 1 && iter->to >= 1)
		mtree_begin(&iter->it1);

	const int from = iter->from < 2 ? 0 : iter->from - 2;
	const int to = iter->to < 2 ? 0 : iter->to - 1;

	for (int i = from; i < to; ++i) {
		const int rc = ctree_begin(&iter->iti[i]);

		if (rc < 0)
			return rc;
	}
	lsm_set_items(iter);
	return lsm_set_the_smallest(iter);
}

int lsm_end(struct lsm_iter *iter)
{
	if (!iter->from)
		mtree_end(&iter->it0);

	if (iter->from <= 1 && iter->to >= 1)
		mtree_end(&iter->it1);

	const int from = iter->from < 2 ? 0 : iter->from - 2;
	const int to = iter->to < 2 ? 0 : iter->to - 1;

	for (int i = from; i < to; ++i) {
		const int rc = ctree_end(&iter->iti[i]);

		if (rc < 0)
			return rc;
	}
	lsm_set_items(iter);
	return 0;
}

int lsm_next(struct lsm_iter *iter)
{
	const struct lsm *const lsm = iter->lsm;
	struct lsm_key *key = &iter->key;
	struct lsm_val *val = &iter->val;

	if (!key->ptr)
		return -ENOENT;

	/* We don't really need a loop here since iter->from isn't going
	 * to be changed, but using loop we can use break and continue, to
	 * avoid ugly branches. */
	while (!iter->from) {
		if (!iter->keyi[0].ptr)
			break;

		if (lsm->cmp(&iter->keyi[0], key) > 0)
			break;

		mtree_next(&iter->it0);
		mtree_key(&iter->it0, &iter->keyi[0]);
		mtree_val(&iter->it0, &iter->vali[0]);
	}

	while (iter->from <= 1 && iter->to >= 1) {
		if (!iter->keyi[1].ptr)
			break;

		if (lsm->cmp(&iter->keyi[1], key) > 0)
			break;

		mtree_next(&iter->it1);
		mtree_key(&iter->it1, &iter->keyi[1]);
		mtree_val(&iter->it1, &iter->vali[1]);
	}

	const int from = iter->from < 2 ? 0 : iter->from - 2;
	const int to = iter->to < 2 ? 0 : iter->to - 1;

	for (int i = from; i < to; ++i) {
		struct lsm_key *const keyi = &iter->keyi[i + 2];
		struct lsm_val *const vali = &iter->vali[i + 2];

		while (1) {
			if (!keyi->ptr)
				break;

			if (lsm->cmp(keyi, key) > 0)
				break;

			const int rc = ctree_next(&iter->iti[i]);

			if (rc < 0 && rc != -ENOENT)
				return rc;
			ctree_key(&iter->iti[i], keyi);
			ctree_val(&iter->iti[i], vali);
		}
	}

	for (int i = iter->from; i <= iter->to; ++i) {
		if (!iter->keyi[i].ptr)
			continue;
		return lsm_set_the_smallest(iter);
	}
	memset(key, 0, sizeof(*key));
	memset(val, 0, sizeof(*val));
	return -ENOENT;
}

int lsm_prev(struct lsm_iter *iter)
{
	const struct lsm *const lsm = iter->lsm;
	struct lsm_key *key = &iter->key;
	int moved = 0;

	while (!iter->from) {
		if (iter->keyi[0].ptr && !key->ptr) {
			moved = 1;
			break;
		}

		if (iter->keyi[0].ptr && lsm->cmp(&iter->keyi[0], key) < 0) {
			moved = 1;
			break;
		}

		const int rc = mtree_prev(&iter->it0);

		mtree_key(&iter->it0, &iter->keyi[0]);
		mtree_val(&iter->it0, &iter->vali[0]);
		if (rc < 0)
			break;
	}

	while (iter->from <= 1 && iter->to >= 1) {
		if (iter->keyi[1].ptr && !key->ptr) {
			moved = 1;
			break;
		}

		if (iter->keyi[1].ptr && lsm->cmp(&iter->keyi[1], key) < 0) {
			moved = 1;
			break;
		}

		const int rc = mtree_prev(&iter->it1);

		mtree_key(&iter->it1, &iter->keyi[1]);
		mtree_val(&iter->it1, &iter->vali[1]);
		if (rc < 0)
			break;
	}

	const int from = iter->from < 2 ? 0 : iter->from - 2;
	const int to = iter->to < 2 ? 0 : iter->to - 1;

	for (int i = from; i < to; ++i) {
		struct lsm_key *const keyi = &iter->keyi[i + 2];
		struct lsm_val *const vali = &iter->vali[i + 2];

		while (1) {
			if (keyi->ptr && !key->ptr) {
				moved = 1;
				break;
			}

			if (keyi->ptr && lsm->cmp(keyi, key) < 0) {
				moved = 1;
				break;
			}

			const int rc = ctree_prev(&iter->iti[i]);

			if (rc == -ENOENT)
				break;
			if (rc < 0)
				return rc;
			ctree_key(&iter->iti[i], keyi);
			ctree_val(&iter->iti[i], vali);
		}
	}

	if (!moved)
		return -ENOENT;
	return lsm_set_prev(iter);
}

int lsm_has_item(const struct lsm_iter *iter)
{
	return iter->key.ptr ? 1 : 0;
}

int lsm_lower_bound(struct lsm_iter *iter, const struct lsm_key *key)
{
	if (!iter->from) {
		mtree_lower_bound(&iter->it0, key);
		mtree_key(&iter->it0, &iter->keyi[0]);
		mtree_val(&iter->it0, &iter->vali[0]);
	}

	if (iter->from <= 1 && iter->to >= 1) {
		mtree_lower_bound(&iter->it1, key);
		mtree_key(&iter->it1, &iter->keyi[1]);
		mtree_val(&iter->it1, &iter->vali[1]);
	}

	const int from = iter->from < 2 ? 0 : iter->from - 2;
	const int to = iter->to < 2 ? 0 : iter->to - 1;

	for (int i = from; i < to; ++i) {
		const int rc = ctree_lower_bound(&iter->iti[i], key);

		if (rc < 0)
			return rc;

		ctree_key(&iter->iti[i], &iter->keyi[i + 2]);
		ctree_val(&iter->iti[i], &iter->vali[i + 2]);
	}
	return lsm_set_the_smallest(iter);
}

int lsm_upper_bound(struct lsm_iter *iter, const struct lsm_key *key)
{
	if (!iter->from) {
		mtree_upper_bound(&iter->it0, key);
		mtree_key(&iter->it0, &iter->keyi[0]);
		mtree_val(&iter->it0, &iter->vali[0]);
	}

	if (iter->from <= 1 && iter->to >= 1) {
		mtree_upper_bound(&iter->it1, key);
		mtree_key(&iter->it1, &iter->keyi[1]);
		mtree_val(&iter->it1, &iter->vali[1]);
	}

	const int from = iter->from < 2 ? 0 : iter->from - 2;
	const int to = iter->to < 2 ? 0 : iter->to - 1;

	for (int i = from; i < to; ++i) {
		const int rc = ctree_upper_bound(&iter->iti[i], key);

		if (rc < 0)
			return rc;

		ctree_key(&iter->iti[i], &iter->keyi[i + 2]);
		ctree_val(&iter->iti[i], &iter->vali[i + 2]);
	}
	return lsm_set_the_smallest(iter);
}

int lsm_lookup(struct lsm_iter *iter, const struct lsm_key *key)
{
	const int rc = lsm_lower_bound(iter, key);

	if (rc < 0)
		return rc;

	if (!iter->key.ptr)
		return 0;

	if (iter->lsm->cmp(&iter->key, key))
		return 0;
	return 1;
}
