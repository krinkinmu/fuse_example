#ifndef __LSM_H__
#define __LSM_H__

#include <aulsmfs.h>
#include <aulsmfs_io.h>

#include <stddef.h>

struct aulsmfs_lsm_log {
	struct aulsmfs_io *io;

	/* Offset and size of the log area. */
	uint64_t offs, size;

	/* Generation of the next log entry. */
	uint64_t gen;

	/* Position of the next entry in the log. */
	size_t next_pos;

	/* Current size of the user data in bytes. */
	size_t next_size;

	void *buf;
};

void aulsmfs_lsm_log_create(struct aulsmfs_lsm_log *log,
			struct aulsmfs_io *io, uint64_t gen,
			uint64_t offs, uint64_t size);
void auslsmfs_lsm_log_destroy(struct aulsmfs_lsm_log *log);
size_t aulsmfs_lsm_log_remains(const struct aulsmfs_lsm_log *log);
size_t aulsmfs_lsm_log_size(const struct aulsmfs_lsm_log *log);
int aulsmfs_lsm_log_append(struct aulsmfs_lsm_log *log,
			const void *data, size_t size);

/* Checkpoint operation doesn't preform sync because it's enought to do it just
 * before filesystem root writing and maybe, to ensure commit persistancy, right
 * after root has been written. */
int aulsmfs_lsm_log_checkpoint(struct aulsmfs_lsm_log *log);

#endif /*__LSM_H__*/
