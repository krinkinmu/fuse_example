#include <lsm.h>
#include <file_wrappers.h>
#include <crc64.h>

#include <endian.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>


static uint64_t aligndown(const struct lsm_config *config, uint64_t addr)
{
	return addr & ~(uint64_t)(config->page_size - 1);
}

static uint64_t alignup(const struct lsm_config *config, uint64_t addr)
{
	return aligndown(config, addr + config->page_size - 1);
}

void lsm_log_create(struct lsm_log *log, const struct lsm_config *config,
			uint64_t offs, uint64_t gen)
{
	const size_t buf_size = config->log_pages * config->page_size;

	log->offs = offs * config->page_size;
	log->gen = gen;

	log->next_pos = 0;
	log->next_size = sizeof(struct aulsmfs_log_entry);

	log->buf = malloc(buf_size);
	assert(log->buf && "Log buffer allocation failed");
	memset(log->buf, 0, buf_size);
}

void lsm_log_destroy(struct lsm_log *log)
{
	free(log->buf);

	log->offs = log->gen = 0;
	log->next_pos = log->next_size = 0;
	log->buf = 0;
}

size_t lsm_log_remains(const struct lsm_log *log)
{
	const size_t pages = log->config->log_pages;
	const size_t bytes = pages * log->config->page_size;

	return bytes - log->next_pos - log->next_size;
}

size_t lsm_log_size(const struct lsm_log *log)
{
	return alignup(log->config, log->next_size);
}

int lsm_log_append(struct lsm_log *log, const void *data, size_t size)
{
	if (size > lsm_log_remains(log))
		return -1;

	memcpy((char *)log->buf + log->next_size, data, size);
	log->next_size += size;
	return 0;
}

int lsm_log_checkpoint(struct lsm_log *log)
{
	const size_t size = alignup(log->config, log->next_size);
	const uint64_t offs = log->offs + log->next_pos;
	struct aulsmfs_log_entry *entry = log->buf;

	entry->gen = htole64(log->gen);
	entry->size = htole64(log->next_size);
	entry->csum = 0;

	memset((char *)log->buf + log->next_size, 0, size - log->next_size);
	entry->csum = htole64(crc64(log->buf, size));

	if (file_write_at(log->config->fd, log->buf, size, offs) < 0)
		return -1;

	log->next_size = sizeof(*entry);
	log->next_pos += size;
	++log->gen;
	return 0;
}
