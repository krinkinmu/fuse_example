#include <lsm.h>
#include <crc64.h>

#include <endian.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>


static uint64_t aligndown(uint64_t addr, uint64_t align)
{
	assert((align & (align - 1)) == 0 && "alignment must be power of two");
	return addr & ~(uint64_t)(align - 1);
}

static uint64_t alignup(uint64_t addr, uint64_t align)
{
	return aligndown(addr + align - 1, align);
}

void aulsmfs_lsm_log_create(struct aulsmfs_lsm_log *log,
			struct aulsmfs_io *io, uint64_t gen,
			uint64_t offs, uint64_t size)
{
	const size_t buf_size = size * io->page_size;

	log->offs = offs;
	log->gen = gen;

	log->next_pos = 0;
	log->next_size = sizeof(struct aulsmfs_log_entry);

	log->buf = malloc(buf_size);
	assert(log->buf && "Log buffer allocation failed");
	memset(log->buf, 0, buf_size);
}

void aulsmfs_lsm_log_destroy(struct aulsmfs_lsm_log *log)
{
	free(log->buf);

	log->offs = log->gen = 0;
	log->next_pos = log->next_size = 0;
	log->buf = 0;
}

size_t aulsmfs_lsm_log_remains(const struct aulsmfs_lsm_log *log)
{
	const size_t page_size = log->io->page_size;

	return (log->size - log->next_pos) * page_size - log->next_size;
}

size_t aulsmfs_lsm_log_size(const struct aulsmfs_lsm_log *log)
{
	return alignup(log->next_size, log->io->page_size);
}

int aulsmfs_lsm_log_append(struct aulsmfs_lsm_log *log,
			const void *data, size_t size)
{
	if (size > aulsmfs_lsm_log_remains(log))
		return -1;

	memcpy((char *)log->buf + log->next_size, data, size);
	log->next_size += size;
	return 0;
}

int aulsmfs_lsm_log_checkpoint(struct aulsmfs_lsm_log *log)
{
	struct aulsmfs_io * const io = log->io;
	const size_t page_size = io->page_size;

	const size_t size = alignup(log->next_size, page_size);
	const uint64_t offs = log->offs + log->next_pos;

	struct aulsmfs_log_entry *entry = log->buf;

	entry->gen = htole64(log->gen);
	entry->size = htole64(log->next_size);
	entry->csum = 0;

	memset((char *)log->buf + log->next_size, 0, size - log->next_size);
	entry->csum = htole64(crc64(log->buf, size));

	if (aulsmfs_io_write(io, log->buf, size / page_size, offs) < 0)
		return -1;

	log->next_size = sizeof(*entry);
	log->next_pos += size / page_size;
	++log->gen;
	return 0;
}
