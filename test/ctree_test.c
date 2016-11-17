#include <lsm.h>
#include <file_wrappers.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdint.h>
#include <stdio.h>


struct ctree_test_alloc {
	struct lsm_alloc alloc;
	uint64_t offs;
};

static int test_reserve(struct lsm *lsm, uint64_t size, uint64_t *offs)
{
	struct ctree_test_alloc *alloc = (struct ctree_test_alloc *)lsm->alloc;

	*offs = alloc->offs;
	alloc->offs += size;
	return 0;
}

static int test_persist(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	(void) lsm;
	(void) offs;
	(void) size;
	return 0;
}

static void test_cancel(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	(void) lsm;
	(void) offs;
	(void) size;
}

static int test_free(struct lsm *lsm, uint64_t offs, uint64_t size)
{
	(void) lsm;
	(void) offs;
	(void) size;
	return 0;
}

static struct ctree_test_alloc test_alloc = {
	.alloc = {
		.reserve = &test_reserve,
		.persist = &test_persist,
		.cancel = &test_cancel,
		.free = &test_free
	},
	.offs = 0
};

struct file_io {
	struct io io;
	int fd;
};

static int test_read(struct io *io, void *buf, size_t size, off_t offs)
{
	struct file_io *file = (struct file_io *)io;

	return file_read_at(file->fd, buf, size, offs);
}

static int test_write(struct io *io, const void *buf, size_t size, off_t offs)
{
	struct file_io *file = (struct file_io *)io;

	return file_write_at(file->fd, buf, size, offs);
}

static int test_sync(struct io *io)
{
	(void) io;
	return 0;
}

static struct io_ops test_ops = {
	.read = &test_read,
	.write = &test_write,
	.sync = &test_sync
};

struct test_key {
	size_t value;
};

static int test_cmp(const struct lsm_key *l, const struct lsm_key *r)
{
	const struct test_key *left = l->ptr;
	const struct test_key *right = r->ptr;

	if (left->value != right->value)
		return left->value < right->value ? -1 : 1;
	return 0;
}

static int create_ctree(struct ctree *ctree)
{
	static const size_t KEYS = 1000000;
	struct ctree_builder builder;
	int rc;

	ctree_builder_setup(&builder, ctree->lsm);
	for (size_t i = 0; i != KEYS; ++i) {
		struct test_key data = { .value = i };
		struct lsm_key key = { .ptr = &data, .size = sizeof(data) };
		struct lsm_val val = { .ptr = NULL, .size = 0 };

		rc = ctree_builder_append(&builder, 0, &key, &val);
		if (rc < 0) {
			ctree_builder_release(&builder);
			return -1;
		}
	}

	rc = ctree_builder_finish(&builder);
	if (rc < 0) {
		ctree_builder_release(&builder);
		return -1;
	}
	ctree->offs = builder.offs;
	ctree->size = builder.size;
	ctree->csum = builder.csum;
	ctree->height = builder.height;
	ctree_builder_release(&builder);
	return 0;
}

int main()
{
	const int fd = open("tree", O_RDWR | O_CREAT | O_TRUNC,
				S_IRUSR | S_IWUSR);

	if (fd < 0) {
		perror("open failed");
		return -1;
	}

	struct file_io test_io = {
		.io = {
			.ops = &test_ops,
			.page_size = 4096
		},
		.fd = fd
	};

	struct lsm test_lsm = {
		.io = &test_io.io,
		.alloc = &test_alloc.alloc,
		.cmp = &test_cmp
	};

	struct ctree ctree = { .lsm = &test_lsm };
	int ret = -1;

	if (create_ctree(&ctree))
		goto out;
	ret = 0;

out:
	close(fd);

	return ret;
}
