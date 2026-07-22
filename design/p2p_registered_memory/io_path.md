# Registered-memory registration and I/O path

## 1. Kernel object model

The registered-memory implementation belongs in `dev.c` initially.  It can be
split into a separate component object later if the implementation obscures the
read path, but that split is not required for the first version.

The conceptual object is:

```c
struct p2p_registered_mem {
	u64 handle;
	u64 addr;
	u64 size;
	struct p2p_pinned_pa *pinned_pa;
	struct p2p_batch *owner;
	struct pid *owner_tgid;

	struct percpu_ref io_refs;
	struct completion io_zero;
	struct rcu_head rcu;
	struct list_head owner_node;
};
```

The full `u64 handle` is used directly as the XArray index.  No explicit state
enum is needed because XArray membership and `percpu_ref` liveness define
whether the object accepts new I/O.

The registered object reuses `p2p_pinned_pa` rather than duplicating its devmm
process ID, aligned-range maps, page sizes, and PA-list ownership.  A
registration owns one `p2p_pinned_pa` containing one `p2p_iov_map`; normal
unregistered reads continue to use the same structure with one map per IOV.

The module owns one global XArray and mutex.  Each `p2p_batch` keeps a list of
registrations it owns so file release can revoke them.  The implementation must
preserve these separate concepts:

- global visibility by complete handle;
- unregister and cleanup ownership by the registering open file description;
- authorization by the registering thread-group identity;
- cached devmm translation state;
- accepted logical-I/O lifetime.

## 2. Registry and locking

Use one module-wide XArray keyed by the complete handle.  Reads perform an
RCU-protected global lookup.  A global registration mutex serializes XArray
publication, explicit removal, and owner-list updates during ioctls.  A separate
owner-list lock is unnecessary because final file release cannot race an ioctl
using the same open file description.  Release may walk its private owner list
without the global mutex; `xa_erase()` provides the XArray's internal locking.

The lookup sequence is:

```text
RCU read lock
    load object by complete handle from the global XArray
    acquire percpu_ref only if it is still live
RCU read unlock
verify current belongs to owner_tgid
validate every IOV against the immutable registered range
```

The handle is not split during lookup and no separate cookie comparison is
needed.

Explicit unpublish holds the global registration mutex to serialize competing
ioctls and uses an outer RCU read-side section around `xa_load()` and the
`owner` and `owner_tgid` comparisons.  Although `xa_load()` takes RCU
internally, that section ends before it returns and does not protect later
field access.  Check `owner` first.  Once it matches the ioctl's `p2p_batch`,
the ioctl's active file reference prevents release of that batch, so unpublish
may leave RCU before erasing the entry.  This control lookup does not acquire an
I/O `percpu_ref`.

Object memory remains allocated until all of these are true:

- it has been erased from the XArray;
- its `percpu_ref` has been killed;
- all successful I/O references have been returned; and
- every RCU reader that could have loaded its pointer has passed through the
  grace period provided by the object's `call_rcu()` callback.

`percpu_ref` does not supply the RCU grace period.  Teardown must use explicit
RCU-deferred freeing.

Erasing the global XArray entry prevents new lookups.  A lookup that loaded
the pointer immediately before removal can race with `percpu_ref_kill()`: if
its `percpu_ref_tryget_live()` succeeds, that reference is included in the
count folded by the kill; if it observes the dead ref, it fails.  `io_zero`
cannot be signalled until every successful acquisition has been accounted and
returned.

After `io_zero`, no lookup can newly reach `pinned_pa` and no accepted I/O can
still use it, so unregister may call `p2p_put_pinned_pa()` synchronously in its
process context.  A pre-existing RCU reader may still hold the registration
pointer while it compares the handle or attempts the now-dead `io_refs`.
Teardown therefore calls `call_rcu()` after putting the PA list; the callback
calls `percpu_ref_exit()` and frees the registration object.  One completion is
sufficient; neither `synchronize_rcu()` nor a separate kill-confirm completion
is needed.

No devmm call, completion wait, or `kvfree()` may execute while the global
registration mutex is held.  XArray insertion may use `GFP_KERNEL`.

## 3. Registration path

`IOCTL_REGISTER_MEM` operates in the registering process's context:

1. Copy and validate the fixed request.  Reject zero size, address overflow,
   and a nonzero 8-byte reserved field.
2. Capture the process identity from `current`.  Do not accept an owner PID
   supplied by userspace.  Retain `get_task_pid(current, PIDTYPE_TGID)` for
   authorization until teardown.
3. Call `devmm_get_mem_page_size()` for the requested range.  Require the same
   supported power-of-two device page-size constraints as the current read
   path.
4. Align the start down and the end up to that page size with checked
   arithmetic.  Keep the original range separately for later authorization.
5. Allocate one `p2p_pinned_pa`, one `p2p_iov_map`, and the required PA list.
   Store the process ID, aligned range, page size, and page count through those
   existing structures.
6. Call `devmm_get_mem_pa_list()` once and mark the single map pinned only after
   it succeeds.
7. Initialize `io_zero` and initialize `io_refs` with
   `PERCPU_REF_INIT_DEAD` so every pre-publication error can call
   `percpu_ref_exit()` directly.
8. Build the handle from the next global 48-bit atomic sequence ID and one
   16-bit random prefix.  Equal random prefixes are valid because IDs are not
   recycled.
9. Write only `p2p_mem_register_param.mem_handle` back to userspace.
10. Insert the dead object in the global XArray by its complete handle, link it
    to the owner batch, and call `percpu_ref_reinit()` before releasing the
    registration lock.

The object becomes visible only after every fallible devmm, allocation, and
userspace-copy step has succeeded.  Before XArray insertion, the dead ref lets
cleanup call `devmm_put_mem_pa_list()` and `percpu_ref_exit()` directly without
RCU teardown or an I/O wait.  Every failure after capturing `owner_tgid` drops
that PID reference.  Normal teardown drops it after accepted I/O reaches zero.

## 4. Cached VA-to-PA translation

The registered object uses its single `p2p_iov_map` and associated PA bases
stored in `p2p_pinned_pa`.  For an IOV
`[iov_addr, iov_addr + iov_size)`, translation starts with:

```text
map          = &mem->pinned_pa->maps[0]
range_offset = iov_addr - map->aligned_addr
page_index   = range_offset / map->page_size
page_offset  = range_offset % map->page_size
```

The first physical segment is:

```text
pa  = map->pa_list[page_index] + page_offset
len = min(iov_size, map->page_size - page_offset)
```

Later segments use the following page bases and are clipped when the IOV ends.
The resulting exact `p2p_pa_iov[]` contains no registration alignment padding.

Before performing this calculation, use subtraction-based bounds checks so an
overflowing user address cannot pass validation:

```c
if (iov_addr < mem->addr || iov_size > mem->size ||
    iov_addr - mem->addr > mem->size - iov_size)
	return -ERANGE;
```

The existing destination-sector alignment and `p2p_pa_iov` alignment checks
remain unchanged.

## 5. Registered read path

The existing `p2p_read_file()` flow remains responsible for copying IOVs and
extents, validating their totals, opening the source file, and acquiring the
topology.  The destination translation step becomes mode-specific:

```text
(flags & P2P_IO_F_REGISTERED_MEM) == 0
    ignore mem_handle
    current get_pa_iov(host_pid, ...)
    io_ctx owns p2p_pinned_pa until drain

(flags & P2P_IO_F_REGISTERED_MEM) != 0
    require mem_handle != 0
    global lookup + percpu_ref_tryget_live
    require task_tgid(current) == owner_tgid
    validate registered VA IOVs
    build exact p2p_pa_iov[] from cached pages
    io_ctx owns one live registration I/O reference
```

Reject unknown flag bits and a zero registered-memory handle before destination
translation.  The handle is never inspected to infer the mode.

After successful lookup, a registered read takes exactly one registration
reference regardless of:

- IOV count;
- number of HBM pages;
- number of FIEMAP extents;
- topology splits; or
- number of generated NVMe requests.

The reference is transferred to `p2p_io_context` before any request is issued.
Every error after that transfer completes the logical I/O normally so the
reference is returned exactly once.

`p2p_io_context` owns one tagged pinned-memory value:

```c
struct p2p_pinned_io_mem {
	bool pinned;
	bool reg_mem;
	union {
		struct p2p_pinned_pa *pinned_pa;
		struct p2p_registered_mem *registered_mem;
	};
};
```

