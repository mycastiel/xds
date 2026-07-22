# XDS registered HBM memory

## 1. Status and scope

This document is the entry point for adding reusable HBM memory registration
to XDS.  The first implementation deliberately continues to use
`devmm_get_mem_pa_list()` and `devmm_put_mem_pa_list()`.

The current read path asks devmm to translate and pin every destination IOV for
every `read_file()` call.  Registration moves that work out of the I/O issue
path: a process registers an HBM VA range once and reuses the returned opaque
handle plus the original VA values in later `read_file()` calls from the same
process.

This design covers:

- registration and synchronous unregistration ioctls;
- global lookup through an opaque 64-bit memory handle;
- reuse of `read_file()`, `read_parameter`, and `p2p_iov`;
- one cached VA-to-PA table per registration;
- scalable I/O lifetime accounting; and
- automatic bookkeeping cleanup when the owning p2p device fd is released.

This version does not provide a hard PA-lifetime guarantee when the owner exits
or replaces its address space without first closing its p2p fd or unregistering
its HBM.  The reason and the supported lifecycle contract are described in
[the lifetime design](p2p_registered_memory/lifetime.md).

## 2. Sub-design documents

- [UAPI and userspace API](p2p_registered_memory/uapi.md) defines the handle,
  ioctls, `read_file()` extension, validation rules, and error behavior.
- [Registration and I/O path](p2p_registered_memory/io_path.md) defines the
  kernel objects, global lookup, cached-PA translation, and fast-path
  refcounting.
- [Lifetime and teardown](p2p_registered_memory/lifetime.md) defines explicit
  unregister, fd release, races with I/O, and the limitation imposed by the
  current devmm interface.

These documents form one design.  A sub-design must not be implemented in
isolation if doing so violates the ownership or teardown rules in another.

## 3. Chosen design

### 3.1 Registration and reuse

A process registers memory through one `/dev/p2p_device` open file description
and may use the handle through another p2p fd opened by the same process:

```text
registering process
    open /dev/p2p_device -> dev_fd
    register_mem(dev_fd, &register_param)

same process
    open /dev/p2p_device -> io_fd
    read_file(io_fd, {
        .flags = P2P_IO_F_REGISTERED_MEM,
        .mem_handle = register_param.mem_handle,
        .iov = [{ .addr = registered_va + displacement, .size = length }]
    })

registering process
    unregister_mem(dev_fd, &unregister_param)
```

`p2p_iov.addr` is the registering process's numeric VA.  It is a coordinate
within the registered range, not an offset and not a pointer that the
registered I/O path dereferences.  Preserving the VA makes the buffer identity
unambiguous without overloading the address field with a second meaning.

The registration is stored in a module-wide XArray.  Any p2p fd in the
registering thread group may issue a read with the handle.  The registration
retains `struct pid *owner_tgid`, and a read from another process is rejected.
Explicit unregister requires both the registering process and the open file
description that created the registration.  Closing that description's last
reference unregisters all memory it owns, regardless of which process closes
the final reference.

### 3.2 Handle format

The memory handle is an opaque nonzero `u64`:

```text
63                 48 47                                      0
+--------------------+------------------------------------------+
| random: 16 bits    | registry object ID: 48 bits              |
+--------------------+------------------------------------------+
```

The low 48 bits come from one global `atomic64_t` sequence and are not recycled.
The high 16 bits are sampled independently for each registration; equal random
prefixes on different sequence IDs are valid.  The complete handle is the
XArray index, so lookup does not split the handle or compare a second stored
cookie.

The random prefix is not a security boundary.  The handle is an opaque lookup
key, not a cross-process sharing or authorization token.

### 3.3 Read flags and one handle per logical read

`flags` and `mem_handle` belong to `p2p_read_param`, not to each `p2p_iov`.
`flags` is an `unsigned long`, and bit 0 is
`P2P_IO_F_REGISTERED_MEM`.  Consequently, all destination IOVs in one
`read_file()` call use the same memory mode and, in registered mode, must lie
within the same registration.  This has three useful properties:

- the existing ordered IOV semantics remain unchanged;
- the issue path performs one registry lookup and takes one registration
  reference per logical read; and
- the I/O context needs only one registered-memory owner.

A transfer that spans several registrations is submitted as several
`read_file()` calls and collected by the existing `drain_io()` operation.

When `P2P_IO_F_REGISTERED_MEM` is clear, the request uses the current
one-shot path.  That path ignores `mem_handle`, uses `host_pid`, and calls devmm
for every read.  When the bit is set, the request uses the registered path,
requires a nonzero handle, ignores `host_pid`, and resolves every IOV through
the cached PA table.  The handle value itself never selects the memory mode.
Unknown read-flag bits are rejected.

