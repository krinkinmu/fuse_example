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
