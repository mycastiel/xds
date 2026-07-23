#!/usr/bin/env python3

import argparse
import sys

import file_p2p

MIXED_READ_CMB_OFFSET = 4 << 20


def make_iovs(cmb_va: int, length: int) -> list[tuple[int, int]]:
    first = 512
    second = 128 << 10
    if length < first + second + 512:
        return [(cmb_va, length)]
    return [
        (cmb_va, first),
        (cmb_va + first, second),
        (cmb_va + first + second, length - first - second),
    ]


def require(operation: str, result: int) -> None:
    if result:
        raise RuntimeError(f"{operation} returned {result}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--op", required=True, choices=("read", "write", "mixed"))
    parser.add_argument("--offset", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--source-offset", type=lambda value: int(value, 0))
    parser.add_argument("--cmb-va", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--length", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--registered-mem", action="store_true")
    args = parser.parse_args()

    if (
        args.offset < 0
        or args.cmb_va < 0
        or args.length <= 0
        or (args.source_offset is not None and args.source_offset < 0)
    ):
        parser.error("offset and CMB VA must be nonnegative; length must be positive")
    if args.op == "mixed" and args.source_offset is None:
        parser.error("--source-offset is required for mixed I/O")

    op = (
        file_p2p.P2P_IO_WRITE
        if args.op in ("write", "mixed")
        else file_p2p.P2P_IO_READ
    )
    dev_fd = file_p2p.new_p2p_fd()
    if dev_fd < 0:
        raise RuntimeError(f"new_p2p_fd returned {dev_fd}")

    handle = None
    try:
        require("add_topo", file_p2p.add_topo(dev_fd, args.topology))
        flags = 0
        if args.registered_mem:
            register_length = args.length
            if args.op == "mixed":
                register_length += MIXED_READ_CMB_OFFSET
            handle = file_p2p.register_mem(
                dev_fd, args.cmb_va, register_length
            )
            if handle is None:
                raise RuntimeError("register_mem failed")
            flags = file_p2p.P2P_IO_F_REGISTERED_MEM

        result = file_p2p.rw_file(
            dev_fd,
            op,
            args.target,
            args.offset,
            make_iovs(args.cmb_va, args.length),
            handle or 0,
            flags,
        )
        if not result and args.op == "mixed":
            result = file_p2p.rw_file(
                dev_fd,
                file_p2p.P2P_IO_READ,
                args.target,
                args.source_offset,
                make_iovs(args.cmb_va + MIXED_READ_CMB_OFFSET, args.length),
                handle or 0,
                flags,
            )
        drain_result = file_p2p.drain_io(dev_fd)
        require("rw_file", result)
        require("drain_io", drain_result)
        if handle is not None:
            require("unregister_mem", file_p2p.unregister_mem(dev_fd, handle))
            handle = None
    finally:
        if handle is not None:
            file_p2p.unregister_mem(dev_fd, handle)
        file_p2p.close_p2p_fd(dev_fd)

    print(f"api=python case={args.op} ret=0 expected=0")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"python_rw_api_test: {error}", file=sys.stderr)
        sys.exit(1)
