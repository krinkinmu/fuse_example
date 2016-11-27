#include <lsm.h>

#include <string.h>

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

int lsm_begin(struct lsm_iter *iter)
{
	mtree_begin(&iter->it0);
	mtree_begin(&iter->it1);

	for (int i = 0; i != AULSMFS_MAX_DISK_TREES; ++i) {
		const int rc = ctree_begin(&iter->iti[i]);

		if (rc < 0)
			return rc;
	}
	return 0;
}

int lsm_end(struct lsm_iter *iter)
{
	mtree_end(&iter->it0);
	mtree_end(&iter->it1);

	for (int i = 0; i != AULSMFS_MAX_DISK_TREES; ++i) {
		const int rc = ctree_end(&iter->iti[i]);

		if (rc < 0)
			return rc;
	}
	return 0;
}
