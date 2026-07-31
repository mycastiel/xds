#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nds_api.h"

/*
 * Opaque ctx wrapper: Python holds the pointer as a PyCapsule named "nds_io_ctx".
 */
static const char *nds_ctx_capsule_name = "nds_io_ctx";

static void nds_ctx_capsule_destructor(PyObject *capsule)
{
	struct nds_io_ctx *ctx = PyCapsule_GetPointer(capsule, nds_ctx_capsule_name);

	if (ctx)
		nds_io_destroy_ctx(ctx);
}

static struct nds_io_ctx *nds_ctx_from_obj(PyObject *obj)
{
	struct nds_io_ctx *ctx;

	if (!PyCapsule_CheckExact(obj)) {
		PyErr_SetString(PyExc_TypeError, "ctx must be an nds_io_ctx capsule");
		return NULL;
	}
	ctx = PyCapsule_GetPointer(obj, nds_ctx_capsule_name);
	if (!ctx)
		PyErr_SetString(PyExc_ValueError, "invalid or destroyed nds_io_ctx");
	return ctx;
}

static int parse_nds_obj(PyObject *obj, struct nds_io_obj *out)
{
	PyObject *type_obj;
	PyObject *value_obj;
	Py_ssize_t n;
	long type;

	memset(out, 0, sizeof(*out));
	if (!PyTuple_Check(obj)) {
		PyErr_SetString(PyExc_TypeError,
				"obj must be (type, fd_or_id)");
		return -1;
	}
	n = PyTuple_GET_SIZE(obj);
	if (n != 2) {
		PyErr_SetString(PyExc_TypeError,
				"obj must be (type, fd_or_id)");
		return -1;
	}
	type_obj = PyTuple_GET_ITEM(obj, 0);
	value_obj = PyTuple_GET_ITEM(obj, 1);
	type = PyLong_AsLong(type_obj);
	if (PyErr_Occurred())
		return -1;
	out->type = (uint32_t)type;

	if (out->type == NDS_IO_OBJ_FD) {
		out->fd = (int32_t)PyLong_AsLong(value_obj);
		if (PyErr_Occurred())
			return -1;
		return 0;
	}
	if (out->type == NDS_IO_OBJ_ID) {
		/* Pass through; C layer returns -EOPNOTSUPP. */
		out->_data = (uint64_t)PyLong_AsUnsignedLongLong(value_obj);
		if (PyErr_Occurred())
			return -1;
		return 0;
	}
	PyErr_SetString(PyExc_ValueError, "unsupported nds_io_obj type");
	return -1;
}

static PyObject *py_nds_init(PyObject *Py_UNUSED(self), PyObject *args)
{
	struct nds_init_param param = { .version = NDS_API_VERSION };
	unsigned int version = NDS_API_VERSION;
	unsigned int flags = 0;
	PyObject *fd_seq;
	PyObject *seq = NULL;
	int32_t *fds = NULL;
	Py_ssize_t count;
	Py_ssize_t i;
	int ret;

	/* init(fs_fds[, version=NDS_API_VERSION[, flags=0]]) */
	if (!PyArg_ParseTuple(args, "O|II", &fd_seq, &version, &flags))
		return NULL;

	seq = PySequence_Fast(fd_seq, "fs_fds must be a sequence of ints");
	if (!seq)
		return NULL;
	count = PySequence_Fast_GET_SIZE(seq);
	if (count < 0 || count > UINT32_MAX) {
		Py_DECREF(seq);
		PyErr_SetString(PyExc_ValueError, "fs_fds count out of range");
		return NULL;
	}
	fds = calloc((size_t)(count ? count : 1), sizeof(*fds));
	if (!fds) {
		Py_DECREF(seq);
		return PyErr_NoMemory();
	}
	for (i = 0; i < count; i++) {
		long v = PyLong_AsLong(PySequence_Fast_GET_ITEM(seq, i));

		if (PyErr_Occurred() || v < 0 || v > INT32_MAX) {
			if (!PyErr_Occurred())
				PyErr_SetString(PyExc_ValueError,
						"invalid fd in fs_fds");
			free(fds);
			Py_DECREF(seq);
			return NULL;
		}
		fds[i] = (int32_t)v;
	}
	Py_DECREF(seq);

	param.version = version;
	param.flags = flags;
	param.desc.fs_fd = fds;
	param.desc.fs_fd_cnt = (uint32_t)count;
	param.desc.reserved = 0;

	Py_BEGIN_ALLOW_THREADS
	ret = nds_init(&param);
	Py_END_ALLOW_THREADS
	free(fds);
	return PyLong_FromLong(ret);
}

