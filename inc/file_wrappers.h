#ifndef __FILE_WRAPPERS_H__
#define __FILE_WRAPPERS_H__

#include <sys/types.h>

ssize_t file_size(int fd);
int file_write_at(int fd, const void *data, int size, off_t off);
int file_read_at(int fd, void *data, int size, off_t off);

#endif /*__FILE_WRAPPERS_H__*/
