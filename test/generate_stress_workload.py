#!/usr/bin/env python3

import argparse
import csv
import random
import subprocess
import sys
from pathlib import Path
from typing import List, Tuple


KIB = 1 << 10
MIB = 1 << 20
MIN_FILE_SIZE = 4 * KIB
MAX_FILE_SIZE = 4 * MIB
MAX_TOTAL_FILE_SIZE = 96 * MIB
ALLOCATION_GRANULARITY = 4 * KIB
BACKING_PAGE_SIZE = 2 * MIB


def integer(value: str) -> int:
    return int(value, 0)


def round_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def choose_file_sizes(rng: random.Random, workers: int) -> List[int]:
    while True:
        sizes = [rng.randint(4, 4096) * KIB for _ in range(workers)]
        if 4 * MIB < sum(sizes) <= MAX_TOTAL_FILE_SIZE:
            return sizes


def choose_round(
    rng: random.Random, file_sizes: List[int]
) -> List[Tuple[int, int]]:
    while True:
        requests: List[Tuple[int, int]] = []
        non_page_multiple = 0
        for file_size in file_sizes:
            file_kib = file_size // KIB
            offset_kib = rng.randint(0, file_kib - 4)
            length_kib = rng.randint(4, file_kib - offset_kib)
            offset = offset_kib * KIB
            length = length_kib * KIB
            requests.append((offset, length))
            reservation = round_up(length, ALLOCATION_GRANULARITY)
            if reservation % BACKING_PAGE_SIZE:
                non_page_multiple += 1
        # With at least three such reservations, no allocation order and
        # rotating start cursor can put every internal boundary on a 2 MiB
        # boundary.  At least two adjacent files must therefore share a page.
        if non_page_multiple >= 3:
            return requests


def generate_file(path: Path, size: int, seed: int) -> None:
    subprocess.run(
        [
            sys.executable,
            str(Path(__file__).with_name("generate_pattern.py")),
            "--output",
            str(path),
            "--size",
            str(size),
            "--seed",
            str(seed),
        ],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--workers", required=True, type=integer)
    parser.add_argument("--iterations", required=True, type=integer)
    parser.add_argument("--seed", required=True, type=integer)
    parser.add_argument("--mode", required=True, choices=("raid0", "dm", "nvme"))
    parser.add_argument("--topology", required=True)
    args = parser.parse_args()

    if args.workers < 2:
        parser.error("--workers must be at least 2")
    if args.iterations <= 0:
        parser.error("--iterations must be positive")

    rng = random.Random(args.seed)
    file_sizes = choose_file_sizes(rng, args.workers)
    total_file_size = sum(file_sizes)
    args.directory.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)

    paths: List[Path] = []
    for worker, file_size in enumerate(file_sizes):
        path = args.directory / f"stress_file_{worker:02d}.dat"
        generate_file(path, file_size, args.seed + worker + 1)
        paths.append(path)

    with args.manifest.open("w", newline="", encoding="utf-8") as stream:
        stream.write(f"# seed={args.seed}\n")
        stream.write(f"# mode={args.mode}\n")
        stream.write(f"# topology={args.topology}\n")
        stream.write(f"# workers={args.workers}\n")
        stream.write(f"# iterations={args.iterations}\n")
        stream.write(f"# total_file_size={total_file_size}\n")
        stream.write(
            "# fields=iteration,worker,case_id,path,file_size,offset,length\n"
        )
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        for iteration in range(args.iterations):
            requests = choose_round(rng, file_sizes)
            for worker, (offset, length) in enumerate(requests):
                writer.writerow(
                    (
                        iteration,
                        worker,
                        f"stress-i{iteration:04d}-w{worker:02d}",
                        paths[worker],
                        file_sizes[worker],
                        offset,
                        length,
                    )
                )

    print(
        f"stress-workload seed={args.seed} mode={args.mode} "
        f"workers={args.workers} iterations={args.iterations} "
        f"total_file_size={total_file_size}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"generate_stress_workload: {error}", file=sys.stderr)
        sys.exit(1)