static PyObject *py_nds_exit(PyObject *Py_UNUSED(self),
			     PyObject *Py_UNUSED(ignored))
{
	int ret;

	Py_BEGIN_ALLOW_THREADS
	ret = nds_exit();
	Py_END_ALLOW_THREADS
	return PyLong_FromLong(ret);
}

static PyObject *py_nds_register_mem(PyObject *Py_UNUSED(self), PyObject *args)
{
	unsigned long long addr;
	unsigned long long size;
	int flags = 0;
	int ret;

	if (!PyArg_ParseTuple(args, "KK|i", &addr, &size, &flags))
		return NULL;

	Py_BEGIN_ALLOW_THREADS
	ret = nds_register_mem((void *)(uintptr_t)addr, size, flags);
	Py_END_ALLOW_THREADS

	return PyLong_FromLong(ret);
}

static PyObject *py_nds_unregister_mem(PyObject *Py_UNUSED(self), PyObject *args)
{
	unsigned long long addr;
	int ret;

	if (!PyArg_ParseTuple(args, "K", &addr))
		return NULL;

	Py_BEGIN_ALLOW_THREADS
	ret = nds_unregister_mem((void *)(uintptr_t)addr);
	Py_END_ALLOW_THREADS
	return PyLong_FromLong(ret);
}

static PyObject *py_nds_io_new_ctx(PyObject *Py_UNUSED(self), PyObject *args)
{
	struct nds_io_ctx_param param = { };
	struct nds_io_ctx *ctx = NULL;
	unsigned int max_io_nr;
	unsigned int flags = 0;
	int ret;

	if (!PyArg_ParseTuple(args, "I|I", &max_io_nr, &flags))
		return NULL;
	param.max_io_nr = max_io_nr;
	param.flags = flags;

	Py_BEGIN_ALLOW_THREADS
	ret = nds_io_new_ctx(&param, &ctx);
	Py_END_ALLOW_THREADS
	if (ret)
		return PyLong_FromLong(ret);

	return PyCapsule_New(ctx, nds_ctx_capsule_name, nds_ctx_capsule_destructor);
}

static PyObject *py_nds_io_destroy_ctx(PyObject *Py_UNUSED(self), PyObject *args)
{
	PyObject *capsule;
	struct nds_io_ctx *ctx;
	int ret;

	if (!PyArg_ParseTuple(args, "O", &capsule))
		return NULL;
	ctx = nds_ctx_from_obj(capsule);
	if (!ctx)
		return NULL;

	/* Disable auto-destroy; this call owns the teardown. */
	if (PyCapsule_SetDestructor(capsule, NULL) < 0)
		return NULL;

	Py_BEGIN_ALLOW_THREADS
	ret = nds_io_destroy_ctx(ctx);
	Py_END_ALLOW_THREADS

	/* Invalidate capsule so later use fails cleanly. */
	if (PyCapsule_SetName(capsule, "nds_io_ctx_destroyed") < 0)
		return NULL;
	return PyLong_FromLong(ret);
}

