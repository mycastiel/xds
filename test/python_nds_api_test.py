#!/usr/bin/env python3

import argparse
import csv
import errno
import os
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

import nds


HARVEST_TIMEOUT_SEC = 300.0


@dataclass
class TestCase:
    case_id: str
    path: str
    file_offset: int
    cmb_va: int
    length: int
    expected: int
    fd: Optional[int] = None
    result: Optional[int] = None


@dataclass
class StressCase:
    iteration: int
    worker: int
    case_id: str
    path: str
    file_size: int
    file_offset: int
    length: int
    cmb_va: Optional[int] = None
    fd: Optional[int] = None
    result: Optional[int] = None


def integer(value: str) -> int:
    return int(value, 0)


def manifest_rows(path: Path, field_count: int) -> List[List[str]]:
    rows: List[List[str]] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for line_number, row in enumerate(csv.reader(stream, delimiter="\t"), 1):
            if not row or row[0].startswith("#"):
                continue
            if len(row) != field_count:
                raise ValueError(
                    f"manifest line {line_number} must have "
                    f"{field_count} TSV fields"
                )
            rows.append(row)
    if not rows:
        raise ValueError("manifest contains no test cases")
    return rows


def load_manifest(path: Path) -> List[TestCase]:
    tests: List[TestCase] = []
    for line_number, row in enumerate(manifest_rows(path, 6), 1):
        test = TestCase(
            case_id=row[0],
            path=row[1],
            file_offset=int(row[2], 0),
            cmb_va=int(row[3], 0),
            length=int(row[4], 0),
            expected=int(row[5], 0),
        )
        if not test.case_id or not test.path or test.length <= 0:
            raise ValueError(f"invalid manifest entry at data row {line_number}")
        tests.append(test)
    return tests


def load_stress_manifest(path: Path) -> List[StressCase]:
    tests: List[StressCase] = []
    for line_number, row in enumerate(manifest_rows(path, 7), 1):
        test = StressCase(
            iteration=int(row[0], 0),
            worker=int(row[1], 0),
            case_id=row[2],
            path=row[3],
            file_size=int(row[4], 0),
            file_offset=int(row[5], 0),
            length=int(row[6], 0),
        )
        if (
            not test.case_id
            or not test.path
            or test.file_size <= 0
            or test.file_offset < 0
            or test.length <= 0
        ):
            raise ValueError(f"invalid stress entry at data row {line_number}")
        tests.append(test)
    return tests


def open_case_fds(tests: Sequence) -> None:
    for test in tests:
        if test.fd is None:
            test.fd = os.open(test.path, os.O_RDONLY | os.O_DIRECT)


def close_case_fds(tests: Sequence) -> None:
    for test in tests:
        if test.fd is not None:
            os.close(test.fd)
            test.fd = None


def new_ctx(max_io_cnt: int):
    ctx = nds.io_new_ctx(max_io_cnt)
    if isinstance(ctx, int):
        raise RuntimeError(f"nds.io_new_ctx returned {ctx}")
    return ctx


def destroy_ctx(ctx) -> None:
    result = nds.io_destroy_ctx(ctx)
    if result:
        raise RuntimeError(f"nds.io_destroy_ctx returned {result}")


def harvest(ctx, nr: int) -> None:
    got = 0
    while got < nr:
        events = nds.io_getevents(ctx, 1, nr - got, HARVEST_TIMEOUT_SEC)
        if isinstance(events, int):
            raise RuntimeError(f"nds.io_getevents returned {events}")
        if not events:
            raise TimeoutError(errno.ETIMEDOUT, "nds.io_getevents timed out")
        got += len(events)


def submit(ctx, test: TestCase, flags: int, user_data: int = 0) -> None:
    if test.fd is None:
        raise RuntimeError(f"{test.case_id}: file fd not opened")
    ret = nds.io_submit(
        ctx,
        [
            (
                nds.NDS_IO_OP_PREAD,
                test.fd,
                test.file_offset,
                [(test.cmb_va, test.length)],
                flags,
                0,
                user_data,
            )
        ],
    )
    # submit returns count of accepted iocbs; normalize to 0 like rw_file.
    test.result = 0 if ret == 1 else ret
    print(
        f"api=nds-python case={test.case_id} ret={test.result} "
        f"expected={test.expected}",
        flush=True,
    )


def check_result(test: TestCase) -> None:
    if test.result != test.expected:
        raise RuntimeError(
            f"{test.case_id}: returned {test.result}, expected {test.expected}"
        )


