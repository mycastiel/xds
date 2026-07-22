# Mandatory topology-aware I/O for NVMe, NVMe partitions, dm-linear, and MD RAID0

## 1. Scope

`p2p_dev` resolves the block device from the file descriptor and requires a
registered topology for every read.  Direct NVMe namespaces and their
partitions share a dedicated one-component NVMe backend.  For an NVMe
partition, dm-linear, or MD RAID0 device, the physical sectors returned by
FIEMAP are translated to sectors on the underlying whole NVMe namespaces
before NVMe read commands are submitted.

The supported configurations are deliberately narrow:

- a directly accessible NVMe namespace or one of its partitions, registered as
  an `nvme` topology;
- a dm device whose active table contains only contiguous `linear` targets;
- an MD array whose level is `raid0` and whose members form one uniform strip
  zone; and
- at most four component devices, each of which must be a directly accessible
  whole NVMe namespace or a partition backed by one.

The topology is a userspace-supplied, kernel-validated snapshot.  This design
does not introspect dm or MD internals in `p2p_dev`, monitor topology changes,
support nested topologies, or submit I/O through the upper dm/MD device.

## 2. Operating contract and limitations

Topology registration must happen before every read, including a read from a
direct NVMe namespace.  Once registered, the topology is visible to all open
`/dev/p2p_device` file descriptors, not only the fd that registered it.

The registered topology must remain unchanged while it is pinned.  In
particular, dm table reload, MD reshape, component replacement, slot changes,
and component size or data-offset changes are unsupported.  Before making
such a change, userspace must close every p2p fd that registered the topology,
perform the reconfiguration, and register the new topology.

There is no `DEL_TOPO` ioctl.  Each successful `ADD_TOPO` call owns one pin.
Closing that p2p fd drops all pins acquired through it.  Therefore, repeated
`ADD_TOPO` calls on one fd are permitted and create repeated pins; the close
path releases each of them.

When concurrent callers register the same top-device `dev_t`, the first
insertion wins.  Later callers use the existing topology and acquire a pin
without comparing their configuration with the installed configuration.  The
callers are responsible for supplying the same topology.  This is an explicit
simplification of this limited implementation.

A read that acquired a topology before its last pin is dropped keeps that
topology valid through mapping and request submission with a separate lifetime
reference.  Submitted requests then keep their component queues active until
completion.  The topology is removed from the global lookup tree immediately
after the last pin, so new reads can no longer acquire it.

## 3. UAPI

Device numbers use the 32-bit `new_encode_dev()` representation.  Userspace
constructs the same encoding from `major()` and `minor()`, and the kernel calls
`new_decode_dev()` before using it.

```c
#define P2P_TOPO_NAME_LEN 32
#define P2P_TOPO_MAX_BDEVS 4

struct topo_user_bdev {
	__u32 dev_id;
	__u32 reserved;
	/* Usable target/member length in 512-byte sectors. */
	__u64 size_sector;
	/* First usable sector relative to this configured component device. */
	__u64 start_sector;
};

struct topo_user_cfg {
	/* Exactly "nvme", "linear", or "raid0", including a terminating NUL. */
	char name[P2P_TOPO_NAME_LEN];
	__u32 top_dev;
	__u32 nr_devs;
	/*
	 * nvme:   extra[0] = extra[1] = 0
	 * linear: extra[0] = extra[1] = 0
	 * raid0:  extra[0] = ilog2(chunk size in sectors), extra[1] = 0
	 */
	__u64 extra[2];
	struct topo_user_bdev bdevs[];
};

#define IOCTL_ADD_TOPO _IOW('k', 1, struct topo_user_cfg)
```

The flexible array is not included in the encoded ioctl size.  The kernel
first copies and validates the fixed header, rejects `nr_devs == 0` or
`nr_devs > P2P_TOPO_MAX_BDEVS`, then copies exactly the bounded number of
component entries.

