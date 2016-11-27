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
		memset(&iter->keyi[0], 0, sizeof(iter->keyi[0]));
		memset(&iter->vali[0], 0, sizeof(iter->vali[0]));

		mtree_key(&iter->it0, &iter->keyi[0]);
		mtree_val(&iter->it0, &iter->vali[0]);
	}

	if (iter->from <= 1 && iter->to >= 1) {
		memset(&iter->keyi[1], 0, sizeof(iter->keyi[1]));
		memset(&iter->vali[1], 0, sizeof(iter->vali[1]));

		mtree_key(&iter->it1, &iter->keyi[1]);
		mtree_val(&iter->it1, &iter->vali[1]);
	}

	for (int i = 0; i + 2 >= iter->from && i + 2 <= iter->to; ++i) {
		memset(&iter->keyi[i + 2], 0, sizeof(iter->keyi[i + 2]));
		memset(&iter->vali[i + 2], 0, sizeof(iter->vali[i + 2]));

		ctree_key(&iter->iti[i], &iter->keyi[i]);
		ctree_val(&iter->iti[i], &iter->vali[i]);
	}
}

static int lsm_set_the_smallest(struct lsm_iter *iter)
{
	const struct lsm *const lsm = iter->lsm;

	struct lsm_key key = { NULL, 0 };
	struct lsm_val val = { NULL, 0 };

	lsm_set_items(iter);
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

	lsm_set_items(iter);
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
	return 0;
}
