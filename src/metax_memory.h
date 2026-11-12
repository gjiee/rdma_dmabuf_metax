#ifndef PERFTEST_METAX_MEMORY_H
#define PERFTEST_METAX_MEMORY_H

#include <stddef.h>
#include <infiniband/verbs.h>

int metax_memory_alloc(void **buffer, size_t bytes, int device_id);
int metax_memory_free(void *buffer, int device_id);
int metax_memory_init_buffer(void *buffer, size_t bytes, int device_id, int zero);
int metax_memory_read_byte(const void *device_address, unsigned char *value,
			   int device_id);
int metax_memory_write_byte(void *device_address, unsigned char value,
			    int device_id);
struct ibv_mr *metax_memory_register(struct ibv_pd *pd, void *buffer,
				     size_t bytes, int access);
const char *metax_memory_error(void);

#endif