static PyObject *py_nds_io_submit(PyObject *Py_UNUSED(self), PyObject *args)
{
	PyObject *capsule;
	PyObject *iocb_seq;
	PyObject *seq;
	struct nds_io_ctx *ctx;
	struct nds_io_cb *cbs = NULL;
	Py_ssize_t count;
	Py_ssize_t i;
	int ret;

	if (!PyArg_ParseTuple(args, "OO", &capsule, &iocb_seq))
		return NULL;
	ctx = nds_ctx_from_obj(capsule);
	if (!ctx)
		return NULL;

	seq = PySequence_Fast(iocb_seq, "iocbs must be a sequence");
	if (!seq)
		return NULL;
	count = PySequence_Fast_GET_SIZE(seq);
	if (count < 0 || count > UINT_MAX) {
		Py_DECREF(seq);
		PyErr_SetString(PyExc_ValueError, "iocb count out of range");
		return NULL;
	}
	if (!count) {
		Py_DECREF(seq);
		return PyLong_FromLong(0);
	}

	cbs = calloc((size_t)count, sizeof(*cbs));
	if (!cbs) {
		Py_DECREF(seq);
		return PyErr_NoMemory();
	}

	for (i = 0; i < count; i++) {
		PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
		PyObject *obj_py;
		unsigned int opcode;
		unsigned int rw_flags = 0;
		unsigned long long offset;
		unsigned long long user_data = 0;
		unsigned long long buf_addr;
		unsigned int buf_len;
		int host_pid = 0;

		/*
		 * iocb tuple:
		 * (opcode, obj, offset, buf_addr, buf_len
		 *  [, rw_flags=0, host_pid=0, user_data=0])
		 * obj: (NDS_IO_OBJ_FD, fd)
		 */
		if (!PyArg_ParseTuple(item, "IOKKI|IiK",
				      &opcode, &obj_py, &offset, &buf_addr,
				      &buf_len, &rw_flags, &host_pid,
				      &user_data)) {
			free(cbs);
			Py_DECREF(seq);
			return NULL;
		}
		cbs[i].opcode = opcode;
		cbs[i].rw_flags = rw_flags;
		cbs[i].offset = offset;
		cbs[i].user_data = user_data;
		cbs[i].buf_addr = buf_addr;
		cbs[i].buf_len = buf_len;
		cbs[i].host_pid = (int32_t)host_pid;
		if (parse_nds_obj(obj_py, &cbs[i].obj)) {
			free(cbs);
			Py_DECREF(seq);
			return NULL;
		}
	}
	Py_DECREF(seq);

	Py_BEGIN_ALLOW_THREADS
	ret = nds_io_submit(ctx, (unsigned int)count, cbs);
	Py_END_ALLOW_THREADS

	free(cbs);
	return PyLong_FromLong(ret);
}

static PyObject *py_nds_io_getevents(PyObject *Py_UNUSED(self), PyObject *args)
{
	PyObject *capsule;
	struct nds_io_ctx *ctx;
	struct nds_io_event *events = NULL;
	struct timespec ts_storage;
	struct timespec *timeout = NULL;
	unsigned int min_nr;
	unsigned int nr;
	double timeout_sec = -1.0;
	PyObject *list;
	int ret;
	int i;

	if (!PyArg_ParseTuple(args, "OII|d", &capsule, &min_nr, &nr, &timeout_sec))
		return NULL;
	ctx = nds_ctx_from_obj(capsule);
	if (!ctx)
		return NULL;
	if (!nr)
		return PyList_New(0);

	events = calloc(nr, sizeof(*events));
	if (!events)
		return PyErr_NoMemory();

	if (timeout_sec >= 0.0) {
		ts_storage.tv_sec = (time_t)timeout_sec;
		ts_storage.tv_nsec =
			(long)((timeout_sec - (double)ts_storage.tv_sec) *
			       1000000000.0);
		timeout = &ts_storage;
	}

	Py_BEGIN_ALLOW_THREADS
	ret = nds_io_getevents(ctx, min_nr, nr, events, timeout);
	Py_END_ALLOW_THREADS

	if (ret < 0) {
		free(events);
		return PyLong_FromLong(ret);
	}

	list = PyList_New(ret);
	if (!list) {
		free(events);
		return NULL;
	}
	for (i = 0; i < ret; i++) {
		PyObject *tup;

		tup = Py_BuildValue("(KLI)",
				    (unsigned long long)events[i].user_data,
				    (long long)events[i].res,
				    (unsigned int)events[i].opcode);
		if (!tup) {
			Py_DECREF(list);
			free(events);
			return NULL;
		}
		PyList_SET_ITEM(list, i, tup);
	}
	free(events);
	return list;
}

