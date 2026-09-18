#define _GNU_SOURCE
#include "hbm_acl.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*acl_init_fn)(const char *);
typedef int (*acl_finalize_fn)(void);
typedef int (*aclrt_set_device_fn)(int);
typedef int (*aclrt_reset_device_fn)(int);
typedef int (*aclrt_malloc_fn)(void **, size_t, int);
typedef int (*aclrt_free_fn)(void *);
typedef int (*aclrt_malloc_host_fn)(void **, size_t);
typedef int (*aclrt_free_host_fn)(void *);
/* aclrtMemcpy(dst, destMax, src, count, kind) */
typedef int (*aclrt_memcpy_fn)(void *, size_t, const void *, size_t, int);
typedef int (*aclrt_memcpy_async_fn)(void *, size_t, const void *, size_t, int,
				     void *);
typedef int (*aclrt_create_stream_fn)(void **);
typedef int (*aclrt_destroy_stream_fn)(void *);
typedef int (*aclrt_synchronize_stream_fn)(void *);
typedef int (*aclrt_synchronize_device_fn)(void);
typedef int (*aclrt_can_access_peer_fn)(int *, int, int);
typedef int (*aclrt_enable_peer_fn)(int, unsigned int);

struct acl_api {
	void *handle;
	acl_init_fn init;
	acl_finalize_fn finalize;
	aclrt_set_device_fn set_device;
	aclrt_reset_device_fn reset_device;
	aclrt_malloc_fn malloc;
	aclrt_free_fn free;
	aclrt_malloc_host_fn malloc_host;
	aclrt_free_host_fn free_host;
	aclrt_memcpy_fn memcpy;
	aclrt_memcpy_async_fn memcpy_async;
	aclrt_create_stream_fn create_stream;
	aclrt_destroy_stream_fn destroy_stream;
	aclrt_synchronize_stream_fn sync_stream;
	aclrt_synchronize_device_fn sync_device;
	aclrt_can_access_peer_fn can_access_peer;
	aclrt_enable_peer_fn enable_peer;
	int refcnt;
};

static struct acl_api g_acl;
static int opened_devs[64];
static int n_opened_devs;

static void note_device(int device_id)
{
	int i;

	for (i = 0; i < n_opened_devs; i++) {
		if (opened_devs[i] == device_id)
			return;
	}
	if (n_opened_devs < (int)(sizeof(opened_devs) / sizeof(opened_devs[0])))
		opened_devs[n_opened_devs++] = device_id;
}

