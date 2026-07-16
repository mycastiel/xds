#!/usr/bin/env python3

import argparse
import collections
import csv
import os
import re
import stat
import subprocess
import sys
from pathlib import Path
from typing import Counter as CounterType, Dict, List, Tuple


KERNEL_CRC = re.compile(
    r"p2p: crc32 file=(\S+) length=0x([0-9a-fA-F]+) "
    r"crc32=0x([0-9a-fA-F]{8})"
)
USER_CRC = re.compile(r"\bcrc32 0x([0-9a-fA-F]{8})\b")


def positive_cases(path: Path) -> List[Tuple[str, Path, int, int]]:
    cases: List[Tuple[str, Path, int, int]] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for line_number, row in enumerate(csv.reader(stream, delimiter="\t"), 1):
            if not row or row[0].startswith("#"):
                continue
            if len(row) != 6:
                raise ValueError(
                    f"manifest line {line_number} must have six TSV fields"
                )
            if int(row[5], 0) == 0:
                cases.append((row[0], Path(row[1]), int(row[2], 0), int(row[4], 0)))
    return cases


def kernel_results(path: Path) -> Dict[Tuple[str, int], CounterType[int]]:
    results: Dict[Tuple[str, int], CounterType[int]] = collections.defaultdict(
        collections.Counter
    )
    for match in KERNEL_CRC.finditer(path.read_text(encoding="utf-8")):
        key = (Path(match.group(1)).name, int(match.group(2), 16))
        results[key][int(match.group(3), 16)] += 1
    return results


def kernel_file_name(path: Path) -> str:
    metadata = path.stat()
    if not stat.S_ISBLK(metadata.st_mode):
        return path.name

    sysfs_path = Path(
        f"/sys/dev/block/{os.major(metadata.st_rdev)}:{os.minor(metadata.st_rdev)}"
    )
    return sysfs_path.resolve(strict=True).name


def userspace_crc(tool: Path, path: Path, offset: int, length: int) -> int:
    output = subprocess.check_output(
        [str(tool), "-f", str(path), "-o", str(offset), "-l", str(length)],
        text=True,
    )
    match = USER_CRC.search(output)
    if not match:
        raise ValueError(f"cannot parse userspace CRC output: {output.strip()}")
    return int(match.group(1), 16)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--crc-tool", required=True, type=Path)
    args = parser.parse_args()

    cases = positive_cases(args.manifest)
    actual = kernel_results(args.log)
    actual_count = sum(sum(values.values()) for values in actual.values())
    if actual_count != len(cases):
        raise ValueError(
            f"kernel produced {actual_count} "
            f"CRC lines for {len(cases)} successful cases"
        )

    expected: Dict[Tuple[str, int], CounterType[int]] = collections.defaultdict(
        collections.Counter
    )
    case_results: List[Tuple[str, int]] = []
    for case_id, source, offset, length in cases:
        file_name = kernel_file_name(source)
        key = (file_name, length)
        user_crc = userspace_crc(args.crc_tool, source, offset, length)
        expected[key][user_crc] += 1
        case_results.append((case_id, user_crc))

    if set(actual) != set(expected):
        missing = set(expected) - set(actual)
        extra = set(actual) - set(expected)
        raise ValueError(f"kernel CRC keys differ: missing={missing} extra={extra}")
    for key, expected_values in expected.items():
        if actual[key] != expected_values:
            raise ValueError(
                f"file={key[0]} length=0x{key[1]:x}: kernel CRC multiset "
                f"{actual[key]} != userspace CRC multiset {expected_values}"
            )

    for case_id, user_crc in case_results:
        print(f"crc-pass case={case_id} crc32=0x{user_crc:08x}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"check_crc: {error}", file=sys.stderr)
        sys.exit(1)
