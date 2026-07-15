#!/usr/bin/env python3

import argparse
import os
import stat
import struct
from pathlib import Path


PATTERN_SIZE = 64 * 1024


def make_pattern(seed: int) -> bytearray:
    return bytearray(
        ((index * 131 + (index >> 8) * 17 + seed * 29) & 0xFF)
        for index in range(PATTERN_SIZE)
    )


def write_all(fd: int, data: memoryview) -> None:
    while data:
        written = os.write(fd, data)
        if written <= 0:
            raise OSError("short write while generating test pattern")
        data = data[written:]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--size", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--seed", required=True, type=lambda value: int(value, 0))
    args = parser.parse_args()
    if args.size <= 0:
        parser.error("--size must be positive")

    flags = os.O_WRONLY | os.O_CLOEXEC
    try:
        mode = args.output.stat().st_mode
    except FileNotFoundError:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        mode = stat.S_IFREG
    if stat.S_ISREG(mode):
        flags |= os.O_CREAT | os.O_TRUNC
    elif not stat.S_ISBLK(mode):
        raise ValueError(f"{args.output} is not a regular file or block device")

    pattern = make_pattern(args.seed)
    remaining = args.size
    chunk_index = 0
    fd = os.open(args.output, flags, 0o600)
    try:
        while remaining:
            chunk = min(remaining, len(pattern))
            struct.pack_into(
                "<QQ",
                pattern,
                0,
                args.seed & ((1 << 64) - 1),
                chunk_index,
            )
            write_all(fd, memoryview(pattern)[:chunk])
            remaining -= chunk
            chunk_index += 1
        os.fsync(fd)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
