#!/usr/bin/env python3

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Set, Tuple


KIB = 1 << 10
MIB = 1 << 20


def integer(value: str) -> int:
    return int(value, 0)


def round_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


@dataclass(frozen=True)
class WorkloadCase:
    iteration: int
    worker: int
    case_id: str
    path: Path
    file_size: int
    offset: int
    length: int


@dataclass(frozen=True)
class ResultCase:
    case_id: str
    path: Path
    offset: int
    va: int
    length: int
    expected: int


def read_rows(path: Path, fields: int) -> List[List[str]]:
    rows: List[List[str]] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for line_number, row in enumerate(csv.reader(stream, delimiter="\t"), 1):
            if not row or row[0].startswith("#"):
                continue
            if len(row) != fields:
                raise ValueError(
                    f"{path}: line {line_number} must have {fields} TSV fields"
                )
            rows.append(row)
    return rows


def load_workload(path: Path) -> List[WorkloadCase]:
    return [
        WorkloadCase(
            iteration=int(row[0], 0),
            worker=int(row[1], 0),
            case_id=row[2],
            path=Path(row[3]),
            file_size=int(row[4], 0),
            offset=int(row[5], 0),
            length=int(row[6], 0),
        )
        for row in read_rows(path, 7)
    ]


def load_results(path: Path) -> Dict[str, ResultCase]:
    results: Dict[str, ResultCase] = {}
    for row in read_rows(path, 6):
        result = ResultCase(
            case_id=row[0],
            path=Path(row[1]),
            offset=int(row[2], 0),
            va=int(row[3], 0),
            length=int(row[4], 0),
            expected=int(row[5], 0),
        )
        if result.case_id in results:
            raise ValueError(f"duplicate result case {result.case_id}")
        results[result.case_id] = result
    return results


def validate_files(workload: List[WorkloadCase], workers: int) -> int:
    files: Dict[int, Tuple[Path, int]] = {}
    for case in workload:
        identity = (case.path, case.file_size)
        if case.worker in files and files[case.worker] != identity:
            raise ValueError(f"worker {case.worker} changes source file or size")
        files[case.worker] = identity
    if set(files) != set(range(workers)):
        raise ValueError("workload does not contain every worker")
    if len({path for path, _ in files.values()}) != workers:
        raise ValueError("workers do not use unique files")

    total = 0
    for path, expected_size in files.values():
        actual_size = path.stat().st_size
        if actual_size != expected_size:
            raise ValueError(
                f"{path}: size {actual_size} does not match {expected_size}"
            )
        if expected_size % KIB or not 4 * KIB <= expected_size <= 4 * MIB:
            raise ValueError(f"{path}: invalid file size {expected_size}")
        total += expected_size
    if not 4 * MIB < total <= 48 * MIB:
        raise ValueError(
            f"combined file size {total} is outside (4 MiB, 48 MiB]"
        )
    return total


def validate_iteration(
    cases: List[WorkloadCase],
    results: Dict[str, ResultCase],
    cmb_size: int,
    granularity: int,
    backing_page_size: int,
) -> None:
    reservations: List[Tuple[int, int, str]] = []
    page_owners: Dict[int, Set[int]] = {}
    for case in cases:
        result = results[case.case_id]
        if (
            result.path != case.path
            or result.offset != case.offset
            or result.length != case.length
            or result.expected != 0
        ):
            raise ValueError(f"{case.case_id}: result does not match workload")
        if result.va % granularity:
            raise ValueError(f"{case.case_id}: VA {result.va} is not aligned")
        reserved = round_up(case.length, granularity)
        if result.va < 0 or result.va + reserved > cmb_size:
            raise ValueError(f"{case.case_id}: reservation is outside the CMB")
        reservations.append((result.va, result.va + reserved, case.case_id))

        first_page = result.va // backing_page_size
        last_page = (result.va + case.length - 1) // backing_page_size
        for page in range(first_page, last_page + 1):
            page_owners.setdefault(page, set()).add(case.worker)

    reservations.sort()
    for previous, current in zip(reservations, reservations[1:]):
        if previous[1] > current[0]:
            raise ValueError(
                f"reservations overlap: {previous[2]} and {current[2]}"
            )
    if not any(len(owners) >= 2 for owners in page_owners.values()):
        raise ValueError(
            f"iteration {cases[0].iteration} does not share a 2 MiB page"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", required=True, type=Path)
    parser.add_argument("--result", required=True, type=Path)
    parser.add_argument("--workers", required=True, type=integer)
    parser.add_argument("--iterations", required=True, type=integer)
    parser.add_argument("--cmb-size", required=True, type=integer)
    parser.add_argument("--granularity", required=True, type=integer)
    parser.add_argument("--backing-page-size", required=True, type=integer)
    args = parser.parse_args()

    workload = load_workload(args.workload)
    results = load_results(args.result)
    expected_count = args.workers * args.iterations
    if len(workload) != expected_count or len(results) != expected_count:
        raise ValueError(
            f"expected {expected_count} operations, got workload={len(workload)} "
            f"result={len(results)}"
        )
    if len({case.case_id for case in workload}) != expected_count:
        raise ValueError("workload case IDs are not unique")
    if set(results) != {case.case_id for case in workload}:
        raise ValueError("workload and result case IDs differ")

    total_file_size = validate_files(workload, args.workers)
    by_iteration: Dict[int, List[WorkloadCase]] = {}
    for case in workload:
        if not 0 <= case.worker < args.workers:
            raise ValueError(f"{case.case_id}: invalid worker {case.worker}")
        if not 0 <= case.iteration < args.iterations:
            raise ValueError(f"{case.case_id}: invalid iteration {case.iteration}")
        if case.offset % KIB or case.length % KIB:
            raise ValueError(f"{case.case_id}: offset or length is not 1 KiB aligned")
        if not 4 * KIB <= case.length <= 4 * MIB:
            raise ValueError(f"{case.case_id}: invalid read length {case.length}")
        if case.offset < 0 or case.offset + case.length > case.file_size:
            raise ValueError(f"{case.case_id}: read is outside the source file")
        by_iteration.setdefault(case.iteration, []).append(case)

    if set(by_iteration) != set(range(args.iterations)):
        raise ValueError("workload does not contain every iteration")
    for iteration in range(args.iterations):
        cases = by_iteration[iteration]
        if len(cases) != args.workers or {case.worker for case in cases} != set(
            range(args.workers)
        ):
            raise ValueError(f"iteration {iteration} does not contain every worker")
        validate_iteration(
            cases,
            results,
            args.cmb_size,
            args.granularity,
            args.backing_page_size,
        )

    print(
        f"stress-check-pass operations={expected_count} "
        f"total_file_size={total_file_size}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"check_stress: {error}", file=sys.stderr)
        sys.exit(1)
