#!/usr/bin/env python3

import argparse
import csv
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

import file_p2p


@dataclass
class TestCase:
    case_id: str
    path: str
    file_offset: int
    cmb_va: int
    length: int
    expected: int
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


def submit(dev_fd: int, test: TestCase, flags: int, mem_handle: int) -> None:
    test.result = file_p2p.rw_file(
        dev_fd,
        file_p2p.P2P_IO_READ,
        test.path,
        test.file_offset,
        [(test.cmb_va, test.length)],
        mem_handle,
        flags,
    )
    print(
        f"api=python case={test.case_id} ret={test.result} "
        f"expected={test.expected}",
        flush=True,
    )


def check_result(test: TestCase) -> None:
    if test.result != test.expected:
        raise RuntimeError(
            f"{test.case_id}: returned {test.result}, expected {test.expected}"
        )


def drain(dev_fd: int) -> None:
    result = file_p2p.drain_io(dev_fd)
    if result:
        raise RuntimeError(f"drain_io returned {result}")


def run_single(
    dev_fd: int, tests: List[TestCase], flags: int, mem_handle: int
) -> None:
    for test in tests:
        submit(dev_fd, test, flags, mem_handle)
        check_result(test)
        if test.expected == 0:
            drain(dev_fd)
    drain(dev_fd)


def run_queued(
    dev_fd: int, tests: List[TestCase], flags: int, mem_handle: int
) -> None:
    if any(test.expected for test in tests):
        raise ValueError("queued mode only accepts successful cases")
    for test in tests:
        submit(dev_fd, test, flags, mem_handle)
        check_result(test)
    drain(dev_fd)