NSID is intentionally absent from both `p2p_read_param` and the topology UAPI.
The kernel obtains the actual NVMe NSID from each opened component bdev by
invoking its block-device ioctl operation with `NVME_IOCTL_ID`, then stores the
positive result in the kernel `topo_bdev`.  This avoids trusting a caller value
or confusing the independently allocated namespace instance in names such as
`nvme0n1` or `nvme0c1n1` with the NVMe command NSID.  Removing the old
`p2p_read_param.nsid` field intentionally changes the read ioctl ABI.

## 4. Kernel file organization and data structures

### 4.1 File ownership

The topology object implementation lives in `topo.c`, with its structures and
cross-file interfaces in `topo.h`.  The main driver source is `dev.c`.

`topo.c` owns:

- the statically initialized global topology radix tree and topology spinlock;
- a dedicated topology-release workqueue;
- topology allocation, validation, insertion, pinning, lookup, deletion, and
  RCU-delayed destruction;
- top and component bdev handles;
- component NSID lookup through `NVME_IOCTL_ID`;
- NVMe, dm-linear, and RAID0 mapping operations; and
- creation of the per-fd tracking entry for each successful `topo_add()`.

`topo.h` is the internal interface between `dev.c` and `topo.c`.  For
simplicity, it contains the topology structures, the per-fd tracking-list
structures, and the lifecycle entry points.  This also permits `dev.c` to call
the selected `topo_ops` mapping function directly.

`dev.c` includes `topo.h` and retains responsibility for:

- `IOCTL_ADD_TOPO` dispatch;
- embedding and initializing `shared_topo_list` in each `p2p_batch`;
- calling `topo_del()` for those pins during p2p-fd release;
- requiring a registered topology for every read;
- splitting reads by topology and destination queue limits; and
- constructing and submitting the NVMe command.

The topology UAPI structures and `IOCTL_ADD_TOPO` remain in
`p2p_dev_uapi.h`.  Userspace discovery and `add_topo()` remain in
`file_p2p/file_p2p_api.c` and `file_p2p/file_p2p_api.h`.

### 4.2 Module composition

`topo.c` is compiled as `topo.o` and linked into the existing `p2p_dev.ko`.
It must not become a separately loadable `topo.ko`, export module symbols, or
be source-included from `dev.c`.

The main driver source is renamed from `p2p_dev.c` to `dev.c`, so its component
object does not conflict with the final composite `p2p_dev.o`.  The module is
built directly as:

```make
obj-m += p2p_dev.o
p2p_dev-objs := dev.o topo.o
```

### 4.3 Definitions in `topo.h`

The topology and per-fd tracking definitions live in `topo.h`.  `topo.c` owns
their lifecycle, while `dev.c` may dereference them only for per-fd tracking
and mapping-result consumption.

```c
struct topo_bdev {
	dev_t id;
	u32 nsid;
	/* Usable target/member length in 512-byte sectors. */
	u64 size_sector;
	/* First usable sector on the normalized whole NVMe namespace. */
	u64 start_sector;
	struct bdev_handle *handle;
};

union topo_priv {
	u32 chunk_sectors_shift;
};

struct topo_ops {
	int (*map_sector)(struct topo *topo, u64 from_sector,
			  u32 in_nr_sectors, struct topo_bdev **to_bdev,
			  u64 *to_sector, u32 *out_nr_sectors);
};

struct topo {
	char name[P2P_TOPO_NAME_LEN];
	const struct topo_ops *ops;
	dev_t top_dev;
	u64 size_sector;
	u32 nr_bdevs;
	struct topo_bdev *bdevs;
	struct bdev_handle *top_handle;
	union topo_priv priv;

	/* Object lifetime: tree ownership plus active read references. */
	struct kref refcnt;
	/* Number of successful ADD_TOPO calls that still own a pin. */
	struct kref pin_refcnt;
	struct rcu_work rcu_work;
};

struct shared_topo {
	struct topo *topo;
	/* One entry per successful ADD_TOPO, tracked by the p2p fd. */
	struct list_head list;
};

struct shared_topo_list {
	spinlock_t lock;
	struct list_head head;
};
```

`struct p2p_batch`, which is the private data of a p2p fd, embeds
`struct shared_topo_list`.  Each entry in the list represents exactly one pin
owned by that fd.  Passing only this embedded list to `topo_add()` prevents
`topo.c` from depending on the rest of the private I/O-batch representation.