static PyMethodDef NdsMethods[] = {
	{ "init", py_nds_init, METH_VARARGS,
	  "init(fs_fds[, version=NDS_API_VERSION[, flags=0]]) -> int\n"
	  "fs_fds: sequence of file/block fds identifying the topology\n" },
	{ "exit", py_nds_exit, METH_NOARGS,
	  "exit() -> int\n" },
	{ "register_mem", py_nds_register_mem, METH_VARARGS,
	  "register_mem(addr, size[, flags=0]) -> int\n" },
	{ "unregister_mem", py_nds_unregister_mem, METH_VARARGS,
	  "unregister_mem(addr) -> int\n" },
	{ "io_new_ctx", py_nds_io_new_ctx, METH_VARARGS,
	  "io_new_ctx(max_io_nr[, flags=0]) -> ctx capsule | negative errno\n" },
	{ "io_destroy_ctx", py_nds_io_destroy_ctx, METH_VARARGS,
	  "io_destroy_ctx(ctx) -> int\n" },
	{ "io_submit", py_nds_io_submit, METH_VARARGS,
	  "io_submit(ctx, iocbs) -> int\n\n"
	  "iocb: (opcode, obj, offset, buf_addr, buf_len"
	  "[, rw_flags=0, host_pid=0, user_data=0])\n"
	  "obj: (NDS_IO_OBJ_FD, fd) or (NDS_IO_OBJ_ID, id)\n" },
	{ "io_getevents", py_nds_io_getevents, METH_VARARGS,
	  "io_getevents(ctx, min_nr, nr[, timeout_sec=-1]) -> list|int\n\n"
	  "On success returns [(user_data, res, opcode), ...].\n"
	  "On failure returns negative errno.\n"
	  "timeout_sec < 0 means wait forever; 0 means non-blocking.\n" },
	{ NULL, NULL, 0, NULL }
};

static struct PyModuleDef nds_module = {
	.m_base = PyModuleDef_HEAD_INIT,
	.m_name = "nds",
	.m_doc = "NDS GDS-aligned P2P API",
	.m_size = -1,
	.m_methods = NdsMethods,
};

PyMODINIT_FUNC PyInit_nds(void)
{
	PyObject *module;

	module = PyModule_Create(&nds_module);
	if (!module)
		return NULL;

	if (PyModule_AddIntConstant(module, "NDS_API_VERSION", NDS_API_VERSION) ||
	    PyModule_AddIntConstant(module, "NDS_IO_OBJ_FD", NDS_IO_OBJ_FD) ||
	    PyModule_AddIntConstant(module, "NDS_IO_OBJ_ID", NDS_IO_OBJ_ID) ||
	    PyModule_AddIntConstant(module, "NDS_IO_OP_PREAD", NDS_IO_OP_PREAD) ||
	    PyModule_AddIntConstant(module, "NDS_IO_OP_PWRITE", NDS_IO_OP_PWRITE) ||
	    PyModule_AddIntConstant(module, "NDS_IO_OP_PREADV", NDS_IO_OP_PREADV) ||
	    PyModule_AddIntConstant(module, "NDS_IO_OP_PWRITEV", NDS_IO_OP_PWRITEV) ||
	    PyModule_AddIntConstant(module, "NDS_IO_F_REGISTERED_MEM",
				    NDS_IO_F_REGISTERED_MEM)) {
		Py_DECREF(module);
		return NULL;
	}
	return module;
}
