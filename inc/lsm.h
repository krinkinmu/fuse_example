#ifndef __LSM_H__
#define __LSM_H__

#include <aulsmfs.h>

#include <stddef.h>

struct lsm_config {
	uint64_t page_size;
	uint64_t log_pages;
	int fd;
};

struct lsm_log {
	const struct lsm_config *config;

	/* Offset the log area in bytes. */
	uint64_t offs;

	/* Generation of the next log entry. */
	uint64_t gen;

	/* Position of the next entry in the log. */
	size_t next_pos;

	/* Current size of the user data in bytes. */
	size_t next_size;

	void *buf;
};

void lsm_log_create(struct lsm_log *log, const struct lsm_config *config,
			uint64_t offs, uint64_t gen);
void lsm_log_destroy(struct lsm_log *log);
size_t lsm_log_remains(const struct lsm_log *log);
size_t lsm_log_size(const struct lsm_log *log);
int lsm_log_append(struct lsm_log *log, const void *data, size_t size);
int lsm_log_checkpoint(struct lsm_log *log);

#endif /*__LSM_H__*/