`topo.name` and `topo_bdev.id` remain in the installed object even though the
mapping path can operate from `ops` and the opened handles alone.  Registration,
pin, and release debug messages use them to identify the topology and component
device numbers.

The topology tree is a radix tree keyed by the encoded `dev_t` value.  Tree
mutation and `pin_refcnt` changes are serialized by the topology spinlock in
`topo.c`.  Changes to a batch's `shared_topo_list` use the batch-local spinlock
in `dev.c` and `topo.c`.  Operations that can sleep, including allocation,
opening or releasing bdevs, and copying userspace data, must never run under
either spinlock.

## 5. Topology creation and insertion

The ioctl handler uses the interface declared by `topo.h`:

```c
int topo_init(void);
void topo_exit(void);
int topo_add(const struct topo_user_cfg *cfg, struct shared_topo_list *list);
void topo_del(struct topo *topo);
struct topo *topo_get(struct block_device *top_bdev);
void topo_put(struct topo *topo);
```

`topo_add()` returns zero on success or a negative errno on failure.  Before
calling it, the ioctl handler copies the complete variable-length userspace
configuration into kernel memory.  `topo_add()` allocates a `shared_topo` and,
on success, stores the topology installed in or found in the radix tree in that
node, links it into the supplied `shared_topo_list`, and owns one `pin_refcnt`
for the caller.  The ioctl handler frees the copied configuration and returns
the integer result directly.  The pin is released by `topo_del()` when the
related p2p fd closes.

`topo_get()` returns NULL when the top bdev has no registered topology,
`ERR_PTR(-ESTALE)` when the topology found by an RCU reader has already reached
a zero lifetime reference count, or a valid topology with one read-path
`refcnt`.  `topo_put()` releases that reference after the read has mapped and
submitted all of its requests.  Callers must test `IS_ERR()` and reject a NULL
result with `-ENODEV`.

`topo_add()` performs these steps:

1. Decode `top_dev`, then allocate a `shared_topo` tracking entry.
2. Under the topology spinlock, look up `top_dev`.  If it already exists,
   increment its `pin_refcnt`, link it into the supplied per-fd list, and return
   success without validating or opening the duplicate caller's component list.
3. If the initial lookup misses, validate the copied header and component array
   and allocate a candidate `topo` and its component array.
4. Open the top and component bdevs read-only with `bdev_open_by_dev()`.  The
   handles pin the devices against removal and `dev_t` reuse.
5. Validate the common and topology-specific constraints.  For each component,
   invoke its bdev ioctl operation with `NVME_IOCTL_ID` and store the positive
   result in `topo_bdev.nsid`.
6. Initialize `refcnt` and `pin_refcnt` to one.  The initial `refcnt` is the
   global-tree reference; the initial `pin_refcnt` belongs to this ADD call.
7. Call `radix_tree_preload(GFP_KERNEL)`, then look up `top_dev` again under the
   spinlock to close the race with another creator.  Insert the candidate if it
   is still absent; otherwise pin the topology that won the race.
8. Store the pinned topology in the preallocated `shared_topo` and link it into
   the supplied list under `list->lock`.
9. End the radix-tree preload.  On the raced-duplicate path, synchronously
   release the unused candidate and all bdev handles outside both spinlocks.

Failures before insertion synchronously destroy the candidate.  Once a
candidate is inserted, every exit path must either link its pinned pointer into
the preallocated `shared_topo` and return success or undo the insertion and its
initial references.  Linking the pointer does not allocate and cannot fail.

### 5.1 Common kernel validation

The kernel does not trust userspace topology discovery when creating a new
topology.  It validates:

- `name` is NUL-terminated and is exactly `nvme`, `linear`, or `raid0`;
- `reserved` and every topology-specific unused `extra` field are zero;
- `nr_devs` is between one and four;
- an `nvme` topology has exactly one component;
- a `linear` or `raid0` component does not equal `top_dev`, and configured
  component IDs are unique;