### 3.4 Registration object and cached translation

Each registered-memory object records:

- the full handle used directly as the global XArray index;
- the exact exported range `[addr, addr + size)`;
- a referenced thread-group PID identifying the registering process;
- one existing `p2p_pinned_pa` containing the process ID, aligned map, device
  page size, and PA array returned by `devmm_get_mem_pa_list()`; and
- a live I/O reference counter and one teardown completion.

Registration calls `devmm_get_mem_page_size()` and
`devmm_get_mem_pa_list()` once.  I/O issue validates the supplied registered VA
against the exact exported range and calculates the corresponding page-array
index and page offset.  It does not call devmm again.

Unregister first makes the handle unreachable, then waits for previously
accepted hardware I/O and calls `devmm_put_mem_pa_list()` synchronously.  It
RCU-defers only `percpu_ref_exit()` and freeing the registration object so a
lookup that loaded the object before removal can finish accessing `io_refs`.

### 3.5 I/O reference counting

A single atomic counter in a popular registration would bounce one cache line
between CPUs.  The registration therefore uses `struct percpu_ref`:

- lookup uses `percpu_ref_tryget_live()` once per logical read;
- drain returns that logical read's reference with `percpu_ref_put()` once; and
- unregister erases the global handle, calls `percpu_ref_kill()`, and waits
  for one release completion; after putting the PA list in unregister context,
  it uses `call_rcu()` to release `io_refs` and free the object.

The existing per-I/O `io_ref` continues to count child NVMe requests.  The
registered-memory reference remains in `p2p_io_context` through drain so CRC
and content inspection still run against pinned memory.  An unregister racing
accepted I/O therefore waits until the issuing fd drains those contexts.

## 4. Ownership summary

| Resource | Owner | Released by |
| --- | --- | --- |
| Registered-memory handle | Module-wide XArray while active | Unregister or owner-fd release |
| Cleanup ownership | Registering open file description | Explicit unregister or last close |
| Process authorization | Referenced registering TGID | After accepted I/O reaches zero |
| devmm PA-list pin | Registered-memory object | After accepted I/O reaches zero |
| Logical registered-memory I/O reference | `p2p_io_context` | `drain_io()` or fd release |
| I/O result and `p2p_io_context` | p2p fd batch | `drain_io()` or fd release |

The I/O result context retains the registered PA table through drain so the
existing post-completion debug access remains safe.

## 5. Required implementation areas

The implementation is expected to touch these areas while preserving the
current topology and file-extent behavior:

- `p2p_dev_uapi.h`: registration structures, ioctls, read flags, and
  `mem_handle` in the read request;
- `dev.c`: registry, owner tracking, cached translation, I/O references,
  unregister, and fd-release cleanup;
- `file_p2p/file_p2p_api.h`: userspace registration functions and
  `read_parameter.flags` plus `read_parameter.mem_handle`;
- `file_p2p/file_p2p_api.c`: ioctl wrappers and registered-read population;
- `file_p2p/py_file_p2p_api.c`: Python representation of the same contract;
- `stub.c`: deterministic registration and handle-lifetime test support; and
- `test/`: same-process reuse through another fd, wrong-process and
  wrong-owner-fd rejection, race, and stale-handle coverage.

No topology UAPI or file-extent mapping behavior changes as part of memory
registration.

## 6. Acceptance criteria

The feature is complete when all of the following hold:

1. One successful registration performs one devmm VA-to-PA query, and repeated
   registered reads perform no further devmm query for that memory.
2. The registering process can issue repeated reads using the same fd, handle,
   and original numeric VA without another devmm query.
3. A registered handle can be used for I/O through another fd in the same
   process.  Another process cannot use or explicitly unregister it, and only
   its owner open file description can explicitly unregister it.
4. Unknown read flags, a missing or unknown registered-memory handle, and an
   out-of-range VA are rejected before any NVMe command is submitted.
5. Unregister prevents new reads, waits for accepted I/O contexts to drain, and
   then calls `devmm_put_mem_pa_list()` exactly once.
6. Closing the owner p2p fd performs the same cleanup for every registration it
   still owns.
7. High-concurrency registered reads do not update one registration-wide
   atomic cache line on every issue and completion.
8. The documented owner-exit/address-space-replacement limitation remains
   visible in the UAPI documentation and tests; fd cleanup must not be
   represented as a hard SVM PA-lifetime guarantee.
