# introduction

XDS provides a kernel module and userspace library to implement direct storage transfer between an NVMe SSD and an NPU.

# build

## `p2p_dev.ko`

```
$ make mod
$ ls p2p_dev.ko
p2p_dev.ko
```

If the kernel source is not located at `/lib/modules/$(shell uname -r)/build`, use KSRC to specify its location:

```
$ make KSRC=/home/begin/code/oe_knl mod
```

## `libnds.so`

```
$ make lib
$ ls file_p2p/libnds.so file_p2p/libnds.so.0.1
```

## NDS Python binding

Currently it is built in-place.

```
$ make binding
$ ls file_p2p/nds.cpython*.so
```
