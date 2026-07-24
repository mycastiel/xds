#!/usr/bin/env python3

import argparse
import errno
import sys
from pathlib import Path

import file_p2p


def require(name: str, actual: int, expected: int) -> None:
    if actual != expected:
        raise RuntimeError(f"{name} returned {actual}, expected {expected}")
    print(
        f"api=python case={name} ret={actual} expected={expected}",
        flush=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--regular-file", required=True, type=Path)
    args = parser.parse_args()
    args.regular_file.write_bytes(b"\0" * 4096)
    iovs = [(0, 512)]

    require(
        "unknown operation",
        file_p2p.rw_file(-1, file_p2p.P2P_IO_WRITE + 1,
                         str(args.regular_file), 0, iovs, 0, 0),
        -errno.EINVAL,
    )
    require(
        "regular file write",
        file_p2p.rw_file(-1, file_p2p.P2P_IO_WRITE,
                         str(args.regular_file), 0, iovs, 0, 0),
        -errno.EOPNOTSUPP,
    )
    require(
        "negative host pid",
        file_p2p.rw_file(-1, file_p2p.P2P_IO_READ,
                         str(args.regular_file), 0, iovs, 0, 0, -1),
        -errno.EINVAL,
    )
    require(
        "registered host pid",
        file_p2p.rw_file(-1, file_p2p.P2P_IO_READ,
                         str(args.regular_file), 0, iovs, 1,
                         file_p2p.P2P_IO_F_REGISTERED_MEM, 1),
        -errno.EINVAL,
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"python_rw_rejection_test: {error}", file=sys.stderr)
        sys.exit(1)
