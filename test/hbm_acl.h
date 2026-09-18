/*
 * Optional Ascend ACL helpers for physical-machine HBM buffers.
 * Resolved at runtime via dlopen("libascendcl.so") so the benches still
 * build on VMs without the Ascend toolkit.
 */
#ifndef XDS_HBM_ACL_H
#define XDS_HBM_ACL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Match aclrtMemMallocPolicy in CANN acl_rt.h */
enum xds_acl_malloc_policy {
	XDS_ACL_MEM_MALLOC_HUGE_FIRST = 0,
	XDS_ACL_MEM_MALLOC_HUGE_ONLY = 1,
	XDS_ACL_MEM_MALLOC_NORMAL_ONLY = 2,
	XDS_ACL_MEM_MALLOC_HUGE_FIRST_P2P = 3,
	XDS_ACL_MEM_MALLOC_HUGE_ONLY_P2P = 4,
};

/* Match aclrtMemcpyKind in CANN acl_rt.h */
enum xds_acl_memcpy_kind {
	XDS_ACL_MEMCPY_HOST_TO_HOST = 0,
	XDS_ACL_MEMCPY_HOST_TO_DEVICE = 1,
	XDS_ACL_MEMCPY_DEVICE_TO_HOST = 2,
	XDS_ACL_MEMCPY_DEVICE_TO_DEVICE = 3,
};

struct xds_hbm_buf {
	void *addr;
	size_t size;
	int device_id;
	bool active;
};

/* Opaque ACL stream handle (aclrtStream). */
typedef void *xds_acl_stream_t;

/* Returns 0 on success, negative errno-style or positive ACL error. */
int xds_hbm_alloc(struct xds_hbm_buf *buf, int device_id, size_t size,
		  int malloc_policy);
void xds_hbm_free(struct xds_hbm_buf *buf);
int xds_hbm_set_device(int device_id);
/* Enable P2P from @local to @peer (logical ids). Call both directions. */
int xds_hbm_enable_peer(int local, int peer);

/*
 * Copy @n bytes from device HBM (@device) into host (@host).
 * Requires an active xds_hbm_alloc on the same device (set_device still in effect).
 */
int xds_hbm_memcpy_d2h(void *host, const void *device, size_t n);

/* Generic sync memcpy (kind = xds_acl_memcpy_kind). */
int xds_hbm_memcpy(void *dst, const void *src, size_t n, int kind);
int xds_hbm_memcpy_h2d(void *device, const void *host, size_t n);
int xds_hbm_memcpy_d2d(void *dst_dev, const void *src_dev, size_t n);

/* Pinned host memory (aclrtMallocHost / aclrtFreeHost). */
int xds_hbm_host_alloc(void **addr, size_t size);
void xds_hbm_host_free(void *addr);

/* Streams + async memcpy for queue-depth style benches. */
int xds_hbm_stream_create(xds_acl_stream_t *stream);
int xds_hbm_stream_destroy(xds_acl_stream_t stream);
int xds_hbm_stream_synchronize(xds_acl_stream_t stream);
int xds_hbm_device_synchronize(void);
int xds_hbm_memcpy_async(void *dst, const void *src, size_t n, int kind,
			 xds_acl_stream_t stream);

#endif /* XDS_HBM_ACL_H */
