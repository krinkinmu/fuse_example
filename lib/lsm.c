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

	mtree_setup(&lsm->c0, lsm);
	mtree_setup(&lsm->c1, lsm);

	for (int i = 0; i != AULSMFS_MAX_DISK_TREES; ++i)
		ctree_setup(&lsm->ci[i], lsm);
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

	for (int i = 0; i + 2 >= iter->from && i + 2 <= iter->to; ++i) {
		ctree_key(&iter->iti[i], &iter->keyi[i]);
		ctree_val(&iter->iti[i], &iter->vali[i]);
	}
}

static int lsm_set_the_smallest(struct lsm_iter *iter)
{
	const struct lsm *const lsm = iter->lsm;

	struct lsm_key key = { NULL, 0 };
	struct lsm_val val = { NULL, 0 };

	iter->val.ptr = NULL;
	iter->val.size = 0;
	iter->key.ptr = NULL;
	iter->val.size = 0;

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

int lsm_set_the_largest(struct lsm_iter *iter)
{
	const struct lsm *const lsm = iter->lsm;

	struct lsm_key key = { NULL, 0 };
	struct lsm_val val = { NULL, 0 };

	iter->val.ptr = NULL;
	iter->val.size = 0;
	iter->key.ptr = NULL;
	iter->val.size = 0;

	for (int i = iter->from; i <= iter->to; ++i) {
		if (!iter->keyi[i].ptr)
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

	for (int i = 0; i + 2 >= iter->from && i + 2 <= iter->to; ++i) {
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

	for (int i = 0; i + 2 >= iter->from && i + 2 <= iter->to; ++i) {
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

	for (int i = 0; i + 2 >= iter->from && i + 2 <= iter->to; ++i) {
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
		mtree_val(&iter->it1, &iter->vali[0]);
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

	for (int i = 0; i + 2 >= iter->from && i + 2 <= iter->to; ++i) {
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

	const int rc = lsm_set_the_largest(iter);

	if (rc < 0)
		return rc;
	return moved ? 0 : -ENOENT;
}
