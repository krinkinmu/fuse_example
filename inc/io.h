#ifndef __AULSMFS_IO_H__
#define __AULSMFS_IO_H__

#include <sys/types.h>
#include <stddef.h>
#include <assert.h>


struct io;

/* All offsets and sizes are given in bytes. It's convenient to use bytes for
 * low-level io operations. */
struct io_ops {
	int (*read)(struct io *, void *, size_t, off_t);
	int (*write)(struct io *, const void *, size_t, off_t);
	int (*sync)(struct io *);
};

struct io {
	struct io_ops *ops;
	size_t page_size;
};

inline void io_setup(struct io *io, struct io_ops *ops, size_t page_size)
{
	assert(ops && "IO ops must not be NULL");
	assert(ops->read && "No read operation provided");

	io->ops = ops;
	io->page_size = page_size;
}

/* All offsets are given in pages, beacuse file system works with pages we
 * provide API that uses pages as bas io unit instead of bytes. */
inline int io_read(struct io *io, void *buf, size_t size, uint64_t off)
{
	struct io_ops * const ops = io->ops;

	return ops->read(io, buf, size * io->page_size, off * io->page_size);
}

inline int io_write(struct io *io, const void *buf, size_t size, uint64_t off)
{
	struct io_ops * const ops = io->ops;

	assert(ops->write && "No write operation provided");
	return ops->write(io, buf, size * io->page_size, off * io->page_size);
}

inline int io_sync(struct io *io)
{
	struct io_ops * const ops = io->ops;

	assert(ops->sync && "No sync operation provided");
	return ops->sync(io);
}

#endif /*__AULSMFS_IO_H__*/
