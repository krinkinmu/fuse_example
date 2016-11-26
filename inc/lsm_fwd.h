#ifndef __LSM_FWD_H__
#define __LSM_FWD_H__


#include <stddef.h>

struct lsm_key {
	void *ptr;
	size_t size;
};

struct lsm_val {
	void *ptr;
	size_t size;
};

#endif /*__LSM_FWD_H__*/
