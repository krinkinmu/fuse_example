#include <file_wrappers.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

ssize_t file_size(int fd)
{
	struct stat stat;

	if (fstat(fd, &stat) < 0)
		return -1;
	return (ssize_t)stat.st_size;
}

int file_write(int fd, const void *data, int size)
{
	const char *buf = data;

	for (int w = 0; w != size;) {
		const int rc = write(fd, buf + w, size - w);

		if (rc < 0)
			return -1;
		w += rc;
	}

	return 0;
}

int file_read(int fd, void *data, int size)
{
	char *buf = data;
	int r = 0;

	while (r != size) {
		const int rc = read(fd, buf + r, size - r);

		if (rc < 0)
			return -1;

		if (!rc)
			break;

		r += rc;
	}

	return r;
}
