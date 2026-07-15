#!/usr/bin/env python3

import argparse
import csv
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

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


def load_manifest(path: Path) -> List[TestCase]:
    tests: List[TestCase] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for line_number, row in enumerate(csv.reader(stream, delimiter="\t"), 1):
            if not row or row[0].startswith("#"):
                continue
            if len(row) != 6:
                raise ValueError(
                    f"manifest line {line_number} must have six TSV fields"
                )
            test = TestCase(
                case_id=row[0],
                path=row[1],
                file_offset=int(row[2], 0),
                cmb_va=int(row[3], 0),
                length=int(row[4], 0),
                expected=int(row[5], 0),
            )
            if not test.case_id or not test.path or test.length <= 0:
                raise ValueError(f"invalid manifest entry at line {line_number}")
            tests.append(test)
    if not tests:
        raise ValueError("manifest contains no test cases")
    return tests


def submit(dev_fd: int, test: TestCase) -> None:
    test.result = file_p2p.read_file(
        dev_fd,
        test.path,
        test.file_offset,
        [(test.cmb_va, test.length)],
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
    result = file_p2p.drain_read(dev_fd)
    if result:
        raise RuntimeError(f"drain_read returned {result}")


def run_single(dev_fd: int, tests: List[TestCase]) -> None:
    for test in tests:
        submit(dev_fd, test)
        check_result(test)
        if test.expected == 0:
            drain(dev_fd)
    drain(dev_fd)


def run_queued(dev_fd: int, tests: List[TestCase]) -> None:
    if any(test.expected for test in tests):
        raise ValueError("queued mode only accepts successful cases")
    for test in tests:
        submit(dev_fd, test)
        check_result(test)
    drain(dev_fd)


def run_threaded(dev_fd: int, tests: List[TestCase]) -> None:
    if any(test.expected for test in tests):
        raise ValueError("threaded mode only accepts successful cases")
    barrier = threading.Barrier(len(tests))

    def worker(test: TestCase) -> None:
        barrier.wait()
        submit(dev_fd, test)

    threads = [threading.Thread(target=worker, args=(test,)) for test in tests]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for test in tests:
        check_result(test)
    drain(dev_fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology", required=True)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument(
        "--mode", required=True, choices=("single", "queued", "threaded")
    )
    args = parser.parse_args()

    tests = load_manifest(args.manifest)
    dev_fd = file_p2p.new_p2p_fd()
    if dev_fd < 0:
        raise RuntimeError(f"new_p2p_fd returned {dev_fd}")
    try:
        result = file_p2p.add_topo(dev_fd, args.topology)
        if result:
            raise RuntimeError(f"add_topo returned {result}")
        if args.mode == "single":
            run_single(dev_fd, tests)
        elif args.mode == "queued":
            run_queued(dev_fd, tests)
        else:
            run_threaded(dev_fd, tests)
    finally:
        file_p2p.close_p2p_fd(dev_fd)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"python_api_test: {error}", file=sys.stderr)
        sys.exit(1)
