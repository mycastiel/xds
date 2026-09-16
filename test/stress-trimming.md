# Stress matrix trimming

## Recorded evidence

Baseline: `test-results/release_1-8bd8c1f-gcov-20260909/` in the parent
checkout containing `wt_test` (the artifacts are outside this worktree).
The run tested commit `8bd8c1f53476451204725f22bd4d3f0c935942c7` with
`6.6.0-xds-nokasan-gcov`, from 09:23:07 to 11:46:56 UTC on September 9,
2026: **2h 23m 49s**. See `full-test-report.md`, `full-suite.log`, and
`coverage-raw/*/*.gcov.json.gz` in that artifact directory.

The seven snapshots are disjoint and follow suite order in `full-suite.log`.
Counting positive line counts by `(source basename, line_number)` for
`dev.c`, `topo.c`, `debugfs.c`, `mem.c`, and `compat.h` gives:

| Suite | Snapshot | Covered lines | Additional lines in run order |
| --- | --- | ---: | ---: |
| basic | 1788947848533975325 | 1319 | 1319 |
| stress-raid0 | 1788949698865693249 | 1130 | 2 |
| stress-dm | 1788951483849841099 | 1122 | 0 |
| stress-nvme | 1788953257036459696 | 1112 | 0 |
| cq-race-raid0 | 1788953649574628893 | 1101 | 0 |
| cq-race-dm | 1788954033269900397 | 1093 | 0 |
| cq-race-nvme | 1788954416708013537 | 1083 | 0 |

Basic plus any one original stress topology covers all **1321** lines and
**99** functions reached by the full run. Stress adds baseline `topo.c:523`
and `topo.c:548`: taking a reference when another thread installs the same
topology, and destroying the losing candidate. Retain concurrent stress to
exercise this path; RAID0 also exercises stripe splitting. All four API
frontends and normal/registered memory modes remain in this workload.

Kernel log timestamp spans show where the CRC workload spent time:

| Workload group | Outputs | Sum of last-minus-first kernel timestamp |
| --- | ---: | ---: |
| RAID0 dynamic VA | 8 | 417.3 s |
| dm dynamic VA | 8 | 401.2 s |
| NVMe dynamic VA | 8 | 379.0 s |
| CQ race, all topologies | 12 | 1892.0 s |
| CQ drain, all topologies | 12 | 1875.6 s |

These are spans inside individual `.dmesg` files, not end-to-end suite
measurements. They omit setup, userspace checking, and work outside the first
and last messages. Removed CRC groups account for about **75.8 minutes** of
these recorded spans. The 36 separate live-drain rounds requested another
**18 minutes** of overlap and logged **2,603,670** accepted operations. Their
three snapshot intervals total about 19m 20s, including intervening setup.

## Default selection

- Keep basic functional and targeted error tests, plus live CQ smoke. Move
  fixed CQ smoke into the separate bounded suite below.
- Run RAID0 dynamic-VA stress: four APIs, two memory modes, 16 workers and
  16 iterations. Remove dm/NVMe stress from both default matrix runners.
- Remove the larger CRC CQ race/drain repetitions from `stress_test.sh`.
  `cq_check_test.sh` exercises both fixed modes in C and Python with normal
  and registered memory, without CRC: eight variants of 8 workers x 16
  iterations (1,024 operations total). Both matrix runners include it.
- Keep long live-drain testing opt-in through `XDS_RUN_CQ_RACE=1` in the VM
  runner, or through `cq_race_test.sh` directly. This retains registered
  live-drain pressure for targeted validation.
- Require the eight RAID0 dynamic-VA outputs in strict reports; also accept
  the archived complete 24-output matrix. Reject missing required variants
  and partially supplied extra topologies. New reports also require all
  eight bounded CQ variants, complete case sets, successful manifest checks,
  and mode-appropriate completion counts.

The default CRC stress workload falls from **43,008 to 2,048 operations**
(95.2% fewer). The new bounded CQ suite adds 1,024 non-CRC operations and
replaces the 512 fixed CQ smoke operations formerly in basic. Other basic
cases are unchanged. Standalone dm/NVMe dynamic stress
remains available through `XDS_STRESS_MODE`.

The September 11 coverage supplement added **81 lines** and entered the
remaining function through targeted failures. See
`test-results/release_1-coverage-errors-20260911/test-report.md` in the parent
checkout. This supports retaining targeted error/recovery tests.

## Limits

Coverage snapshots combine dynamic-VA and CQ workloads within each old stress
suite; they do not prove which subcase hit the two added topology lines.
Line coverage also cannot establish equivalent race-detection strength.
Removing high-pressure repetitions reduces that strength. The separate
bounded CQ suite retains manifest and completion accounting for both memory
modes; it does not restore sustained pressure or CRC data verification. The
retained dynamic-VA workload still registers
concurrently, but its coverage must be measured again to confirm those hits.
The baseline predates this worktree, and no new VM run or coverage collection
has been performed. The timing figures describe the recorded GCOV run, not a
promised runtime for the trimmed suite or for KASAN.

## Further basic-suite reduction

After remote basic validation measured 32m 04s and 33m 12s, the basic runner
added `XDS_BASIC_PROFILE=quick|full` (default quick). The earlier statements
about unchanged basic coverage describe the first trimming change. See the
README profile table for the current selection: all topologies and targeted
errors remain, while full cross-products and CRC-enabled live smoke are
opt-in. The selected manifests themselves are unchanged. Local profile tests
check representative API/mode coverage and exact manifest preservation.
The quick profile passed on the same non-KASAN GCOV VM in 9m 06s, down from
33m 12s (72.6% less time). Retained artifacts contain 304 main cases plus 16
registered-memory cases, 306 CRC checks, and 97 kernel logs with no serious
signatures. See `test-results/basic-quick-9574eb0-20260915T134424Z/` in the
validation checkout. The initial offline model overstated main case counts
by adding topology-boundary cases to direct NVMe devices; the model and
README now use the actual 1,320 full / 304 quick main cases. This correction
changes only local test modeling and documentation, not the VM-tested runner.