def require(name: str, actual: int, expected: int) -> None:
    print(
        f"api=nds-python case={name} ret={actual} expected={expected}",
        flush=True,
    )
    if actual != expected:
        raise RuntimeError(f"{name} returned {actual}, expected {expected}")


def require_value_error(name: str, operation) -> None:
    try:
        operation()
    except ValueError:
        require(name, -errno.EINVAL, -errno.EINVAL)
        return
    raise RuntimeError(f"{name} did not raise ValueError")


def run_reject(topology: str) -> None:
    topo_fd = os.open(topology, os.O_RDONLY | os.O_DIRECT)
    require(
        "reject-max-io-cnt",
        nds.io_new_ctx(nds.NDS_IO_MAX_IO_CNT + 1),
        -errno.EINVAL,
    )
    ctx = new_ctx(4)
    try:
        buf_addr = 0
        buf_len = 8192

        def submit_fd(fd, iov=None) -> int:
            if iov is None:
                iov = [(buf_addr, buf_len)]
            return nds.io_submit(
                ctx,
                [
                    (
                        nds.NDS_IO_OP_PREAD,
                        fd,
                        0,
                        iov,
                    )
                ],
            )

        require_value_error(
            "reject-iov-empty",
            lambda: submit_fd(topo_fd, []),
        )
        require_value_error(
            "reject-fd-overflow",
            lambda: submit_fd(2**32 + topo_fd),
        )

        require(
            "reject-reg-unregistered-buf",
            nds.io_submit(
                ctx,
                [
                    (
                        nds.NDS_IO_OP_PREAD,
                        topo_fd,
                        0,
                        [(buf_addr, buf_len)],
                        nds.NDS_IO_F_REGISTERED_MEM,
                        0,
                        1,
                    )
                ],
            ),
            -errno.EINVAL,
        )
        require(
            "reject-host-pid-neg",
            nds.io_submit(
                ctx,
                [
                    (
                        nds.NDS_IO_OP_PREAD,
                        topo_fd,
                        0,
                        [(buf_addr, buf_len)],
                        0,
                        -1,
                        4,
                    )
                ],
            ),
            -errno.EINVAL,
        )
        require(
            "reject-reg-host-pid",
            nds.io_submit(
                ctx,
                [
                    (
                        nds.NDS_IO_OP_PREAD,
                        topo_fd,
                        0,
                        [(buf_addr, buf_len)],
                        nds.NDS_IO_F_REGISTERED_MEM,
                        1,
                        5,
                    )
                ],
            ),
            -errno.EINVAL,
        )
        require(
            "reject-unknown-rw-flags",
            nds.io_submit(
                ctx,
                [
                    (
                        nds.NDS_IO_OP_PREAD,
                        topo_fd,
                        0,
                        [(buf_addr, buf_len)],
                        nds.NDS_IO_F_REGISTERED_MEM << 1,
                        0,
                        6,
                    )
                ],
            ),
            -errno.EOPNOTSUPP,
        )
        require(
            "reject-offset-overflow",
            nds.io_submit(
                ctx,
                [
                    (
                        nds.NDS_IO_OP_PREAD,
                        topo_fd,
                        2**64 - 4096,
                        [(buf_addr, buf_len)],
                    )
                ],
            ),
            -errno.EOVERFLOW,
        )

        events = nds.io_getevents(ctx, 1, 1, 0.0)
        if isinstance(events, int):
            if events < 0:
                raise RuntimeError(f"nds.io_getevents returned {events}")
            count = events
        else:
            count = len(events)
        require("reject-getevents-empty", count, 0)
        require(
            "reject-getevents-timeout-nan",
            nds.io_getevents(ctx, 0, 1, float("nan")),
            -errno.EINVAL,
        )
        require(
            "reject-getevents-timeout-inf",
            nds.io_getevents(ctx, 0, 1, float("inf")),
            -errno.EINVAL,
        )
        require(
            "reject-getevents-timeout-neg-inf",
            nds.io_getevents(ctx, 0, 1, float("-inf")),
            -errno.EINVAL,
        )
    finally:
        destroy_ctx(ctx)
        os.close(topo_fd)


def run_single(ctx, tests: List[TestCase], flags: int) -> None:
    for index, test in enumerate(tests):
        submit(ctx, test, flags, index)
        check_result(test)
        if test.expected == 0:
            harvest(ctx, 1)


def run_queued(ctx, tests: List[TestCase], flags: int) -> None:
    if any(test.expected for test in tests):
        raise ValueError("queued mode only accepts successful cases")
    for index, test in enumerate(tests):
        submit(ctx, test, flags, index)
        check_result(test)
    harvest(ctx, len(tests))


