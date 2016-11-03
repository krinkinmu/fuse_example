#ifndef __CRC64_H__
#define __CRC64_H__

#include <stdint.h>
#include <stddef.h>

#define CRC64_INIT	0

struct crc64_ctx {
	uint64_t crc;
};

uint64_t __crc64(uint64_t init, const void *data, size_t size);

static inline uint64_t crc64(const void *data, size_t size)
{ return __crc64(CRC64_INIT, data, size); }


static inline void crc64_ctx_setup(struct crc64_ctx *ctx)
{ ctx->crc = CRC64_INIT; }

static inline void crc64_ctx_release(struct crc64_ctx *ctx)
{ (void) ctx; }

static inline void crc64_ctx_update(struct crc64_ctx *ctx,
			const void *data, size_t size)
{ ctx->crc = __crc64(ctx->crc, data, size); }

static inline uint64_t crc64_ctx_csum(const struct crc64_ctx *ctx)
{ return ctx->crc; }

#endif /*__CRC64_H__*/
