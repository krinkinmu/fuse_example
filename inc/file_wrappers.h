#ifndef __FILE_WRAPPERS_H__
#define __FILE_WRAPPERS_H__

#include <sys/types.h>

ssize_t file_size(int fd);
int file_write(int fd, const void *data, int size);
int file_read(int fd, void *data, int size);

#endif /*__FILE_WRAPPERS_H__*/
