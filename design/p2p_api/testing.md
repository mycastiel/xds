# NDS testing

> **Status:** Historical proposal. The authoritative implemented v1 contract
> is [fix3](fix3.md); conflicting test-surface descriptions below are
> superseded.

This document is part of the [p2p_api](../p2p_api.md) design.  It must not be
implemented in isolation from the [UAPI](uapi.md), [backend](backend.md),
[completion](completion.md), or [objects](objects.md) contracts.

It defines the validation model for the NDS facade.  NDS landing must
**keep passing the full existing `file_p2p` suite** and **add a parallel NDS
suite** that mirrors that suite’s structure (basic + stress), not a one-off
smoke that replaces it.

## 1. Environment

NDS tests run in the same disposable QEMU NVMe CMB guest as
[`test/README.md`](../../test/README.md):

- 128 MiB CMB, two 64 MiB namespaces;
- `nvme.use_cmb_sqes=0`;
- `stub.ko` + `p2p_dev.ko` (CRC-enabled rebuild when CRC checks are used);
- modules that include `IOCTL_GET_IO_EVENTS` (and submit `user_data` echo)
  when claiming faithful getevents;
- mandatory `XDS_DEV_1` / `XDS_DEV_2`.

Destructive device rules from the write-testing design remain in force: no
mounted or production devices.

## 2. Gate: existing tests still pass

Before or together with NDS results, the prepared VM must still pass the
current XDS entry points unchanged in intent:

| Existing entry | Must still pass |
| --- | --- |
| `test/basic_test.sh` | full topology × API × FS × write/registration matrix |
| `test/stress_test.sh` | `raid0` / `dm` / `nvme` modes as today |
| Dual-kernel / in-VM matrix runners | when used for release, same as today |

Additive kernel UAPI (`IOCTL_GET_IO_EVENTS`, submit cookie) must not break
existing `file_p2p` callers: old binaries keep using `drain_io` only.

## 3. New NDS tests — mirror the existing shape

NDS adds **corresponding** coverage, not a thinner substitute.

| Existing (`file_p2p`) | New (NDS) | Relationship |
| --- | --- | --- |
| `basic_test.sh` | `nds_basic_test.sh` (name flexible) | Same prepared-VM contract; same topology/FS/write/CRC intent; drive **`nds_*`** instead of `file_p2p_*` |
| `stress_test.sh` | `nds_stress_test.sh` | Same worker/iteration/CMB-VA stress idea; submit/getevents (+ both memory modes) |
| `c_api_test` / `python_api_test.py` | `nds_api_test` (+ optional Python) | single/queued/threaded-style loads via NDS |
| `rw_api_test` / rejection tests | NDS rw + rejection | opcode/obj/mem-flag negatives |
| `mem_registration_test` | NDS registered-mem cases | handle out-param, reuse, unregister-by-addr |

Matrix runners (`run_all_tests_in_vm.sh` / dual-kernel) should gain NDS basic +
NDS stress alongside the existing suites once NDS lands, so kasan/nokasan
reports cover both APIs.

### 3.1 NDS basic (functional) requirements

Mirror `basic_test.sh` intent at least for:

- direct NVMe raw read/write pipelines (seed → HBM → writeback → host
  verify);
- both **registered** and **one-shot** HBM modes;
- ext4 read coverage (1K/2K/4K) on the first namespace;
- partition / dm-linear / RAID0 phases as the NDS facade matures (first
  landing may stage topology expansion, but the **target** is parity with
  basic_test’s topology set — not permanent direct-NVMe-only);
- CRC + debugfs checks on reads when `CALC_CRC32` is enabled;
- regular-file preallocation and raw XDS write/readback, including
  unwritten extents;
- rejection: read-only file write; shared-extent write; bad mem flags;
  `OBJ_ID`.

### 3.2 NDS stress requirements

Mirror `stress_test.sh` intent:

- multi-worker concurrent submit/getevents;
- `XDS_STRESS_MODE` in {`raid0`,`dm`,`nvme`} (or equivalent);
- dynamic HBM VA allocation constraints as today where applicable;
- both normal and registered-memory variants;
- CRC / allocation-manifest checks analogous to `check_stress.py`.

Faithful **`min_nr` / timeout** getevents behavior is required in stress and
queued basic cases once `IOCTL_GET_IO_EVENTS` is present (see
[completion.md](completion.md) §5).  Do not claim those semantics under a
drain-only shim.

## 4. What must be proven (cross-cutting)

### 4.1 Dual HBM memory modes

| Case | Requirement |
| --- | --- |
| Registered | `nds_register_mem` returns handle; submit/getevents with `NDS_IO_F_REGISTERED_MEM`; stub PA get/put shows reuse |
| One-shot | submit/getevents with clear flags and VA+size |
| Rejection | inconsistent flag/handle → `-EINVAL` before NVMe |

### 4.2 Submit / getevents

- Batch submit; getevents echoes `user_data`.
- `min_nr` / `max_nr` / timeout against `IOCTL_GET_IO_EVENTS` (timeout with no
  completions returns `0`).
- Partial batch accept still harvestable.
- Sync pread/pwrite == submit(1)+getevents(1).
- Destroy may use full `drain_io`; getevents tests must not.

### 4.3 Objects and topology

- `NDS_IO_OBJ_PATH` and `NDS_IO_OBJ_FD` for bdev; file read; file write
  rejected; auto `add_topo` for supported layouts.

### 4.4 Reliability

- No fd sharing across ctxes; destroy/shutdown leave modules unloadable;
  stale handle fails cleanly.

## 5. Pass criteria

A change set that claims NDS v1-complete is acceptable when:

1. **All existing** `file_p2p` basic + stress (and matrix, if run) still pass.
2. **New NDS basic + NDS stress** pass on the same prepared VM, covering the
   mirrored requirements in §3–§4.
3. Applications never need io_uring or raw `drain_io` for NDS completions.
4. getevents timeout/`min_nr` tests run against `IOCTL_GET_IO_EVENTS`, not a
   drain-only approximation.
5. Failures return negative errno; debugfs failure/inflight counters stay
   healthy after successful suites.

## 6. Non-goals

- Dropping the legacy `file_p2p` suite once NDS exists.
- Using kernel CRC as write verification (writes still use host read-back).
- Performance benchmarking as a correctness gate.
- Shipping “NDS done” with only a direct-NVMe smoke and no stress counterpart.