def run_threaded(tests: List[TestCase], flags: int) -> None:
    if any(test.expected for test in tests):
        raise ValueError("threaded mode only accepts successful cases")
    barrier = threading.Barrier(len(tests))

    def worker(test: TestCase) -> None:
        barrier.wait()
        ctx = new_ctx(4)
        try:
            submit(ctx, test, flags)
            if test.result == 0:
                harvest(ctx, 1)
        finally:
            destroy_ctx(ctx)

    threads = [threading.Thread(target=worker, args=(test,)) for test in tests]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for test in tests:
        check_result(test)


def registered_range(tests: List[TestCase]) -> Tuple[int, int]:
    if any(test.expected for test in tests):
        raise ValueError("registered-memory mode only accepts successful cases")
    start = min(test.cmb_va for test in tests)
    end = max(test.cmb_va + test.length for test in tests)
    if start < 0 or end >= 1 << 64:
        raise ValueError("registered-memory range exceeds the UAPI")
    return start, end - start


class VaAllocator:
    def __init__(self, size: int, granularity: int) -> None:
        if size <= 0 or granularity <= 0 or size % granularity:
            raise ValueError("invalid allocator size or granularity")
        self.granularity = granularity
        self.slots = size // granularity
        self.bitmap = bytearray(self.slots)
        self.cursor = 0
        self.lock = threading.Lock()

    def allocate(self, length: int) -> int:
        needed = (length + self.granularity - 1) // self.granularity
        with self.lock:
            for scanned in range(self.slots):
                start = (self.cursor + scanned) % self.slots
                if start + needed > self.slots:
                    continue
                if any(self.bitmap[start : start + needed]):
                    continue
                self.bitmap[start : start + needed] = b"\x01" * needed
                self.cursor = (start + needed) % self.slots
                return start * self.granularity
        raise RuntimeError(f"cannot allocate {length} bytes of CMB VA")

    def release(self, va: int, length: int) -> None:
        start = va // self.granularity
        needed = (length + self.granularity - 1) // self.granularity
        with self.lock:
            if va % self.granularity or not all(
                self.bitmap[start : start + needed]
            ):
                raise RuntimeError(f"invalid CMB VA release at {va}")
            self.bitmap[start : start + needed] = b"\x00" * needed


def validate_stress_allocations(
    cases: Sequence[StressCase],
    granularity: int,
    backing_page_size: int,
) -> None:
    reservations = []
    page_owners: Dict[int, Set[int]] = {}
    for test in cases:
        if test.cmb_va is None:
            raise RuntimeError(f"{test.case_id}: missing CMB VA")
        reserved = (test.length + granularity - 1) // granularity * granularity
        reservations.append((test.cmb_va, test.cmb_va + reserved, test.case_id))
        first_page = test.cmb_va // backing_page_size
        last_page = (test.cmb_va + test.length - 1) // backing_page_size
        for page in range(first_page, last_page + 1):
            page_owners.setdefault(page, set()).add(test.worker)

    reservations.sort()
    for previous, current in zip(reservations, reservations[1:]):
        if previous[1] > current[0]:
            raise RuntimeError(
                f"reservations overlap: {previous[2]} and {current[2]}"
            )
    if not any(len(owners) >= 2 for owners in page_owners.values()):
        raise RuntimeError(
            f"iteration {cases[0].iteration} does not share a backing page"
        )


def stress_matrix(
    tests: List[StressCase], workers: int, iterations: int
) -> List[List[StressCase]]:
    matrix: List[List[Optional[StressCase]]] = [
        [None for _ in range(workers)] for _ in range(iterations)
    ]
    if len(tests) != workers * iterations:
        raise ValueError(
            f"expected {workers * iterations} stress cases, got {len(tests)}"
        )
    for test in tests:
        if not 0 <= test.iteration < iterations or not 0 <= test.worker < workers:
            raise ValueError(f"{test.case_id}: invalid iteration or worker")
        if matrix[test.iteration][test.worker] is not None:
            raise ValueError(
                f"duplicate iteration={test.iteration} worker={test.worker}"
            )
        matrix[test.iteration][test.worker] = test
    if any(test is None for row in matrix for test in row):
        raise ValueError("stress manifest is incomplete")
    return [[test for test in row if test is not None] for row in matrix]