static int acl_load(void)
{
	const char *path;
	char errbuf[256];

	if (g_acl.handle) {
		g_acl.refcnt++;
		return 0;
	}

	path = getenv("ASCEND_ACL_LIB");
	if (!path || !*path)
		path = "libascendcl.so";
	g_acl.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!g_acl.handle) {
		snprintf(errbuf, sizeof(errbuf), "%s", dlerror());
		fprintf(stderr,
			"hbm_acl: dlopen(%s) failed: %s\n"
			"  Set ASCEND_ACL_LIB or source Ascend set_env.sh\n",
			path, errbuf);
		return -ENOENT;
	}

	g_acl.init = (acl_init_fn)dlsym(g_acl.handle, "aclInit");
	g_acl.finalize = (acl_finalize_fn)dlsym(g_acl.handle, "aclFinalize");
	g_acl.set_device = (aclrt_set_device_fn)dlsym(g_acl.handle, "aclrtSetDevice");
	g_acl.reset_device =
		(aclrt_reset_device_fn)dlsym(g_acl.handle, "aclrtResetDevice");
	g_acl.malloc = (aclrt_malloc_fn)dlsym(g_acl.handle, "aclrtMalloc");
	g_acl.free = (aclrt_free_fn)dlsym(g_acl.handle, "aclrtFree");
	g_acl.malloc_host =
		(aclrt_malloc_host_fn)dlsym(g_acl.handle, "aclrtMallocHost");
	g_acl.free_host =
		(aclrt_free_host_fn)dlsym(g_acl.handle, "aclrtFreeHost");
	g_acl.memcpy = (aclrt_memcpy_fn)dlsym(g_acl.handle, "aclrtMemcpy");
	g_acl.memcpy_async =
		(aclrt_memcpy_async_fn)dlsym(g_acl.handle, "aclrtMemcpyAsync");
	g_acl.create_stream =
		(aclrt_create_stream_fn)dlsym(g_acl.handle, "aclrtCreateStream");
	g_acl.destroy_stream =
		(aclrt_destroy_stream_fn)dlsym(g_acl.handle, "aclrtDestroyStream");
	g_acl.sync_stream = (aclrt_synchronize_stream_fn)dlsym(
		g_acl.handle, "aclrtSynchronizeStream");
	g_acl.sync_device = (aclrt_synchronize_device_fn)dlsym(
		g_acl.handle, "aclrtSynchronizeDevice");
	g_acl.can_access_peer = (aclrt_can_access_peer_fn)dlsym(
		g_acl.handle, "aclrtDeviceCanAccessPeer");
	g_acl.enable_peer = (aclrt_enable_peer_fn)dlsym(
		g_acl.handle, "aclrtDeviceEnablePeerAccess");
	if (!g_acl.init || !g_acl.finalize || !g_acl.set_device ||
	    !g_acl.reset_device || !g_acl.malloc || !g_acl.free ||
	    !g_acl.memcpy) {
		fprintf(stderr, "hbm_acl: missing Ascend symbols in %s\n", path);
		dlclose(g_acl.handle);
		memset(&g_acl, 0, sizeof(g_acl));
		return -ENOSYS;
	}
	g_acl.refcnt = 1;
	return 0;
}

static void acl_unload(void)
{
	if (!g_acl.handle)
		return;
	if (--g_acl.refcnt > 0)
		return;
	dlclose(g_acl.handle);
	memset(&g_acl, 0, sizeof(g_acl));
}

static int acl_require_async(void)
{
	if (!g_acl.handle)
		return -ENODEV;
	if (!g_acl.memcpy_async || !g_acl.create_stream ||
	    !g_acl.destroy_stream || !g_acl.sync_stream) {
		fprintf(stderr,
			"hbm_acl: async memcpy/stream symbols missing\n");
		return -ENOSYS;
	}
	return 0;
}

int xds_hbm_alloc(struct xds_hbm_buf *buf, int device_id, size_t size,
		  int malloc_policy)
{
	void *addr = NULL;
	int ret;

	if (!buf || !size || device_id < 0)
		return -EINVAL;
	memset(buf, 0, sizeof(*buf));

	ret = acl_load();
	if (ret)
		return ret;

	ret = g_acl.init(NULL);
	if (ret != 0) {
		/* ACL_ERROR_REPEAT_INITIALIZE is commonly 100002; ignore. */
		if (ret != 100002) {
			fprintf(stderr, "hbm_acl: aclInit failed: %d\n", ret);
			acl_unload();
			return ret > 0 ? -EIO : ret;
		}
	}

	ret = g_acl.set_device(device_id);
	if (ret != 0) {
		fprintf(stderr, "hbm_acl: aclrtSetDevice(%d) failed: %d\n",
			device_id, ret);
		g_acl.finalize();
		acl_unload();
		return ret > 0 ? -ENODEV : ret;
	}

	ret = g_acl.malloc(&addr, size, malloc_policy);
	if (ret != 0 || !addr) {
		fprintf(stderr,
			"hbm_acl: aclrtMalloc(size=%zu policy=%d) failed: %d\n",
			size, malloc_policy, ret);
		g_acl.reset_device(device_id);
		g_acl.finalize();
		acl_unload();
		return ret > 0 ? -ENOMEM : ret;
	}

	buf->addr = addr;
	buf->size = size;
	buf->device_id = device_id;
	buf->active = true;
	note_device(device_id);
	fprintf(stderr,
		"hbm_acl: device=%d HBM %zu bytes at %p (policy=%d)\n",
		device_id, size, addr, malloc_policy);
	return 0;
}

