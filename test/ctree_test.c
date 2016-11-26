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

static const size_t KEYS = 1000000;

static int create_ctree(struct ctree *ctree)
{
	struct ctree_builder builder;
	int rc;

	ctree_builder_setup(&builder, ctree->lsm);
	for (size_t i = 0; i != KEYS; ++i) {
		struct test_key data = { .value = 2 * i };
		struct lsm_key key = { .ptr = &data, .size = sizeof(data) };
		struct lsm_val val = { .ptr = NULL, .size = 0 };

		rc = ctree_builder_append(&builder, &key, &val);
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
	ctree->ptr = builder.ptr;
	ctree->height = builder.height;
	ctree_builder_release(&builder);
	return 0;
}

static int iterate_ctree_forward(struct ctree *ctree)
{
	struct ctree_iter iter;
	struct test_key zero = { .value = 0 };
	struct lsm_key key = { .ptr = &zero, .size = sizeof(zero) };
	size_t count = 0;
	size_t expected = 0;
	int ret = -1;

	ctree_iter_setup(&iter, ctree);
	if (ctree_begin(&iter) < 0) {
		puts("ctree_begin failed");
		goto out;
	}

	while (count != KEYS) {
		struct test_key data;
		int rc;

		rc = ctree_key(&iter, &key);
		if (rc == -ENOENT) {
			puts("ctree_key failed");
			goto out;
		}

		if (key.size != sizeof(data)) {
			puts("wrong key size");
			goto out;
		}

		memcpy(&data, key.ptr, key.size);
		if (data.value != expected) {
			puts("wrong key value");
			goto out;
		}

		expected += 2;
		++count;

		rc = ctree_next(&iter);
		if (rc == -ENOENT)
			break;

		if (rc < 0) {
			puts("ctree_next failed");
			goto out;
		}
	}

	if (ctree_next(&iter) != -ENOENT || count != KEYS) {
		puts("wrong number of keys");
		goto out;
	}

	do {
		struct test_key data;
		int rc;

		rc = ctree_prev(&iter);
		if (rc == -ENOENT)
			break;

		if (rc) {
			puts("ctree_prev failed");
			goto out;
		}
		expected -= 2;
		--count;

		rc = ctree_key(&iter, &key);
		if (rc < 0) {
			puts("ctree_key failed");
			goto out;
		}

		if (key.size != sizeof(data)) {
			puts("wrong key size");
			goto out;
		}

		memcpy(&data, key.ptr, key.size);
		if (data.value != expected) {
			puts("wrong key value");
			goto out;
		}
	} while (count);

	if (ctree_prev(&iter) != -ENOENT || count) {
		puts("wrong number of keys");
		goto out;
	}

	ret = 0;
out:
	ctree_iter_release(&iter);
	return ret;
}

static int iterate_ctree_backward(struct ctree *ctree)
{
	struct ctree_iter iter;
	struct test_key zero = { .value = 0 };
	struct lsm_key key = { .ptr = &zero, .size = sizeof(zero) };
	size_t count = KEYS;
	size_t expected = (KEYS - 1) * 2;
	int ret = -1;

	ctree_iter_setup(&iter, ctree);
	if (ctree_end(&iter) < 0) {
		puts("ctree_end failed");
		goto out;
	}

	while (count) {
		struct test_key data;
		int rc;

		rc = ctree_prev(&iter);
		if (rc == -ENOENT)
			break;

		if (rc < 0) {
			puts("ctree_prev failed");
			goto out;
		}

		rc = ctree_key(&iter, &key);
		if (rc < 0) {
			puts("ctree_key failed");
			goto out;
		}

		if (key.size != sizeof(data)) {
			puts("wrong key size");
			goto out;
		}

		memcpy(&data, key.ptr, key.size);
		if (data.value != expected) {
			puts("wrong key value");
			goto out;
		}

		expected -= 2;
		--count;
	}

	if (ctree_prev(&iter) != -ENOENT || count) {
		puts("wrong number of keys");
		goto out;
	}

	do {
		struct test_key data;
		int rc;

		expected += 2;
		++count;

		rc = ctree_key(&iter, &key);
		if (rc < 0) {
			puts("ctree_key failed");
			goto out;
		}

		if (key.size != sizeof(data)) {
			puts("wrong key size");
			goto out;
		}

		memcpy(&data, key.ptr, key.size);
		if (data.value != expected) {
			puts("wrong key value");
			goto out;
		}

		rc = ctree_next(&iter);
		if (rc == -ENOENT)
			break;

		if (rc) {
			puts("ctree_next failed");
			goto out;
		}
	} while (count != KEYS);

	if (ctree_next(&iter) != -ENOENT || count != KEYS) {
		puts("wrong number of keys");
		goto out;
	}

	ret = 0;
out:
	ctree_iter_release(&iter);
	return ret;
}

static int lookup_ctree(struct ctree *ctree)
{
	for (size_t i = 0; i != KEYS; ++i) {
		struct test_key data = { .value = 2 * i };
		struct lsm_key key = { .ptr = &data, .size = sizeof(data) };
		struct ctree_iter iter;
		int ret;

		ctree_iter_setup(&iter, ctree);
		ret = ctree_lookup(&iter, &key);
		if (ret < 0) {
			ctree_iter_release(&iter);
			puts("lookup failed");
			return -1;
		}
		if (!ret) {
			ctree_iter_release(&iter);
			puts("key not found");
			return -1;
		}
		if (ctree_key(&iter, &key) < 0) {
			ctree_iter_release(&iter);
			puts("ctree_next failed to get key");
			return -1;
		}
		if (key.size != sizeof(data)) {
			ctree_iter_release(&iter);
			puts("wrong key size");
			return -1;
		}
		memcpy(&data, key.ptr, key.size);
		if (data.value != 2 * i) {
			ctree_iter_release(&iter);
			puts("wrong key value");
			return -1;
		}
		ctree_iter_release(&iter);
	}

	for (size_t i = 0; i != KEYS - 1; ++i) {
		struct test_key data = { .value = 2 * i + 1 };
		struct lsm_key key = { .ptr = &data, .size = sizeof(data) };
		struct ctree_iter iter;
		int ret;

		ctree_iter_setup(&iter, ctree);
		ret = ctree_lower_bound(&iter, &key);
		if (ret < 0) {
			ctree_iter_release(&iter);
			puts("lookup failed");
			return -1;
		}
		if (ctree_key(&iter, &key) < 0) {
			ctree_iter_release(&iter);
			puts("ctree_next failed to get key");
			return -1;
		}
		if (key.size != sizeof(data)) {
			ctree_iter_release(&iter);
			puts("wrong key size");
			return -1;
		}
		memcpy(&data, key.ptr, key.size);
		if (data.value != 2 * i + 2) {
			ctree_iter_release(&iter);
			puts("wrong key value");
			return -1;
		}
		ctree_iter_release(&iter);
	}

	for (size_t i = 0; i != KEYS - 1; ++i) {
		struct test_key data = { .value = 2 * i };
		struct lsm_key key = { .ptr = &data, .size = sizeof(data) };
		struct ctree_iter iter;
		int ret;

		ctree_iter_setup(&iter, ctree);
		ret = ctree_upper_bound(&iter, &key);
		if (ret < 0) {
			ctree_iter_release(&iter);
			puts("lookup failed");
			return -1;
		}
		if (ctree_key(&iter, &key) < 0) {
			ctree_iter_release(&iter);
			puts("ctree_next failed to get key");
			return -1;
		}
		if (key.size != sizeof(data)) {
			ctree_iter_release(&iter);
			puts("wrong key size");
			return -1;
		}
		memcpy(&data, key.ptr, key.size);
		if (data.value != 2 * i + 2) {
			ctree_iter_release(&iter);
			puts("wrong key value");
			return -1;
		}
		ctree_iter_release(&iter);
	}
	return 0;
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

	struct lsm test_lsm;
	struct ctree *ctree = &test_lsm.ci[0];
	int ret = -1;

	lsm_setup(&test_lsm, &test_io.io, &test_alloc.alloc, &test_cmp);

	if (create_ctree(ctree)) {
		puts("create_ctree failed");
		goto out;
	}
	if (iterate_ctree_forward(ctree)) {
		puts("iterate_ctree_forward failed");
		goto out;
	}
	if (iterate_ctree_backward(ctree)) {
		puts("iterate_ctree_backward failed");
		goto out;
	}
	if (lookup_ctree(ctree)) {
		puts("lookup_ctree failed");
		goto out;
	}
	ret = 0;

out:
	lsm_release(&test_lsm);
	close(fd);

	return ret;
}