def write_stress_results(path: Path, tests: List[StressCase]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        for test in sorted(tests, key=lambda item: (item.iteration, item.worker)):
            if test.cmb_va is None or test.result != 0:
                raise RuntimeError(f"{test.case_id}: incomplete stress result")
            writer.writerow(
                (
                    test.case_id,
                    test.path,
                    test.file_offset,
                    test.cmb_va,
                    test.length,
                    0,
                )
            )


def run_stress(
    tests: List[StressCase],
    workers: int,
    iterations: int,
    cmb_size: int,
    granularity: int,
    backing_page_size: int,
    result_manifest: Path,
    registered_mem: bool,
) -> None:
    matrix = stress_matrix(tests, workers, iterations)
    allocator = VaAllocator(cmb_size, granularity)
    phase = threading.Barrier(workers)
    ctxs: List[Optional[object]] = [None] * workers
    errors: List[BaseException] = []
    error_lock = threading.Lock()
    flags = nds.NDS_IO_F_REGISTERED_MEM if registered_mem else 0

    def failed() -> bool:
        with error_lock:
            return bool(errors)

    def fail(error: BaseException) -> None:
        with error_lock:
            if not errors:
                errors.append(error)

    def wait_phase() -> bool:
        try:
            phase.wait()
            return True
        except threading.BrokenBarrierError:
            return False

    def worker(worker_id: int) -> None:
        ctx = None
        try:
            try:
                ctx = new_ctx(4)
                ctxs[worker_id] = ctx
            except BaseException as error:
                fail(error)
            if not wait_phase():
                return
            if worker_id == 0 and not failed():
                open_ctxs = [item for item in ctxs if item is not None]
                if len(open_ctxs) != workers or len(set(map(id, open_ctxs))) != workers:
                    fail(RuntimeError("stress workers do not have distinct ctxs"))
            if not wait_phase():
                return

            for iteration in range(iterations):
                test = matrix[iteration][worker_id]
                va: Optional[int] = None
                if not failed():
                    try:
                        va = allocator.allocate(test.length)
                        test.cmb_va = va
                    except BaseException as error:
                        fail(error)
                if not wait_phase():
                    return
                if worker_id == 0 and not failed():
                    try:
                        validate_stress_allocations(
                            matrix[iteration], granularity, backing_page_size
                        )
                    except BaseException as error:
                        fail(error)
                if not wait_phase():
                    return

                if not failed() and ctx is not None and va is not None:
                    try:
                        if test.fd is None:
                            raise RuntimeError(f"{test.case_id}: fd not opened")
                        ret = nds.io_submit(
                            ctx,
                            [
                                (
                                    nds.NDS_IO_OP_PREAD,
                                    test.fd,
                                    test.file_offset,
                                    [(va, test.length)],
                                    flags,
                                    0,
                                    0,
                                )
                            ],
                        )
                        test.result = 0 if ret == 1 else ret
                        print(
                            f"api=nds-python case={test.case_id} va=0x{va:x} "
                            f"ret={test.result} expected=0",
                            flush=True,
                        )
                        if test.result:
                            raise RuntimeError(
                                f"{test.case_id}: io_submit returned {test.result}"
                            )
                        harvest(ctx, 1)
                    except BaseException as error:
                        fail(error)
                if not wait_phase():
                    return

                if va is not None:
                    try:
                        allocator.release(va, test.length)
                    except BaseException as error:
                        fail(error)
                if not wait_phase():
                    return
        except BaseException as error:
            fail(error)
            phase.abort()
        finally:
            if ctx is not None:
                try:
                    destroy_ctx(ctx)
                except BaseException as error:
                    fail(error)

    threads = [
        threading.Thread(target=worker, args=(worker_id,))
        for worker_id in range(workers)
    ]
    started: List[threading.Thread] = []
    try:
        for thread in threads:
            thread.start()
            started.append(thread)
    except BaseException:
        phase.abort()
        for thread in started:
            thread.join()
        raise
    for thread in started:
        thread.join()
    if errors:
        raise RuntimeError(f"stress worker failed: {errors[0]}")
    write_stress_results(result_manifest, tests)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology", required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument(
        "--mode",
        required=True,
        choices=("single", "queued", "threaded", "stress", "reject"),
    )
    parser.add_argument("--workers", type=integer)
    parser.add_argument("--iterations", type=integer)
    parser.add_argument("--cmb-size", type=integer)
    parser.add_argument("--va-granularity", type=integer)
    parser.add_argument("--backing-page-size", type=integer)
    parser.add_argument("--result-manifest", type=Path)
    parser.add_argument("--registered-mem", action="store_true")
    args = parser.parse_args()

    stress_values = (
        args.workers,
        args.iterations,
        args.cmb_size,
        args.va_granularity,
        args.backing_page_size,
        args.result_manifest,
    )

    if args.mode == "reject":
        if args.manifest is not None:
            parser.error("reject mode does not take --manifest")
        if any(value is not None for value in stress_values):
            parser.error("stress-specific options require --mode stress")
        if args.registered_mem:
            parser.error("reject mode does not take --registered-mem")
        topo_fd = os.open(args.topology, os.O_RDONLY | os.O_DIRECT)
        try:
            require("reject-init-empty-fds", nds.init([]), -errno.EINVAL)
            require(
                "reject-init-flags",
                nds.init([topo_fd], 1),
                -errno.EINVAL,
            )
            ret = nds.init([topo_fd])
            if ret:
                raise RuntimeError(f"nds.init returned {ret}")
            try:
                run_reject(args.topology)
            finally:
                exit_rc = nds.exit()
                if exit_rc:
                    raise RuntimeError(f"nds.exit returned {exit_rc}")
        finally:
            os.close(topo_fd)
        return 0

    if args.manifest is None:
        parser.error("--manifest is required unless --mode reject")

    if args.mode == "stress":
        if any(value is None for value in stress_values):
            parser.error("stress mode requires all stress-specific options")
        tests = load_stress_manifest(args.manifest)
        topo_fd = os.open(args.topology, os.O_RDONLY | os.O_DIRECT)
        try:
            ret = nds.init([topo_fd])
            if ret:
                raise RuntimeError(f"nds.init returned {ret}")
            reg_addr: Optional[int] = None
            try:
                if args.registered_mem:
                    result = nds.register_mem(0, args.cmb_size)
                    if result:
                        raise RuntimeError(f"register_mem returned {result}")
                    reg_addr = 0
                open_case_fds(tests)
                run_stress(
                    tests,
                    args.workers,
                    args.iterations,
                    args.cmb_size,
                    args.va_granularity,
                    args.backing_page_size,
                    args.result_manifest,
                    args.registered_mem,
                )
            finally:
                cleanup_errors = []
                close_case_fds(tests)
                if reg_addr is not None:
                    result = nds.unregister_mem(reg_addr)
                    if result:
                        cleanup_errors.append(f"unregister_mem returned {result}")
                exit_rc = nds.exit()
                if exit_rc:
                    cleanup_errors.append(f"nds.exit returned {exit_rc}")
                if cleanup_errors:
                    error = RuntimeError(", ".join(cleanup_errors))
                    if sys.exc_info()[0] is None:
                        raise error
                    print(
                        f"python_nds_api_test: cleanup failed: {error}",
                        file=sys.stderr,
                    )
        finally:
            os.close(topo_fd)
        return 0

    if any(value is not None for value in stress_values):
        parser.error("stress-specific options require --mode stress")

    tests = load_manifest(args.manifest)
    topo_fd = os.open(args.topology, os.O_RDONLY | os.O_DIRECT)
    try:
        ret = nds.init([topo_fd])
        if ret:
            raise RuntimeError(f"nds.init returned {ret}")
        ctx = None
        reg_addr: Optional[int] = None
        try:
            if args.registered_mem:
                addr, size = registered_range(tests)
                result = nds.register_mem(addr, size)
                if result:
                    raise RuntimeError(f"register_mem returned {result}")
                reg_addr = addr
            flags = nds.NDS_IO_F_REGISTERED_MEM if args.registered_mem else 0
            open_case_fds(tests)
            if args.mode == "threaded":
                run_threaded(tests, flags)
            else:
                ctx = new_ctx(1)
                if args.mode == "single":
                    run_single(ctx, tests, flags)
                else:
                    run_queued(ctx, tests, flags)
        finally:
            cleanup_errors = []
            close_case_fds(tests)
            if ctx is not None:
                try:
                    destroy_ctx(ctx)
                except BaseException as error:
                    cleanup_errors.append(str(error))
            if reg_addr is not None:
                result = nds.unregister_mem(reg_addr)
                if result:
                    cleanup_errors.append(f"unregister_mem returned {result}")
            exit_rc = nds.exit()
            if exit_rc:
                cleanup_errors.append(f"nds.exit returned {exit_rc}")
            if cleanup_errors:
                error = RuntimeError(", ".join(cleanup_errors))
                if sys.exc_info()[0] is None:
                    raise error
                print(
                    f"python_nds_api_test: cleanup failed: {error}",
                    file=sys.stderr,
                )
    finally:
        os.close(topo_fd)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"python_nds_api_test: {error}", file=sys.stderr)
        sys.exit(1)