void xds_hbm_free(struct xds_hbm_buf *buf)
{
	int i;

	if (!buf || !buf->active)
		return;
	if (g_acl.free && buf->addr)
		g_acl.free(buf->addr);
	/*
	 * Multiple cards hold separate allocs. Only reset/finalize when the
	 * last buffer is released so peer D2D still has a live ACL context.
	 */
	if (g_acl.refcnt <= 1) {
		for (i = 0; i < n_opened_devs; i++) {
			if (g_acl.reset_device)
				g_acl.reset_device(opened_devs[i]);
		}
		n_opened_devs = 0;
		if (g_acl.finalize)
			g_acl.finalize();
	}
	acl_unload();
	memset(buf, 0, sizeof(*buf));
}

int xds_hbm_set_device(int device_id)
{
	int ret;

	if (device_id < 0)
		return -EINVAL;
	if (!g_acl.handle || !g_acl.set_device)
		return -ENODEV;
	ret = g_acl.set_device(device_id);
	if (ret != 0) {
		fprintf(stderr, "hbm_acl: aclrtSetDevice(%d) failed: %d\n",
			device_id, ret);
		return ret > 0 ? -ENODEV : ret;
	}
	note_device(device_id);
	return 0;
}

int xds_hbm_enable_peer(int local, int peer)
{
	int can = 1;
	int ret;

	if (local < 0 || peer < 0 || local == peer)
		return -EINVAL;
	if (!g_acl.handle)
		return -ENODEV;

	if (g_acl.can_access_peer) {
		ret = g_acl.can_access_peer(&can, local, peer);
		if (ret != 0) {
			fprintf(stderr,
				"hbm_acl: aclrtDeviceCanAccessPeer(%d,%d) failed: %d\n",
				local, peer, ret);
			return ret > 0 ? -EIO : ret;
		}
		fprintf(stderr, "hbm_acl: canAccessPeer %d -> %d = %d\n",
			local, peer, can);
		if (!can)
			return -EHOSTUNREACH;
	} else {
		fprintf(stderr,
			"hbm_acl: aclrtDeviceCanAccessPeer missing; skip check\n");
	}

	ret = xds_hbm_set_device(local);
	if (ret)
		return ret;
	if (!g_acl.enable_peer) {
		fprintf(stderr,
			"hbm_acl: aclrtDeviceEnablePeerAccess missing; try memcpy anyway\n");
		return 0;
	}
	ret = g_acl.enable_peer(peer, 0);
	if (ret != 0) {
		fprintf(stderr,
			"hbm_acl: aclrtDeviceEnablePeerAccess(%d->%d) failed: %d\n",
			local, peer, ret);
		return ret > 0 ? -EIO : ret;
	}
	return 0;
}

int xds_hbm_memcpy(void *dst, const void *src, size_t n, int kind)
{
	int ret;

	if (!dst || !src || !n)
		return -EINVAL;
	if (!g_acl.handle || !g_acl.memcpy)
		return -ENODEV;

	ret = g_acl.memcpy(dst, n, src, n, kind);
	if (ret != 0) {
		fprintf(stderr,
			"hbm_acl: aclrtMemcpy kind=%d (n=%zu) failed: %d\n",
			kind, n, ret);
		return ret > 0 ? -EIO : ret;
	}
	return 0;
}

int xds_hbm_memcpy_d2h(void *host, const void *device, size_t n)
{
	return xds_hbm_memcpy(host, device, n, XDS_ACL_MEMCPY_DEVICE_TO_HOST);
}

