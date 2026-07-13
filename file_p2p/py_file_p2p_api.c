#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "file_p2p_api.h"

static int parse_iovs(PyObject *object, struct p2p_iov **iov_out,
		      unsigned int *iov_nr_out)
{
	struct p2p_iov *iov;
	PyObject *sequence;
	Py_ssize_t count;
	Py_ssize_t i;

	sequence = PySequence_Fast(object, "IOVs must be a sequence");
	if (!sequence)
		return -1;
	count = PySequence_Fast_GET_SIZE(sequence);
	if (count <= 0 || count > UINT_MAX ||
	    (size_t)count > SIZE_MAX / sizeof(*iov)) {
		PyErr_SetString(PyExc_ValueError,
				"IOVs must be a non-empty bounded sequence");
		Py_DECREF(sequence);
		return -1;
	}

	iov = calloc(count, sizeof(*iov));
	if (!iov) {
		PyErr_NoMemory();
		Py_DECREF(sequence);
		return -1;
	}

	for (i = 0; i < count; i++) {
		PyObject *entry;
		PyObject **items;

		entry = PySequence_Fast(PySequence_Fast_GET_ITEM(sequence, i),
					"IOV must be (addr, size)");
		if (!entry)
			goto error;
		if (PySequence_Fast_GET_SIZE(entry) != 2) {
			PyErr_SetString(PyExc_ValueError,
					"IOV must be (addr, size)");
			Py_DECREF(entry);
			goto error;
		}
		items = PySequence_Fast_ITEMS(entry);
		iov[i].addr = PyLong_AsUnsignedLong(items[0]);
		iov[i].size = PyLong_AsUnsignedLong(items[1]);
		Py_DECREF(entry);
		if (PyErr_Occurred())
			goto error;
	}

	Py_DECREF(sequence);
	*iov_out = iov;
	*iov_nr_out = count;
	return 0;

error:
	free(iov);
	Py_DECREF(sequence);
	return -1;
}

static PyObject *py_read_file(PyObject *self, PyObject *args)
{
	int dev_fd = 0;
	const char *file_name = NULL;
	unsigned long file_offset = 0;
	struct p2p_iov *iov = NULL;
	unsigned int iov_nr = 0;
	PyObject *py_iovs = NULL;
	int ret = 0;

	if (!PyArg_ParseTuple(args, "iskO", &dev_fd, &file_name, &file_offset,
			      &py_iovs))
		return NULL;
	if (parse_iovs(py_iovs, &iov, &iov_nr))
		return NULL;

	struct read_parameter param = {
		.file_name = file_name,
		.file_offset = file_offset,
		.iov = iov,
		.iov_nr = iov_nr,
	};

	Py_BEGIN_ALLOW_THREADS ret = read_file(dev_fd, &param);
	Py_END_ALLOW_THREADS

	free(iov);
	return PyLong_FromLong((long)ret);
}

static PyObject *py_read_file_batch(PyObject *self, PyObject *args)
{
	int dev_fd = 0;
	struct read_parameter *params = NULL;
	PyObject *py_requests = NULL;
	PyObject *requests = NULL;
	Py_ssize_t count;
	Py_ssize_t i;
	int ret = 0;

	if (!PyArg_ParseTuple(args, "iO", &dev_fd, &py_requests))
		return NULL;
	requests = PySequence_Fast(py_requests, "requests must be a sequence");
	if (!requests)
		return NULL;
	count = PySequence_Fast_GET_SIZE(requests);
	if (count <= 0 || count > INT_MAX ||
	    (size_t)count > SIZE_MAX / sizeof(*params)) {
		PyErr_SetString(PyExc_ValueError,
				"requests must be a non-empty bounded sequence");
		goto error;
	}
	params = calloc(count, sizeof(*params));
	if (!params) {
		PyErr_NoMemory();
		goto error;
	}

	for (i = 0; i < count; i++) {
		PyObject *request;
		PyObject **items;
		const char *file_name;

		request = PySequence_Fast(PySequence_Fast_GET_ITEM(requests, i),
					  "request must be (file_name, file_offset, IOVs)");
		if (!request)
			goto error;
		if (PySequence_Fast_GET_SIZE(request) != 3) {
			PyErr_SetString(PyExc_ValueError,
					"request must be (file_name, file_offset, IOVs)");
			Py_DECREF(request);
			goto error;
		}
		items = PySequence_Fast_ITEMS(request);
		file_name = PyUnicode_AsUTF8(items[0]);
		if (!file_name) {
			Py_DECREF(request);
			goto error;
		}
		params[i].file_name = strdup(file_name);
		if (!params[i].file_name) {
			PyErr_NoMemory();
			Py_DECREF(request);
			goto error;
		}
		params[i].file_offset = PyLong_AsUnsignedLong(items[1]);
		if (PyErr_Occurred() ||
		    parse_iovs(items[2], &params[i].iov, &params[i].iov_nr)) {
			Py_DECREF(request);
			goto error;
		}
		Py_DECREF(request);
	}

	Py_BEGIN_ALLOW_THREADS ret = read_file_batch(dev_fd, params, count);
	Py_END_ALLOW_THREADS

	for (i = 0; i < count; i++) {
		free((void *)params[i].file_name);
		free(params[i].iov);
	}
	free(params);
	Py_DECREF(requests);
	return PyLong_FromLong((long)ret);

error:
	if (params) {
		for (i = 0; i < count; i++) {
			free((void *)params[i].file_name);
			free(params[i].iov);
		}
	}
	free(params);
	Py_XDECREF(requests);
	return NULL;
}

