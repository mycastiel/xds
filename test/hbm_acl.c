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
/* aclrtMemcpy(dst, destMax, src, count, kind) — kind 2 = DEVICE_TO_HOST */
typedef int (*aclrt_memcpy_fn)(void *, size_t, const void *, size_t, int);

enum {
	XDS_ACL_MEMCPY_DEVICE_TO_HOST = 2,
};

struct acl_api {
	void *handle;
	acl_init_fn init;
	acl_finalize_fn finalize;
	aclrt_set_device_fn set_device;
	aclrt_reset_device_fn reset_device;
	aclrt_malloc_fn malloc;
	aclrt_free_fn free;
	aclrt_memcpy_fn memcpy;
	int refcnt;
};

static struct acl_api g_acl;

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
	g_acl.memcpy = (aclrt_memcpy_fn)dlsym(g_acl.handle, "aclrtMemcpy");
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
	fprintf(stderr,
		"hbm_acl: device=%d HBM %zu bytes at %p (policy=%d)\n",
		device_id, size, addr, malloc_policy);
	return 0;
}

void xds_hbm_free(struct xds_hbm_buf *buf)
{
	if (!buf || !buf->active)
		return;
	if (g_acl.free && buf->addr)
		g_acl.free(buf->addr);
	if (g_acl.reset_device)
		g_acl.reset_device(buf->device_id);
	if (g_acl.finalize)
		g_acl.finalize();
	acl_unload();
	memset(buf, 0, sizeof(*buf));
}

int xds_hbm_memcpy_d2h(void *host, const void *device, size_t n)
{
	int ret;

	if (!host || !device || !n)
		return -EINVAL;
	if (!g_acl.handle || !g_acl.memcpy)
		return -ENODEV;

	ret = g_acl.memcpy(host, n, device, n, XDS_ACL_MEMCPY_DEVICE_TO_HOST);
	if (ret != 0) {
		fprintf(stderr,
			"hbm_acl: aclrtMemcpy D2H (n=%zu) failed: %d\n", n, ret);
		return ret > 0 ? -EIO : ret;
	}
	return 0;
}
