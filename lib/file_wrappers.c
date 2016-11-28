#include <file_wrappers.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

ssize_t file_size(int fd)
{
	struct stat stat;

	if (fstat(fd, &stat) < 0)
		return (ssize_t)-errno;
	return (ssize_t)stat.st_size;
}

int file_write_at(int fd, const void *data, int size, off_t off)
{
	const char *buf = data;

	while (size) {
		const int rc = pwrite(fd, buf, size, off);

		if (rc < 0)
			return -errno;
		buf += rc;
		off += rc;
		size -= rc;
	}

	return 0;
}

int file_read_at(int fd, void *data, int size, off_t off)
{
	char *buf = data;
	int r = 0;

	while (r != size) {
		const int rc = pread(fd, buf, size, off);

		if (rc < 0)
			return -errno;

		if (!rc)
			break;
		buf += rc;
		off += rc;
		size -= rc;
		r += rc;
	}

	return r;
}