static PyObject *py_drain_read(PyObject *self, PyObject *args)
{
	int dev_fd = 0;
	int ret = 0;

	if (!PyArg_ParseTuple(args, "i", &dev_fd)) {
		return NULL;
	}

	ret = drain_read(dev_fd);

	return PyLong_FromLong((long)ret);
}

static PyObject *py_new_p2p_fd(PyObject *self, PyObject *Py_UNUSED(ignored))
{
	int ret = new_p2p_fd();

	return PyLong_FromLong((long)ret);
}

static PyObject *py_add_topo(PyObject *self, PyObject *args)
{
	const char *dev;
	int dev_fd;
	int ret;

	if (!PyArg_ParseTuple(args, "is", &dev_fd, &dev))
		return NULL;

	Py_BEGIN_ALLOW_THREADS ret = add_topo(dev_fd, dev);
	Py_END_ALLOW_THREADS
	return PyLong_FromLong((long)ret);
}

static PyObject *py_close_p2p_fd(PyObject *self, PyObject *args)
{
	int dev_fd = 0;
	int ret = 0;

	if (!PyArg_ParseTuple(args, "i", &dev_fd)) {
		return NULL;
	}

	ret = close_p2p_fd(dev_fd);

	Py_RETURN_NONE;
}

static PyMethodDef FileP2PMethods[] = {
	{ "read_file", py_read_file, METH_VARARGS,
	  "read_file(dev_fd, file_name, file_offset, iovs) -> int\n\n"
	  "Read file from p2p device.\n"
	  "\n"
	  "Parameters:\n"
	  "    dev_fd (int): File descriptor of p2p device.\n"
	  "    file_name (str): Name of file to read.\n"
	  "    file_offset (int): Starting file offset.\n"
	  "    iovs (sequence): Destination (address, size) pairs.\n"
	  "\n"
	  "Returns:\n"
	  "    int: 0 on success, non-zero on error.\n" },
	{ "read_file_batch", py_read_file_batch, METH_VARARGS,
	  "read_file_batch(dev_fd, requests) -> int\n\n"
	  "Read file batch from p2p device.\n"
	  "\n"
	  "Parameters:\n"
	  "    dev_fd (int): File descriptor of p2p device.\n"
	  "    requests (sequence): (file_name, file_offset, iovs) entries.\n"
	  "\n"
	  "Returns:\n"
	  "    int: 0 on success, non-zero on error.\n" },
	{ "drain_read", py_drain_read, METH_VARARGS,
	  "drain_read(dev_fd) -> int\n\n"
	  "Drain read from p2p device.\n"
	  "\n"
	  "Parameters:\n"
	  "    dev_fd (int): File descriptor of p2p device.\n"
	  "\n"
	  "Returns:\n"
	  "    int: 0 on success, non-zero on error.\n" },
	{ "new_p2p_fd", py_new_p2p_fd, METH_NOARGS,
	  "new_p2p_fd() -> int\n\n"
	  "New p2p device file descriptor.\n"
	  "\n"
	  "Returns:\n"
	  "    int: File descriptor on success, negative errno on error.\n" },
	{ "add_topo", py_add_topo, METH_VARARGS,
	  "add_topo(dev_fd, dev) -> int\n\n"
	  "Discover and register a supported block-device topology.\n"
	  "Returns 0 on success or a negative errno.\n" },
	{ "close_p2p_fd", py_close_p2p_fd, METH_VARARGS,
	  "close_p2p_fd(dev_fd) -> None\n\n"
	  "Close p2p device file descriptor.\n"
	  "\n"
	  "Parameters:\n"
	  "    dev_fd (int): File descriptor of p2p device.\n"
	  "\n"
	  "Returns:\n"
	  "    None\n" },
	{ NULL, NULL, 0, NULL }
};

static struct PyModuleDef file_p2p_module = {
	PyModuleDef_HEAD_INIT, "file_p2p", "p2p file access", -1,
	FileP2PMethods,
};

PyMODINIT_FUNC PyInit_file_p2p(void)
{
	return PyModule_Create(&file_p2p_module);
}