`reg_mem` selects exactly one cleanup operation: normal I/O puts its per-read
PA pin, while registered I/O puts its registration reference.  `pinned` says
whether the structure currently owns that resource.  Passing the structure to
I/O-context creation transfers ownership by copying it into the context and
clearing the caller's `pinned` bit.  The copied PA IOV array, pinned-memory
ownership, and normal I/O result fields remain owned by the I/O context until
drain.

## 6. Completion accounting

The current `io_ref` starts at one and gains one reference for every submitted
NVMe request.  Today the initial reference is dropped by `wait_io_done()` at
drain time.  Registered-memory lifetime requires a different boundary:

1. Keep the initial reference while `do_read_ios()` may still submit more
   requests.  This prevents an early device completion from finishing the
   logical I/O during submission.
2. Drop the initial reference immediately when `do_read_ios()` returns,
   including its partial-submission error path.
3. Each NVMe completion drops one child reference.
4. Whichever path drops the final reference records logical hardware
   completion and completes `io_done`.
5. `wait_io_done()` only waits and reports status; it no longer drops the
   submission reference.
6. Drain performs CRC/content inspection while the registration is still live,
   then returns the one registration reference when freeing the I/O context.

A common helper should own the final transition so the registered-memory
reference cannot be returned twice:

```c
static void p2p_io_ctx_put(struct p2p_io_context *io_ctx)
{
	if (!atomic_dec_and_test(&io_ctx->io_ref))
		return;

	complete(&io_ctx->io_done);
}
```

The exact ordering must ensure all DMA completion status writes are visible to
the drain waiter before `complete()`.

## 7. Why `percpu_ref`

Using a registration-wide `atomic_t` would add one operation on the same cache
line for every logical read issue and completion.  Several threads or
submission CPUs targeting one large HBM registration could therefore serialize
on cache-line ownership.

`percpu_ref_tryget_live()` and `percpu_ref_put()` update per-CPU counters in
the common active state.  The cost of folding those counters into a global
atomic is paid during the uncommon unregister operation.

This is preferable to custom sharded counters because it already supplies the
required active-to-dying transition and a way to reject new references.

The remaining `io_ctx->io_ref` atomic is local to one logical I/O.  Its child
NVMe completions may contend with each other, but unrelated reads of the same
registration do not share it.

## 8. Failure and ownership rules

The registered path follows these rules:

- Failure before `tryget_live`: the read owns no registration reference.
- Failure after `tryget_live` but before I/O-context creation: put the
  registration reference directly.
- Failure after I/O-context creation but before any NVMe request: drop the
  submission reference, which completes the context.
- Partial issue failure: enqueue the context as today, drop the submission
  reference, and let existing requests finish.
- Drain: inspect the completed destination, put the registration once, and free
  the copied PA IOVs and context.
- p2p fd release: drain its contexts before waiting for registrations it owns.

The normal-memory path selected by a clear `P2P_IO_F_REGISTERED_MEM` bit
keeps its current `p2p_pinned_pa` ownership unless it is deliberately
refactored separately.

## 9. Debug-data access

The CRC and content-dump helpers access destination physical addresses after
`wait_io_done()` in the drain path.  Registered I/O therefore retains its one
registration reference through these helpers and returns it only when drain
frees the context.  An explicit unregister racing accepted I/O can wait for the
issuing fd to drain.

## 10. Focused validation

At minimum, add coverage for:

- one registered IOV inside one HBM page;
- clear `P2P_IO_F_REGISTERED_MEM` with the handle ignored;
- set `P2P_IO_F_REGISTERED_MEM` with valid and zero handles;
- every unknown read-flag bit;
- use through another fd in the same process succeeds;
- use through another process is rejected;
- unregister through a non-owner fd is rejected;
- unaligned-to-page but sector-aligned registration and IOV boundaries;
- several IOVs in one registration;
- many repeated reads with one devmm get and one final devmm put;
- partial NVMe submission failure followed by unregister;
- concurrent issue and unregister on several CPUs;
- handle lookup racing with XArray removal;
- monotonically distinct full handles; and
- registered reads with post-drain PA debug access enabled.
