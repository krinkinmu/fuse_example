#include <lsm.h>
#include <file_wrappers.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>


struct ctree_test_alloc {
	struct alloc alloc;
	uint64_t offs;
};

static int test_reserve(struct alloc *a, uint64_t size, uint64_t *offs)
{
	struct ctree_test_alloc *alloc = (struct ctree_test_alloc *)a;

	*offs = alloc->offs;
	alloc->offs += size;
	return 0;
}

static int test_persist(struct alloc *a, uint64_t offs, uint64_t size)
{
	(void) a;
	(void) offs;
	(void) size;
	return 0;
}

static int test_cancel(struct alloc *a, uint64_t offs, uint64_t size)
{
	(void) a;
	(void) offs;
	(void) size;
	return 0;
}

static int test_free(struct alloc *a, uint64_t offs, uint64_t size)
{
	(void) a;
	(void) offs;
	(void) size;
	return 0;
}

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

static const size_t KEYS = 10000000;

static int test_key_deleted(struct lsm_merge_policy *policy,
			const struct lsm_key *key, const struct lsm_val *val)
{
	(void) policy;
	(void) key;
	(void) val;
	return 0;
}

static int create_lsm(struct lsm *lsm)
{
	struct lsm_merge_policy policy;
	int rc;

	policy.deleted = &test_key_deleted;
	for (size_t i = 0; i != KEYS; ++i) {
		struct test_key data = { .value = 2 * i };
		struct lsm_key key = { .ptr = &data, .size = sizeof(data) };
		struct lsm_val val = { .ptr = NULL, .size = 0 };

		rc = lsm_add(lsm, &key, &val);
		if (rc < 0) {
			puts("lsm_add failed");
			return -1;
		}

		if ((i + 1) % 100000 == 0) {
			rc = lsm_merge(lsm, 0, &policy);
			if (rc < 0) {
				puts("lsm_merge failed");
				return -1;
			}
		}

		if ((i + 1) % 1000000 == 0) {
			rc = lsm_merge(lsm, 2, &policy);
			if (rc < 0) {
				puts("lsm_merge failed");
				return -1;
			}
		}

		if ((i + 1) % 5000000 == 0) {
			rc = lsm_merge(lsm, 3, &policy);
			if (rc < 0) {
				puts("lsm_merge failed");
				return -1;
			}
		}
	}
	return 0;
}

static int iterate_lsm_forward(struct lsm *lsm)
{
	struct lsm_iter iter;
	int ret = -1;

	lsm_iter_setup(&iter, lsm);
	if (lsm_begin(&iter) < 0) {
		puts("lsm_begin failed");
		goto out;
	}

	for (size_t i = 0; i != KEYS; ++i) {
		if (!lsm_has_item(&iter)) {
			puts("wrong number of keys");
			goto out;
		}

		struct test_key *key;

		if (iter.key.size != sizeof(*key)) {
			puts("wrong key size");
			goto out;
		}

		key = (struct test_key *)iter.key.ptr;
		if (key->value != i * 2) {
			puts("wrong key value");
			goto out;
		}

		const int rc = lsm_next(&iter);

		if (rc < 0 && rc != -ENOENT) {
			puts("lsm_next failed");
			goto out;
		}
	}

	for (size_t i = KEYS; i != 0; --i) {
		const int rc = lsm_prev(&iter);

		if (rc < 0 && rc != -ENOENT) {
			puts("lsm_prev failed");
			goto out;
		}

		if (rc == -ENOENT || !lsm_has_item(&iter)) {
			puts("wrong number of keys");
			goto out;
		}

		struct test_key *key;

		if (iter.key.size != sizeof(*key)) {
			puts("wrong key size");
			goto out;
		}

		key = (struct test_key *)iter.key.ptr;
		if (key->value != (i - 1) * 2) {
			puts("wrong key value");
			goto out;
		}
	}

	ret = 0;
out:
	lsm_iter_release(&iter);
	return ret;
}

static int iterate_lsm_backward(struct lsm *lsm)
{
	struct lsm_iter iter;
	int ret = -1;

	lsm_iter_setup(&iter, lsm);
	if (lsm_end(&iter) < 0) {
		puts("lsm_begin failed");
		goto out;
	}

	for (size_t i = KEYS; i != 0; --i) {
		const int rc = lsm_prev(&iter);

		if (rc < 0 && rc != -ENOENT) {
			puts("lsm_prev failed");
			goto out;
		}

		if (rc == -ENOENT || !lsm_has_item(&iter)) {
			puts("wrong number of keys");
			goto out;
		}

		struct test_key *key;

		if (iter.key.size != sizeof(*key)) {
			puts("wrong key size");
			goto out;
		}

		key = (struct test_key *)iter.key.ptr;
		if (key->value != (i - 1) * 2) {
			puts("wrong key value");
			goto out;
		}
	}

	for (size_t i = 0; i != KEYS; ++i) {
		if (!lsm_has_item(&iter)) {
			puts("wrong number of keys");
			goto out;
		}

		struct test_key *key;

		if (iter.key.size != sizeof(*key)) {
			puts("wrong key size");
			goto out;
		}

		key = (struct test_key *)iter.key.ptr;
		if (key->value != i * 2) {
			puts("wrong key value");
			goto out;
		}

		const int rc = lsm_next(&iter);

		if (rc < 0 && rc != -ENOENT) {
			puts("lsm_next failed");
			goto out;
		}
	}

	ret = 0;
out:
	lsm_iter_release(&iter);
	return ret;
}

static struct io_ops test_io_ops = {
	.read = &test_read,
	.write = &test_write,
	.sync = &test_sync
};

static struct alloc_ops test_alloc_ops = {
	.reserve = &test_reserve,
	.commit = &test_persist,
	.cancel = &test_cancel,
	.free = &test_free
};

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
			.ops = &test_io_ops,
			.page_size = 4096
		},
		.fd = fd
	};

	struct ctree_test_alloc test_alloc = {
		.alloc = {
			.ops = &test_alloc_ops
		},
		.offs = 0
	};

	struct lsm lsm;
	int ret = -1;

	lsm_setup(&lsm, &test_io.io, &test_alloc.alloc, &test_cmp);

	if (create_lsm(&lsm)) {
		puts("create_lsm failed");
		goto out;
	}
	if (iterate_lsm_forward(&lsm)) {
		puts("iterate_iter_forward failed");
		goto out;
	}
	if (iterate_lsm_backward(&lsm)) {
		puts("iterate_iter_forward failed");
		goto out;
	}
	ret = 0;

out:
	lsm_release(&lsm);
	close(fd);

	return ret;
}