- each configured component is a whole NVMe namespace or a partition whose
  containing whole device is a supported namespace, never an NVMe multipath
  head;
- every size is nonzero;
- the configured `start_sector + size_sector` does not overflow and does not
  exceed the configured whole device or partition;
- for a partition component, the kernel-provided partition geometry guarantees
  that adding `get_start_sect(component)` produces a range within the
  containing whole namespace;
- all calculated top sizes and mapped sectors are overflow-safe; and
- `NVME_IOCTL_ID` is supported and returns a positive NSID for every component.

The kernel opens each configured component before classifying it.  A whole
namespace handle is retained directly.  For a partition, the kernel first
validates the configured range relative to that partition, obtains its exact
whole bdev with the same 5.10/6.6 compatibility helper, adds the partition
start to the configured start, opens and retains an independent handle to the
whole namespace, and releases the temporary partition handle.  The internal
`topo_bdev` therefore always contains a whole-namespace device ID, handle,
NSID, and effective namespace-relative start sector.

For `nvme`, the kernel derives the required relationship from the opened top
bdev.  It obtains the exact containing whole bdev through a compatibility
helper that uses `bd_contains` on Linux 5.10 and `bdev_whole()` on Linux 6.6.
The configured component must be that exact bdev.  If the top bdev is a whole
namespace, `start_sector` must be zero and `size_sector` must equal the opened
namespace size.  If the top bdev is a partition, `start_sector` must equal
`get_start_sect(top_bdev)` and `size_sector` must equal the opened partition
size.  The common range check also proves that the complete partition range
fits in the whole namespace.

For `NVME_IOCTL_ID`, the helper asserts that the validated component's
`block_device_operations` provides `ioctl`, then calls that operation with the
opened bdev, `BLK_OPEN_READ`, `NVME_IOCTL_ID`, and argument zero.  It does not
parse `disk_name` and does not pass this lookup through userspace.

The top device is expected to be a direct NVMe namespace or partition, dm, or
MD device according to `name`.  The `nvme` whole-device or partition checks
validate its exact relationship with the component without trusting userspace
discovery.  For dm and MD, the kernel checks the upper-device identity exposed
by sysfs but does not reconstruct the live table or metadata.  Exact
correspondence with the active stack therefore remains part of the userspace
operating contract.

## 6. Lifetime and concurrency

### 6.1 Lookup

The read path performs lockless lookup:

```c
rcu_read_lock();
topo = radix_tree_lookup(&topo_tree, top_bdev->bd_dev);
if (!topo)
	goto out;
if (!kref_get_unless_zero(&topo->refcnt)) {
	topo = ERR_PTR(-ESTALE);
	goto out;
}

out:
rcu_read_unlock();
return topo;
```

The `kref_get_unless_zero()` failure covers an object whose lifetime count has
already reached zero but whose memory remains visible to an earlier RCU
reader.  A reader that obtained the tree pointer before removal may still
acquire a reference and proceed if another reference keeps the object alive;
otherwise it receives `-ESTALE`.  A successful result owns one `refcnt`, so
radix-tree removal can proceed concurrently without invalidating that reader.
The read path holds the reference while mapping and submitting requests, then
drops it through `topo_put()` after command submission has finished.

The acquired `refcnt` keeps all topology state and bdev handles alive through
request allocation and submission.  Each allocated block request holds its
component request queue active until that request completes, so the topology
reference is not needed by completion or drain.

### 6.2 Pin release

`p2p_release()` in `dev.c` detaches the batch's `shared_topo_list` under its
batch-local spinlock.  After dropping the lock, it calls `topo_del()` once for
every detached entry and then frees the tracking entries.  `topo_del()` drops
one `pin_refcnt`, using the topology spinlock in `topo.c` to serialize the
possible last put with concurrent insertion.  The final-pin release callback
executes while protected by that spinlock and:

1. deletes the topology from the radix tree; and
2. releases the spinlock.

It then drops the tree's initial `refcnt` outside the spinlock.  The reference
keeps the detached topology alive across this handoff.  A concurrent add may
install a new topology for the same top device before the old reference is
dropped; the two objects have independent lifetimes.