def run_threaded(
    dev_fd: int, tests: List[TestCase], flags: int, mem_handle: int
) -> None:
    if any(test.expected for test in tests):
        raise ValueError("threaded mode only accepts successful cases")
    barrier = threading.Barrier(len(tests))

    def worker(test: TestCase) -> None:
        barrier.wait()
        submit(dev_fd, test, flags, mem_handle)

    threads = [threading.Thread(target=worker, args=(test,)) for test in tests]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for test in tests:
        check_result(test)
    drain(dev_fd)


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
    topology: str,
    tests: List[StressCase],
    workers: int,
    iterations: int,
    cmb_size: int,
    granularity: int,
    backing_page_size: int,
    result_manifest: Path,
    mem_handle: int,
    registration_fd: int,
) -> None:
    matrix = stress_matrix(tests, workers, iterations)
    allocator = VaAllocator(cmb_size, granularity)
    phase = threading.Barrier(workers)
    fds: List[Optional[int]] = [None] * workers
    errors: List[BaseException] = []
    error_lock = threading.Lock()
    flags = file_p2p.P2P_IO_F_REGISTERED_MEM if mem_handle else 0

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
        dev_fd: Optional[int] = None
        try:
            dev_fd = file_p2p.new_p2p_fd()
            if dev_fd < 0:
                fail(RuntimeError(f"worker {worker_id}: new_p2p_fd={dev_fd}"))
            else:
                fds[worker_id] = dev_fd
                result = file_p2p.add_topo(dev_fd, topology)
                if result:
                    fail(RuntimeError(f"worker {worker_id}: add_topo={result}"))
            if not wait_phase():
                return
            if worker_id == 0 and not failed():
                open_fds = [fd for fd in fds if fd is not None]
                if len(open_fds) != workers or len(set(open_fds)) != workers:
                    fail(RuntimeError("stress workers do not have distinct fds"))
                if registration_fd in open_fds:
                    fail(RuntimeError("registration fd is also an I/O fd"))
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

                if not failed() and dev_fd is not None and va is not None:
                    try:
                        test.result = file_p2p.rw_file(
                            dev_fd,
                            file_p2p.P2P_IO_READ,
                            test.path,
                            test.file_offset,
                            [(va, test.length)],
                            mem_handle,
                            flags,
                        )
                        print(
                            f"api=python case={test.case_id} va=0x{va:x} "
                            f"ret={test.result} expected=0",
                            flush=True,
                        )
                        if test.result:
                            raise RuntimeError(
                                f"{test.case_id}: rw_file returned {test.result}"
                            )
                        drain(dev_fd)
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
            if dev_fd is not None and dev_fd >= 0:
                file_p2p.close_p2p_fd(dev_fd)

    threads = [threading.Thread(target=worker, args=(worker_id,)) for worker_id in range(workers)]
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
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument(
        "--mode", required=True, choices=("single", "queued", "threaded", "stress")
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
    if args.mode == "stress":
        if any(value is None for value in stress_values):
            parser.error("stress mode requires all stress-specific options")
        tests = load_stress_manifest(args.manifest)
        registration_fd = -1
        mem_handle = 0
        try:
            if args.registered_mem:
                registration_fd = file_p2p.new_p2p_fd()
                if registration_fd < 0:
                    raise RuntimeError(
                        f"new registration fd returned {registration_fd}"
                    )
                handle = file_p2p.register_mem(
                    registration_fd, 0, args.cmb_size
                )
                if handle is None or handle == 0:
                    raise RuntimeError(f"register_mem returned {handle}")
                mem_handle = handle
            run_stress(
                args.topology,
                tests,
                args.workers,
                args.iterations,
                args.cmb_size,
                args.va_granularity,
                args.backing_page_size,
                args.result_manifest,
                mem_handle,
                registration_fd,
            )
        finally:
            cleanup_errors = []
            if mem_handle:
                result = file_p2p.unregister_mem(registration_fd, mem_handle)
                if result:
                    cleanup_errors.append(f"unregister_mem returned {result}")
            if registration_fd >= 0:
                result = file_p2p.close_p2p_fd(registration_fd)
                if result:
                    cleanup_errors.append(
                        f"close registration fd returned {result}"
                    )
            if cleanup_errors:
                error = RuntimeError(", ".join(cleanup_errors))
                if sys.exc_info()[0] is None:
                    raise error
                print(
                    f"python_api_test: cleanup failed: {error}",
                    file=sys.stderr,
                )
        return 0
    if any(value is not None for value in stress_values):
        parser.error("stress-specific options require --mode stress")

    tests = load_manifest(args.manifest)
    dev_fd = file_p2p.new_p2p_fd()
    if dev_fd < 0:
        raise RuntimeError(f"new_p2p_fd returned {dev_fd}")
    registration_fd = -1
    mem_handle = 0
    try:
        result = file_p2p.add_topo(dev_fd, args.topology)
        if result:
            raise RuntimeError(f"add_topo returned {result}")
        if args.registered_mem:
            registration_fd = file_p2p.new_p2p_fd()
            if registration_fd < 0:
                raise RuntimeError(
                    f"new registration fd returned {registration_fd}"
                )
            addr, size = registered_range(tests)
            handle = file_p2p.register_mem(registration_fd, addr, size)
            if handle is None or handle == 0:
                raise RuntimeError(f"register_mem returned {handle}")
            mem_handle = handle
        flags = (
            file_p2p.P2P_IO_F_REGISTERED_MEM if args.registered_mem else 0
        )
        if args.mode == "single":
            run_single(dev_fd, tests, flags, mem_handle)
        elif args.mode == "queued":
            run_queued(dev_fd, tests, flags, mem_handle)
        else:
            run_threaded(dev_fd, tests, flags, mem_handle)
    finally:
        cleanup_errors = []
        result = file_p2p.drain_io(dev_fd)
        if result:
            cleanup_errors.append(f"drain_io returned {result}")
        if mem_handle:
            result = file_p2p.unregister_mem(registration_fd, mem_handle)
            if result:
                cleanup_errors.append(f"unregister_mem returned {result}")
        if registration_fd >= 0:
            result = file_p2p.close_p2p_fd(registration_fd)
            if result:
                cleanup_errors.append(
                    f"close registration fd returned {result}"
                )
        result = file_p2p.close_p2p_fd(dev_fd)
        if result:
            cleanup_errors.append(f"close_p2p_fd returned {result}")
        if cleanup_errors:
            error = RuntimeError(", ".join(cleanup_errors))
            if sys.exc_info()[0] is None:
                raise error
            print(f"python_api_test: cleanup failed: {error}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"python_api_test: {error}", file=sys.stderr)
        sys.exit(1)
