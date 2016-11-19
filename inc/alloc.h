#ifndef __ALLOC_H__
#define __ALLOC_H__

#include <stdint.h>

struct alloc;

struct alloc_ops {
	/* Reserves disk space, space may not be used by anyone else, but
	 * information about this reservation is not stored permamemtly and
	 * so won't survive crashes/power failures/restarts and so on.
	 * On the other hand we can reserve as much space as we want and write
	 * it, and only preserve information about reserved space only when
	 * we finished, or we can use reserved space as a temporary storage. */
	int (*reserve)(struct alloc *, uint64_t /*size*/, uint64_t * /*offs*/);

	/* This cancels previous reservation. */
	int (*cancel)(struct alloc *, uint64_t /*offs*/, uint64_t /*size*/);

	/* This makes reservation persistent, IOW marks reserved range as
	 * busy. So to allocate space we need at first reserve required
	 * amount of space and then commit that reservation. */
	int (*commit)(struct alloc *, uint64_t /*offs*/, uint64_t /*size*/);

	/* Frees previsously busy range. */
	int (*free)(struct alloc *, uint64_t /*offs*/, uint64_t /*free*/);
};

struct alloc {
	struct alloc_ops *ops;
};

static inline int alloc_reserve(struct alloc *alloc, uint64_t size,
			uint64_t *offs)
{
	return alloc->ops->reserve(alloc, size, offs);
}

static inline int alloc_cancel(struct alloc *alloc, uint64_t offs,
			uint64_t size)
{
	return alloc->ops->cancel(alloc, offs, size);
}

static inline int alloc_commit(struct alloc *alloc, uint64_t offs,
			uint64_t size)
{
	return alloc->ops->commit(alloc, offs, size);
}

static inline int alloc_free(struct alloc *alloc, uint64_t offs,
			uint64_t size)
{
	return alloc->ops->free(alloc, offs, size);
}

#endif /*__ALLOC_H__*/