An RCU reader that obtained the old tree pointer before removal may acquire a
live reference and complete against that topology.  A reader that cannot
acquire the reference returns `-ESTALE`.

The final `refcnt` release cannot call `bdev_release()`, because it can occur
from an atomic context and because RCU readers may still hold the old pointer.
Instead, it calls
`queue_rcu_work(topo_release_wq, &topo->rcu_work)`.  After the RCU grace period,
the work function releases the top and component bdev handles and frees the
topology.

`topo_init()` allocates `topo_release_wq` before the character device is
published.  The workqueue is unbound because `bdev_release()` may block, and it
uses `WQ_MEM_RECLAIM` so topology teardown can make progress under memory
pressure.  Failure to allocate the workqueue fails module initialization with
`-ENOMEM`.

Module exit first prevents new opens/ioctls and completes all p2p-fd release
paths, then calls `topo_exit()`.  `topo_exit()` verifies that the radix tree is
empty, calls `rcu_barrier()` so every pending `queue_rcu_work()` callback has
queued its work, and finally calls `destroy_workqueue(topo_release_wq)`.  The
destroy operation drains all queued topology-release work before freeing the
dedicated workqueue.  It never flushes `system_wq` and therefore does not wait
on unrelated kernel work.

## 7. Mapping algorithms

`dev.c` calls `topo->ops->map_sector()` directly.  The operation translates at
most `in_nr_sectors` beginning at a logical sector on the top device and
returns a `topo_bdev`, its physical sector, and the number of sectors that
remain contiguous on that component without crossing a topology boundary.
The returned `topo_bdev` supplies both the destination bdev handle and NSID.
The dm-linear and RAID0 operations return an error for a sector outside the
registered topology.  Creation-time range and arithmetic validation guarantees
that mapping an in-range sector cannot overflow.  The NVMe operation is an
unchecked offset mapping for FIEMAP sectors from the registered namespace or
partition.

### 7.1 NVMe namespace or partition

The `nvme` backend has exactly one component and both `extra` values are zero.
For a whole namespace, the component device ID equals `top_dev`,
`start_sector` is zero, and `size_sector` equals the opened namespace size.  For
a partition, the component is the containing whole NVMe namespace,
`start_sector` is the partition start on that namespace, and `size_sector`
equals the opened partition size.  Both forms use the same dedicated
`nvme_map_sector()` callback:

```text
to_bdev        = bdevs[0]
to_sector      = bdevs[0].start_sector + from_sector
out_nr_sectors = in_nr_sectors
```

For a whole namespace, the zero start makes the callback a pure pass-through.
For a partition, it applies the validated partition offset.  In both cases it
returns the input sector count unchanged and does not reuse the dm-linear
mapping callback.

### 7.2 dm-linear

Userspace orders entries by the logical start of their dm table targets.  The
kernel requires the sum of their `size_sector` values to equal the top bdev
size.  Because the table is required to start at logical sector zero and be
contiguous, an entry's logical start is the sum of the preceding entry sizes.
If a target component is a partition, its configured target offset is relative
to that partition and the kernel adds the partition start when constructing
the internal entry.

For the entry containing `from_sector`:

```text
entry_logical_start = sum(size of preceding entries)
entry_offset        = from_sector - entry_logical_start
to_sector           = entry.start_sector + entry_offset
out_nr_sectors      = min(in_nr_sectors,
                          entry.size_sector - entry_offset)
```

The returned length stops at a linear-target boundary.  The normal read
splitting logic may shorten it further for an IOV boundary or destination
queue limit.

### 7.3 MD RAID0

This implementation supports only a single uniform strip zone.  Userspace
orders entries by MD slot.  The kernel requires:

- `nr_devs >= 2`;
- a nonzero power-of-two chunk size represented by
  `1U << chunk_sectors_shift` sectors, with `chunk_sectors_shift < 32`;
- identical configured member offsets and `size_sector` values for every
  member;
- `size_sector` is a multiple of the chunk size; and
- the top bdev size equals `nr_devs * size_sector`.

For a logical `from_sector`:

```text
chunk_sectors  = 1 << chunk_sectors_shift
chunk_index    = from_sector / chunk_sectors
offset_in_chunk = from_sector % chunk_sectors
member_index   = chunk_index % nr_devs
member_chunk   = chunk_index / nr_devs

to_sector = bdevs[member_index].start_sector +
            member_chunk * chunk_sectors + offset_in_chunk

out_nr_sectors = min(in_nr_sectors,
                     chunk_sectors - offset_in_chunk)
```

The returned length never crosses a RAID0 chunk boundary.  Equal member sizes
exclude the multiple-strip-zone behavior used by RAID0 arrays with unequal
members.  A member offset is relative to its configured member device.  For a
partition member, the kernel adds that partition's start only to the internal
namespace-relative mapping offset.  Partition starts may differ without
relaxing the requirement that the MD-reported member offsets and sizes match.

## 8. Read-path integration

After resolving the bdev from `file_fd`, `p2p_read_file()` calls
`topo_get(bdev)`:

- if `IS_ERR(topo)` is true, it returns `PTR_ERR(topo)`;
- if `topo` is NULL, it returns `-ENODEV` because registration is mandatory; or
- otherwise, it maps every source extent through `topo->ops->map_sector()` and
  drops the topology reference after all possible requests have been
  submitted.

For a topology read, each iteration is limited by all of:

- sectors remaining in the FIEMAP extent;
- bytes remaining in the current physical-address IOV segment;
- sectors returned by `map_sector()`, which stop at the end of an NVMe
  namespace, a dm target, or a RAID0 chunk boundary; and
- `p2p_queue_max_sectors()` for the mapped component bdev.

`dev.c` submits the request to `to_bdev->handle->bdev`'s queue, uses the
returned `to_sector`, and places `to_bdev->nsid` in the NVMe command.  There is
no caller-supplied NSID or direct-device fallback.  Queue limits are calculated
per mapped component; an upper dm/MD queue limit is insufficient.

### 8.1 Per-request command construction

Topology boundaries can create substantially more NVMe commands than the
current `DIV_ROUND_UP(data_size, max_bytes) * 10` allocation.  The fixed
`cmd_list` capacity is therefore removed.

Each call to `do_read_io()` declares and zero-initializes its command on the
stack:

```c
struct nvme_command cmd = { };
```

The submission path fills `cmd`, allocates the block request, and passes
`&cmd` to `nvme_init_request()`.  In the target kernel,
`nvme_init_request()` copies the complete command into the NVMe request-private
storage before returning, so the stack object is no longer referenced when
`do_read_io()` returns.  The block request retains `io_ctx` directly in
`req->end_io_data`; the existing completion handler records status, drops
`io_ctx->io_ref`, and completes the I/O context when the last request finishes.
No command-count estimate, command array, command allocation, or additional
completion context is needed.

If submission fails after some requests were already issued, `issue_err`
records the first issue-side error.  The I/O context remains on the batch list,
and drain still waits for and frees all successfully submitted requests.  The
topology reference is dropped immediately after the submission loop, including
this partial-submission case.

Drain takes a snapshot of the I/O contexts already published on the batch
list.  Submission and drain ioctls may overlap: an I/O context published after
the drain snapshot is not covered by that drain and is handled by a later
drain or by final fd release.  Likewise, concurrent drains do not all wait for
the same snapshot.  A caller that needs completion status or intends to reuse
or unmap a destination physical-address range must serialize its final drain
with submissions and other drains.

## 9. Userspace topology discovery

The library adds:

```c
int add_topo(int dev_fd, const char *dev);
```

`dev` must name the top block device.  The helper obtains its `dev_t` with
`stat()` and probes `/sys/dev/block/<major>:<minor>/`:

- `partition` identifies a partition.  The helper resolves its parent whole
  namespace and returns an `nvme` configuration;
- `device/subsysnqn` identifies a direct NVMe namespace.  The helper returns
  an `nvme` configuration;
- `dm/` selects dm discovery; and
- `md/` selects MD discovery.

