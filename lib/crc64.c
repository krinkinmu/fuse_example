#include <crc64.h>

#include "crc64_table.h"

uint64_t __crc64(uint64_t init, const void *data, size_t size)
{
	const unsigned char *ptr = data;
	uint64_t crc = init;

	while (size--) {
		const int i = ((unsigned char)crc ^ *ptr++) & 0xff;

		crc = crc_table[i] ^ (crc >> 8);
	}

	return crc;
}
