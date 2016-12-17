#ifndef __LOG_H__
#define __LOG_H__

#include <aulsmfs.h>
#include <alloc.h>
#include <io.h>

#include <stddef.h>


struct log_item {
	void *ptr;
	size_t size;
};

struct trans_log {
	struct io *io;
	struct alloc *alloc;

	struct aulsmfs_ptr *chunk;
	size_t chunks;
	size_t max_cunks;
	size_t pages;

	void *chunk_data;
	size_t chunk_size;
	size_t chunk_max_size;

	struct aulsmfs_log_header header;
};

void trans_log_setup(struct trans_log *log, struct io *io, struct alloc *alloc);
int trans_log_append(struct trans_log *log, const struct log_item *item);
int trans_log_finish(struct trans_log *log);
void trans_log_cancel(struct trans_log *log);
void trans_log_release(struct trans_log *log);

#endif /*__LOG_H__*/
