#include <crc64.h>

#include "crc64_table.h"


/*
uint64_t __crc64(uint64_t init, const void *data, size_t size)
{
	const unsigned char *ptr = data;
	uint64_t crc = init;

	while (size--) {
		const int i = ((unsigned char)crc ^ *ptr++) & 0xff;

		crc = crc_table[0][i] ^ (crc >> 8);
	}

	return crc;
}
*/

/* TODO: we really need to verify this algorithm implementation,
 * and also port it to big endian (which should be relatively easy). */
uint64_t __crc64(uint64_t init, const void *data, size_t size)
{
	const uint64_t *word_ptr = data;
	uint64_t crc = init;

	while (size >= 8) {
		const uint64_t word = *word_ptr++ ^ crc;

		crc = crc_table[0][(word >> 56) & 0xff] ^
			crc_table[1][(word >> 48) & 0xff] ^
			crc_table[2][(word >> 40) & 0xff] ^
			crc_table[3][(word >> 32) & 0xff] ^
			crc_table[4][(word >> 24) & 0xff] ^
			crc_table[5][(word >> 16) & 0xff] ^
			crc_table[6][(word >> 8) & 0xff] ^
			crc_table[7][word & 0xff];

		size -= 8;
	}

	const unsigned char *byte_ptr = (const unsigned char *)word_ptr;

	while (size--) {
		const unsigned char i = ((crc & 0xff) ^ *byte_ptr++);

		crc = crc_table[0][i] ^ (crc >> 8);
	}

	return crc;
}
