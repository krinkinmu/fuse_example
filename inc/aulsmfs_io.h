#ifndef __AULSMFS_IO_H__
#define __AULSMFS_IO_H__

#include <sys/types.h>
#include <stddef.h>
#include <assert.h>


struct aulsmfs_io;

/* All offsets and sizes are given in bytes. It's convenient to use bytes for
 * low-level io operations. */
struct aulsmfs_io_ops {
	int (*read)(struct aulsmfs_io *, void *, size_t, off_t);
	int (*write)(struct aulsmfs_io *, const void *, size_t, off_t);
	int (*sync)(struct aulsmfs_io *);
};

struct aulsmfs_io {
	const struct aulsmfs_io_ops *ops;
	size_t page_size;
};

static inline void aulsmfs_io_setup(struct aulsmfs_io *io,
			struct aulsmfs_io_ops *ops, size_t page_size)
{
	assert(ops && "IO ops must not be NULL");
	assert(ops->read && "No read operation provided");

	io->ops = ops;
	io->page_size = page_size;
}

/* All offsets are given in pages, beacuse file system works with pages we
 * provide API that uses pages as bas io unit instead of bytes. */
static inline int aulsmfs_io_read(struct aulsmfs_io *io,
			void *buf, size_t size,
			uint64_t off)
{
	return io->ops->read(io, buf,
				size * io->page_size, off * io->page_size);
}

static inline int aulsmfs_io_write(struct aulsmfs_io *io,
			const void *buf, size_t size,
			uint64_t off)
{
	assert(io->ops->write && "No write operation provided");
	return io->ops->write(io, buf,
				size * io->page_size, off * io->page_size);
}

static inline int auslmfs_io_sync(struct aulsmfs_io *io)
{
	assert(io->ops->sync && "No sync operation provided");
	return io->ops->sync(io);
}

#endif /*__AULSMFS_IO_H__*/
