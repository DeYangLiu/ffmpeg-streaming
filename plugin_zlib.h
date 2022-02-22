#ifndef _PLUGIN_ZLIB_H_
#define _PLUGIN_ZLIB_H_
//#include <stdint.h>
typedef int (*read_fn)(int fd, void *buf, unsigned int count);

void* zlib_init(void);
int zlib_destroy(void *z);
int zlib_read_compress(read_fn fn, int fd, void *z, uint8_t *buf, int size);
#endif