Unknown devices and unsupported stacks return `-EOPNOTSUPP`.  System-call and
library failures are returned as negative errno values.

### 9.1 NVMe discovery

Open the supplied block-device path and issue `BLKGETSIZE64`, rejecting a zero
or non-512-byte-aligned result.  Set `name = "nvme"`, `nr_devs = 1`, and
`top_dev` to the supplied device.  Convert the returned byte size to
`bdevs[0].size_sector`; both `extra` values remain zero.

For a whole namespace, validate it as a non-multipath NVMe namespace, set
`bdevs[0].dev_id = top_dev`, and set `start_sector = 0`.  When the top device
exposes the `partition` sysfs attribute, read `../dev` to obtain and validate
the containing whole NVMe namespace, set that device as `bdevs[0].dev_id`, and
read the partition's `start` attribute in 512-byte sectors.  The kernel
independently verifies the applicable whole-device or partition relationships.

### 9.2 dm-linear discovery

Query the active table through the device-mapper ioctl UAPI in
`<linux/dm-ioctl.h>`.  Open `/dev/mapper/control`, issue `DM_TABLE_STATUS` with
`DM_STATUS_TABLE_FLAG` and the top device number into a fixed 16 KiB result
buffer, and iterate the returned `dm_target_spec` records.  The supported table
has at most four small linear records, so `DM_BUFFER_FULL_FLAG` is rejected with
`-E2BIG` rather than growing the buffer.  This discovery path has no build or
runtime dependency on libdevmapper.

Reject the table unless:

- it has between one and four targets;
- every target type is exactly `linear`;
- the first target starts at sector zero;
- every target begins where the preceding target ends, with no gap or overlap;
- every target length is nonzero;
- each target parameter contains exactly one component device and component
  start sector; and
- every component is a whole NVMe namespace or a partition whose parent is a
  whole NVMe namespace according to sysfs.

For each target, encode its configured whole-device or partition `dev_t`, set
`size_sector` to the target length, and set `start_sector` to the offset from
the linear target parameters.  The offset remains relative to the configured
device; the kernel performs partition translation.  Preserve target order in
`bdevs[]`.  Set `name = "linear"` and both `extra` values to zero.

### 9.3 MD RAID0 discovery

Read `/sys/dev/block/<major>:<minor>/md/level`, `chunk_size`, `raid_disks`,
`degraded`, and the member `dev-*` directories.  Reject the array unless:

- `level` is exactly `raid0`;
- `raid_disks` is between two and four;
- `degraded` is zero;
- the chunk size is nonzero, a power of two, a multiple of 512 bytes, and its
  sector count can be represented by the kernel shift field;
- there is exactly one active member for every slot from zero through
  `raid_disks - 1`;
- all member offsets are equal;
- all member sizes are equal and are multiples of the chunk size; and
- every member is a whole NVMe namespace or a partition whose parent is a
  whole NVMe namespace according to sysfs.

Read each member's `slot`, `offset`, and `size`, sort by slot, and fill the
component array in that order.  Set `name = "raid0"`, set `extra[0]` to
`ilog2(chunk_size / 512)`, and set `extra[1]` to zero.

Member offsets and sizes are compared before partition translation and retain
the existing equality requirements.  The kernel independently adds each
partition start when building the internal whole-namespace mapping.

Userspace takes one snapshot while assembling the structure.  It does not
repeat the full discovery because a second snapshot cannot close the remaining
race before registration.  The no-reconfiguration operating contract remains
required.

## 10. Error behavior

Use consistent errors for topology operations:

- `-EFAULT`: userspace copy failed;
- `-EINVAL`: malformed fields, invalid ranges, invalid topology arithmetic,
  or a configuration that does not match the selected topology type;
- `-E2BIG`: component count or variable-size allocation exceeds a limit;
- `-ENODEV`: a device number cannot be opened or a required device disappeared;
- `-ESTALE`: `topo_get()` found an RCU-visible topology whose lifetime
  reference count had already reached zero;
- `-EOPNOTSUPP`: unsupported top-device type, component type, NVMe multipath
  head, or nested topology;
