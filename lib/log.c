#include <log.h>
#include <crc64.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define TRANS_CHUNK_MAX_SIZE	(128 * 1024)


void trans_log_setup(struct trans_log *log, struct io *io, struct alloc *alloc)
{
	memset(log, 0, sizeof(*log));
	log->io = io;
	log->alloc = alloc;
}

void trasn_log_release(struct trans_log *log)
{
	free(log->header);
	free(log->chunk_data);
	memset(log, 0, sizeof(*log));
}

static int trans_log_reserve_chunk(struct trans_log *log, size_t count)
{
	const size_t need = log->chunks + count;

	if (need <= log->max_chunks)
		return 0;

	size_t chunks = log->max_chunks * 2 ? log->max_chunks * 2 : 16;

	if (chunks < need)
		chunks = need;

	const size_t size = chunks * sizeof(*log->chunk) +
				sizeof(*log->header);
	struct aulsmfs_log_header *header = realloc(log->chunk, size);

	if (!header)
		return -ENOMEM;
	log->header = header;
	log->chunk = (struct aulsmfs_ptr *)(header + 1);
	log->max_chunks = chunks;
	return 0;
}

static int trans_log_flush(struct trans_log *log)
{
	assert(io_align(log->io, log->chunk_max_size) == log->chunk_max_size);

	if (!log->chunk_size)
		return 0;

	int rc = trans_log_reserve_chunk(log, 1);

	if (rc < 0)
		return rc;

	const size_t pages = io_pages(log->io, log->chunk_size);
	const size_t bytes = io_bytes(log->io, pages);
	uint64_t offs;

	rc = alloc_reserve(log->alloc, pages, &offs);
	if (rc < 0)
		return rc;

	memset((char *)log->chunk_data + log->chunk_size, 0,
				bytes - log->chunk_size);
	rc = io_write(log->io, log->chunk_data, pages, offs);
	if (rc < 0) {
		assert(alloc_cancel(log->alloc, pages, offs) == 0);
		return rc;
	}

	struct aulsmfs_ptr *ptr = &log->chunk[log->chunks++];

	ptr->size = htole64(pages);
	ptr->offs = htole64(offs);
	ptr->csum = htole64(crc64(log->chunk_data, bytes));
	log->chunk_size = 0;
	log->pages += pages;
	return 0;
}

static int trans_log_reserve(struct trans_log *log, size_t size)
{
	const size_t need = log->chunk_size + size;

	if (need > TRANS_CHUNK_MAX_SIZE) {
		const int rc = trans_log_flush(log);

		if (rc < 0)
			return rc;
	}

	if (need <= log->chunk_max_size)
		return 0;

	size_t new_size = io_align(log->io, need);

	if (need < TRANS_CHUNK_MAX_SIZE) {
		const size_t size = io_align(log->io, log->chunk_max_size * 2);

		if (size <= TRANS_CHUNK_MAX_SIZE && size >= need)
			new_size = size;
	}

	void *data = realloc(log->chunk_data, new_size);

	if (!data)
		return -ENOMEM;
	log->chunk_data = data;
	log->chunk_max_size = new_size;
	return 0;
}

int trans_log_append(struct trans_log *log, const struct log_item *item)
{
	struct aulsmfs_log_entry *entry;
	const size_t size = item->size + sizeof(*entry);
	const int rc = trans_log_reserve(log, size);

	if (rc < 0)
		return rc;

	entry = (struct aulsmfs_log_entry *)((char *)log->chunk_data +
				log->chunk_size);
	entry->size = htole16(item->size);
	memcpy(entry + 1, item->ptr, item->size);
	log->chunk_size += size;
	return 0;
}

int trans_log_finish(struct trans_log *log)
{
	int rc = trans_log_flush(log);

	if (rc < 0)
		return rc;

	const size_t size = sizeof(*log->header) +
				log->chunks * sizeof(*log->chunk);
	const size_t pages = io_pages(log->io, size);
	const size_t bytes = io_bytes(log->io, pages);

	log->header->chunks = htole32(log->chunks);
	log->header->pages = htole32(log->pages);

	uint64_t offs;

	rc = alloc_reserve(log->alloc, pages, &offs);
	if (rc < 0)
		return rc;

	memset((char *)log->header + size, 0, bytes - size);
	rc = io_write(log->io, log->header, pages, offs);
	if (rc < 0) {
		assert(alloc_cancel(log->alloc, pages, offs) == 0);
		return rc;
	}

	log->ptr.offs = htole64(offs);
	log->ptr.size = htole64(pages);
	log->ptr.csum = crc64(log->header, bytes);
	return 0;
}

void trans_log_cancel(struct trans_log *log)
{
	for (size_t i = 0; i != log->chunks; ++i) {
		const struct aulsmfs_ptr *ptr = &log->chunk[i];
		const uint64_t offs = le64toh(ptr->offs);
		const uint64_t size = le64toh(ptr->size);

		assert(alloc_cancel(log->alloc, size, offs) == 0);
	}
}
