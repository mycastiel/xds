# Registered-memory lifetime and teardown

## 1. Lifetime model

A registration has three conceptual states:

```text
ACTIVE  --revoke-->  DYING  --I/O drained and PA list put-->  DEAD
```

- `ACTIVE`: present in the global XArray and eligible for
  `percpu_ref_tryget_live()`.
- `DYING`: absent from the XArray; teardown kills the `percpu_ref` and waits
  for successful prior acquisitions.
- `DEAD`: the devmm PA list has been put and the kernel object is queued for
  RCU-deferred freeing.

These states are derived from XArray membership and `percpu_ref` liveness; the
object does not store a separate state enum.  The transition is one-way.  A
failed unregister cleanup does not re-publish the handle because readers may
already have observed its removal.

## 2. Explicit unregister

`IOCTL_UNREGISTER_MEM` is valid only from the registering thread group and
through the p2p open file description that created the registration.

The synchronous sequence is:

1. Under the global registration lock, enter an outer RCU read-side section,
   find the object by complete handle, and verify its owner batch matches the
   ioctl fd before comparing `owner_tgid` with `task_tgid(current)`.  Leave RCU,
   erase the object from the global XArray, and unlink it from the owner list.
2. Release the registration lock and call `percpu_ref_kill()` exactly once.
   A lookup that loaded the object before erase may race with the kill: a
   successful `percpu_ref_tryget_live()` is accounted by the ref transition,
   while an attempt that observes the dead ref fails.
3. Wait for the single `io_zero` completion driven by the `percpu_ref` release
   callback.  Completion means all previously accepted logical hardware I/O
   has finished.
4. Call `p2p_put_pinned_pa()` exactly once for the object's
   `p2p_pinned_pa`, then drop the referenced `owner_tgid`.
5. Call `call_rcu()` for the registration object's embedded `rcu_head`.  The
   callback calls `percpu_ref_exit()` and frees the object.

Unregister is allowed to sleep.  It does not return success while a request can
still DMA through the cached PA table, and the PA-list put completes in the
unregistering process context before the ioctl returns.  Only the kernel
registration object can remain until its RCU callback executes.

The order must be XArray erase, ref kill, I/O-ref drain, synchronous PA-list
put, and RCU-deferred object free.  The deferred free protects a lookup that
loaded the object before erase and is still accessing its handle or `io_refs`;
that reader cannot use `pinned_pa` unless it successfully acquired an I/O ref.
A separate kill-confirm completion is therefore unnecessary.

## 3. Owner-fd release

Every successful registration is stored in the global XArray and linked into
its owner `p2p_batch`.  The p2p file release path first walks the owner list and
erases every global entry so no handle remains visible, then kills every live
ref.  It drains I/O owned by that p2p fd, waits for every object's `io_zero`,
calls `p2p_put_pinned_pa()` for each, and queues each object for RCU-deferred
`percpu_ref_exit()` and freeing.  It must not destroy one object to completion
while later objects from the same fd remain visible.

Final file release does not take the global registration mutex.  The final
`struct file` release cannot race an ioctl on its own batch, and `xa_erase()`
provides internal XArray locking.  A foreign explicit-unregister lookup holds
outer RCU while it reads `owner`, so the object's `call_rcu()` protects that
rejection path from release-driven freeing.

Because registered-memory and topology teardown both queue RCU callbacks,
module exit uses one `rcu_barrier()` after new opens and callbacks have been
stopped and before destroying the topology release workqueue.  This prevents
module text or the workqueue from disappearing while callbacks remain queued.

This ownership is attached to the open file description, not to a numeric fd:

- `dup()` may use the same registrations and keeps
  them alive until the last duplicate is closed;
- `fork()` or `SCM_RIGHTS` may extend the owner open file description's lifetime
  but do not authorize the receiving process to read or unregister; and
- registered reads may use the handle through an independently opened p2p fd
  in the registering process.

Cross-process sharing is not supported.  A foreign reference to the owner fd
can delay automatic cleanup but cannot authorize registered reads or explicit
unregister.  Automatic cleanup still runs when any process closes the final
reference.  If the registering process closes the last owner-fd reference
while its SVM address space is still alive, fd release safely unregisters all
remaining memory before returning.

## 4. I/O versus unregister races

The full-handle registry lookup and live-reference acquisition form one logical
operation under RCU.

| Race result | Required behavior |
| --- | --- |
| Read sees no object after erase | Fail before submission |
| Read sees object but live-ref acquisition fails | Return `-ESTALE` before submission |
| Read acquires live ref before kill | Unregister waits for that logical I/O |
| Completion during submission | Submission sentinel prevents early logical completion |
| Hardware finishes before drain | Ref remains held; unregister waits for drain |
| The p2p fd is closed | Existing batch release drains its I/O contexts |

There must be no interval in which a read uses the PA list in
`mem->pinned_pa` without either holding the registration's live I/O reference
or being part of the serialized teardown path.

## 5. Confirmed devmm pin behavior

The current CANN SVM implementation used by XDS has two different lifetimes:
an allocation-local page reference and the SVM process object itself.

For `devmm_get_mem_pa_list()` on SVM memory:

1. `devmm_get_mem_pa_list()` calls `devmm_svm_get_and_pin_pa_list()`.
2. The SVM helper obtains the process and heap, resolves the PA list, and calls
   `devmm_inc_page_ref()` to prevent an ordinary free while the address is in
   use.
3. It then drops the temporary SVM process and heap references before returning
   to XDS.

An ordinary user free checks the allocation page reference and returns
`-EBUSY` while the count is greater than one.  A matching
`devmm_put_mem_pa_list()` looks up the process again and decrements the page
reference.

The important consequence is that a successful PA-list query does not retain a
long-lived reference to the SVM process object.

The registration's referenced `owner_tgid` stabilizes process identity for
authorization.  It does not retain the task, its `mm`, or its SVM process
object, so it does not change the PA-lifetime limitation.

## 6. Owner-process exit limitation

Linux process teardown calls `exit_mm()` before `exit_files()`.  Therefore, if
the registering process exits while its p2p fd remains open, the ordering is:

```text
process exits
    exit_mm()
        SVM MMU-notifier release
            remove the SVM process from normal lookup
            destroy its SVM mappings and PA tracking
            begin device-process resource teardown
    exit_files()
        XDS p2p fd release
            revoke registered handles
            wait XDS I/O
            call devmm_put_mem_pa_list()
```

The SVM notifier path performs process-wide teardown; it does not individually
balance the allocation reference acquired by `devmm_get_mem_pa_list()`.  Its
later heap cleanup frees the page-reference bookkeeping.  By the time XDS's fd
release calls `devmm_put_mem_pa_list()`, devmm may no longer find the SVM
process, so the put cannot supply a lifetime barrier for the cached PAs.

Consequently, binding registrations to the p2p fd guarantees XDS bookkeeping
cleanup but does not guarantee PA validity across an uncoordinated owner exit.
There is an interval between SVM MM teardown and XDS fd release in which
asynchronous hardware I/O may still be in flight against PAs that SVM is
tearing down.

An `execve()` that replaces the registering process's address space has the
same limitation while preserving its open p2p fd and thread-group identity.
The application must unregister all memory before either exiting or replacing
its address space.

This is a limitation of using `devmm_get_mem_pa_list()` without a devmm release
callback or retained SVM-process lifetime reference.  XDS cannot close that
ordering gap using only its own file release callback.

## 7. Supported application contract

The first version therefore requires coordinated owner lifetime:

1. The process keeps the HBM allocation and its SVM address space alive while
   the registration is active.
2. Every thread using the handle stops submitting new work, and the process
   drains its accepted I/O contexts.
3. The owner calls `unregister_mem()` and waits for it to return, or explicitly
   closes the last owner-fd reference while its SVM address space is intact.
4. Only then may the owner free the HBM or exit.

Normal HBM free while a registration exists is expected to fail with
`-EBUSY` because devmm holds the allocation page reference.  Applications must
not depend on process-exit teardown honoring the same check.

Unexpected termination such as `SIGKILL`, fatal signal, crash, or an
uncoordinated `execve()` is not a safe registered-memory revocation mechanism
in this version.  Owner-fd release must still remove handles and free XDS
objects to avoid registry leaks, but the driver cannot promise that
already-issued DMA remained valid during SVM teardown.

This limitation must be included in the public API documentation and release
notes.  Tests must not claim that killing the process with registered I/O in
flight is supported.

## 8. Normal cleanup cases

### 8.1 Explicit unregister before memory free

```text
process unregisters
    XDS removes handle
    I/O issuers drain accepted contexts
    XDS waits for their registration references
    XDS calls devmm_put_mem_pa_list
    process frees HBM
```

This is the primary and fully supported path.

### 8.2 Explicit p2p-fd close before process exit

```text
process closes last owner-fd reference while mm is alive
    XDS unregisters every owned range
    close returns
process exits or frees HBM
```

This is also supported, provided no duplicate reference to the owner file
description remains open.

## 9. Error handling during release

The devmm put function currently returns `void`, so unregister cannot report a
late failure to release the allocation reference.  XDS should still:

- guarantee at-most-once put invocation;
- log the handle, owner process identity, and range at a rate-limited level when
  release occurs after the owner SVM process is gone;
- erase the handle permanently; and
- free XDS bookkeeping after the required RCU and I/O barriers.

The log must not imply that the physical memory remained pinned through owner
exit.

## 10. Debug and drain lifetime

Registered I/O retains its registration reference until `drain_io()` performs
CRC or destination-content inspection and frees the I/O context.  This keeps
the cached PA table pinned for the complete debug-access interval.  As a
result, unregister can wait for an accepted I/O's issuing fd to drain even when
the hardware request has already completed.

## 11. Future boundary

A future version may replace the devmm PA-list query with an interface that
notifies XDS before owner memory teardown.  Such a change can make unexpected
owner exit safe by revoking the handle and waiting for XDS I/O before SVM frees
the allocation.

That callback-based lifetime is intentionally outside this design.  The first
implementation must use only `devmm_get_mem_pa_list()` and
`devmm_put_mem_pa_list()` and must retain the owner-exit limitation above.