int xds_hbm_memcpy_h2d(void *device, const void *host, size_t n)
{
	return xds_hbm_memcpy(device, host, n, XDS_ACL_MEMCPY_HOST_TO_DEVICE);
}

int xds_hbm_memcpy_d2d(void *dst_dev, const void *src_dev, size_t n)
{
	return xds_hbm_memcpy(dst_dev, src_dev, n,
			      XDS_ACL_MEMCPY_DEVICE_TO_DEVICE);
}

int xds_hbm_host_alloc(void **addr, size_t size)
{
	int ret;

	if (!addr || !size)
		return -EINVAL;
	*addr = NULL;
	if (!g_acl.handle)
		return -ENODEV;
	if (!g_acl.malloc_host || !g_acl.free_host) {
		fprintf(stderr, "hbm_acl: aclrtMallocHost missing\n");
		return -ENOSYS;
	}
	ret = g_acl.malloc_host(addr, size);
	if (ret != 0 || !*addr) {
		fprintf(stderr, "hbm_acl: aclrtMallocHost(%zu) failed: %d\n",
			size, ret);
		*addr = NULL;
		return ret > 0 ? -ENOMEM : ret;
	}
	return 0;
}

void xds_hbm_host_free(void *addr)
{
	if (!addr || !g_acl.free_host)
		return;
	g_acl.free_host(addr);
}

int xds_hbm_stream_create(xds_acl_stream_t *stream)
{
	int ret;

	if (!stream)
		return -EINVAL;
	*stream = NULL;
	ret = acl_require_async();
	if (ret)
		return ret;
	ret = g_acl.create_stream(stream);
	if (ret != 0 || !*stream) {
		fprintf(stderr, "hbm_acl: aclrtCreateStream failed: %d\n", ret);
		*stream = NULL;
		return ret > 0 ? -EIO : ret;
	}
	return 0;
}

int xds_hbm_stream_destroy(xds_acl_stream_t stream)
{
	int ret;

	if (!stream)
		return 0;
	ret = acl_require_async();
	if (ret)
		return ret;
	ret = g_acl.destroy_stream(stream);
	if (ret != 0) {
		fprintf(stderr, "hbm_acl: aclrtDestroyStream failed: %d\n", ret);
		return ret > 0 ? -EIO : ret;
	}
	return 0;
}

int xds_hbm_stream_synchronize(xds_acl_stream_t stream)
{
	int ret;

	if (!stream)
		return -EINVAL;
	ret = acl_require_async();
	if (ret)
		return ret;
	ret = g_acl.sync_stream(stream);
	if (ret != 0) {
		fprintf(stderr,
			"hbm_acl: aclrtSynchronizeStream failed: %d\n", ret);
		return ret > 0 ? -EIO : ret;
	}
	return 0;
}

int xds_hbm_device_synchronize(void)
{
	int ret;

	if (!g_acl.handle)
		return -ENODEV;
	if (!g_acl.sync_device) {
		fprintf(stderr, "hbm_acl: aclrtSynchronizeDevice missing\n");
		return -ENOSYS;
	}
	ret = g_acl.sync_device();
	if (ret != 0) {
		fprintf(stderr,
			"hbm_acl: aclrtSynchronizeDevice failed: %d\n", ret);
		return ret > 0 ? -EIO : ret;
	}
	return 0;
}

int xds_hbm_memcpy_async(void *dst, const void *src, size_t n, int kind,
			 xds_acl_stream_t stream)
{
	int ret;

	if (!dst || !src || !n || !stream)
		return -EINVAL;
	ret = acl_require_async();
	if (ret)
		return ret;
	ret = g_acl.memcpy_async(dst, n, src, n, kind, stream);
	if (ret != 0) {
		fprintf(stderr,
			"hbm_acl: aclrtMemcpyAsync kind=%d (n=%zu) failed: %d\n",
			kind, n, ret);
		return ret > 0 ? -EIO : ret;
	}
	return 0;
}