- `-ENOMEM`: allocation or radix-tree preload failed; and
- the underlying bdev-open or ioctl error when it gives more precise failure
  information.

If no topology is registered for the resolved bdev, the read returns
`-ENODEV`.  The read ioctl does not discover or register a topology implicitly.

## 11. Implementation order

1. Rename `p2p_dev.c` to `dev.c`, add `topo.c` and `topo.h`, then update
   Makefile/Kbuild so `dev.o` and `topo.o` form the existing `p2p_dev.ko`.
2. Extend the UAPI and userspace library with topology structures,
   `IOCTL_ADD_TOPO`, and `add_topo()` discovery.
3. Add topology objects, radix-tree lookup, spinlock-protected insertion/pin
   management, RCU-work destruction, and per-fd tracking.
4. Add strict common, NVMe, dm-linear, and RAID0 validation plus component NSID
   lookup through `NVME_IOCTL_ID`.
5. Implement and unit-test all three sector-mapping operations independently.
6. Hold topology references through `do_read_ios()` and integrate mapped bdev,
   sector, NSID, boundary, and queue-limit selection into request submission.
7. Replace `cmd_list` with a stack-local command copied by
   `nvme_init_request()` for each submitted request.
8. Call `topo_init()` before publishing the character device and call
   `topo_exit()` from every later initialization-unwind path and from module
   exit after new file operations have been prevented.

## 12. Validation plan

### Module layout tests

- build the external module after adding `topo.c` and confirm that `topo.o` is
  linked into `p2p_dev.ko`;
- confirm `modules.order` contains `p2p_dev.ko` but no `topo.ko`;
- confirm `modinfo p2p_dev.ko` retains the intended module name and
  dependencies; and
- confirm there are no exported `topo_*` symbols or source inclusion of
  `topo.c`.

### Mapping tests

- direct NVMe reads at sector zero and the final namespace sector;
- NVMe-partition reads at the first and final partition-relative sectors;
- dm-linear reads wholly inside each target and reads crossing every target
  boundary;
- RAID0 reads at the first/last sector of a chunk, across a chunk boundary,
  across a full stripe, and at the final array sector;
- arithmetic near the end of every component range; and
- dm-linear and RAID0 mapping input outside the top-device size.

### Registration and lifetime tests

- direct NVMe `add_topo()` installs and pins an `nvme` topology;
- NVMe-partition `add_topo()` installs and pins an `nvme` topology keyed
  by the partition device;
- two threads concurrently add the same top device and both fds can read;
- repeated adds on one fd create pins that are all released by close;
- the topology remains visible after one of multiple owner fds closes;
- the last owner close removes it from new lookup;
- a read acquired before the last owner close completes safely afterward; and
- a `topo_get()` racing with last-owner close returns either a referenced
  topology or `-ESTALE`, never a topology without a lifetime reference; and
- repeated register/close and module unload exercise the RCU release path.

### End-to-end I/O tests

- direct NVMe after explicit registration, including a namespace whose NSID is
  not one;
- an NVMe partition with data comparison across its first and last filesystem
  allocation regions;
- dm-linear with two to four NVMe components, including reads crossing target
  boundaries, and with whole-namespace or partition components;
- uniform two- to four-member MD RAID0, including reads crossing chunk and
  stripe boundaries, and with whole-namespace or partition members whose raw
  MD offsets and sizes remain equal;
- mixed component queue limits;
- enough small RAID0 chunks to produce more requests than the old `* 10`
  command estimate; and
- data comparison against buffered reads from the upper block device or a file
  hosted on it.

### Negative tests

- read without a registered topology;
- non-linear dm target, discontinuous dm table, and nested dm/MD component;
- RAID level other than RAID0, degraded/missing member, duplicate slot,
  unequal member offset or size, and invalid chunk size;
- `nvme` with a partition top and a component from another disk, or a
  mismatched partition start or size;
- NVMe multipath head, non-NVMe component, invalid NSID ioctl, component range
  beyond a configured partition or its whole bdev, and more than four
  components; and
- topology reconfiguration attempts while pins exist, which are documented as
  unsupported and must be prevented by the caller.
