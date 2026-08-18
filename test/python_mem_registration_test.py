#!/usr/bin/env python3

import argparse
import errno
import os
import sys

import file_p2p


REG_ADDR = 512
REG_SIZE = (4 << 20) - REG_ADDR
SECOND_REG_ADDR = (8 << 20) + 512
SECOND_REG_SIZE = (4 << 20) - 512
REGISTERED = file_p2p.P2P_IO_F_REGISTERED_MEM
UNKNOWN_FLAG = 1 << 31


def require(operation: str, actual: int, expected: int = 0) -> None:
    print(
        f"api=python case={operation} ret={actual} expected={expected}",
        flush=True,
    )
    if actual != expected:
        raise RuntimeError(f"{operation} returned {actual}, expected {expected}")


def registered_read(dev_fd: int, source: str, handle: int, iovs: object) -> int:
    return file_p2p.rw_file(
        dev_fd, file_p2p.P2P_IO_READ, source, 0, iovs, handle, REGISTERED
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology", required=True)
    args = parser.parse_args()

    dev_fd = file_p2p.new_p2p_fd()
    other_fd = file_p2p.new_p2p_fd()
    if dev_fd < 0 or other_fd < 0:
        raise RuntimeError(f"new_p2p_fd returned {dev_fd}, {other_fd}")

    handle = 0
    second_handle = 0
    try:
        require("add_topo", file_p2p.add_topo(dev_fd, args.topology))
        require("add_topo other", file_p2p.add_topo(other_fd, args.topology))

        handle = file_p2p.register_mem(dev_fd, REG_ADDR, REG_SIZE)
        if handle is None:
            raise RuntimeError(f"register_mem returned {handle}")

        if file_p2p.register_mem(dev_fd, REG_ADDR, 0) is not None:
            print(
                "api=python case=register zero size ret=0 expected=-22",
                flush=True,
            )
            raise RuntimeError("zero-size registration succeeded")
        print(
            "api=python case=register zero size ret=-22 expected=-22",
            flush=True,
        )
        require(
            "zero handle",
            registered_read(dev_fd, args.topology, 0, [(REG_ADDR, 512)]),
            -errno.EINVAL,
        )
        require(
            "unknown flags",
            file_p2p.rw_file(
                dev_fd,
                file_p2p.P2P_IO_READ,
                args.topology,
                0,
                [(REG_ADDR, 512)],
                handle,
                UNKNOWN_FLAG,
            ),
            -errno.EOPNOTSUPP,
        )
        require(
            "out-of-range IOV",
            registered_read(
                dev_fd,
                args.topology,
                handle,
                [(REG_ADDR + REG_SIZE, 512)],
            ),
            -errno.ERANGE,
        )
        require(
            "other fd",
            registered_read(other_fd, args.topology, handle, [(REG_ADDR, 512)]),
        )
        require("drain other fd", file_p2p.drain_io(other_fd))
        require(
            "unregister on other fd",
            file_p2p.unregister_mem(other_fd, handle),
            -errno.EPERM,
        )

        second_handle = file_p2p.register_mem(
            dev_fd, SECOND_REG_ADDR, SECOND_REG_SIZE
        )
        if second_handle is None or second_handle == 0 or second_handle == handle:
            raise RuntimeError(
                f"second register_mem returned {second_handle}"
            )
        require(
            "primary of multiple registered memories",
            registered_read(
                other_fd, args.topology, handle, [(REG_ADDR + 512, 512)]
            ),
        )
        require(
            "second of multiple registered memories",
            registered_read(
                other_fd,
                args.topology,
                second_handle,
                [(SECOND_REG_ADDR, 512)],
            ),
        )
        require("drain multiple registered memories", file_p2p.drain_io(other_fd))
        stale_second_handle = second_handle
        require(
            "unregister second memory",
            file_p2p.unregister_mem(dev_fd, second_handle),
        )
        second_handle = 0
        require(
            "unregistered second handle",
            registered_read(
                other_fd,
                args.topology,
                stale_second_handle,
                [(SECOND_REG_ADDR, 512)],
            ),
            -errno.ENOENT,
        )
        require(
            "primary after second unregister",
            registered_read(
                other_fd, args.topology, handle, [(REG_ADDR + 512, 512)]
            ),
        )
        require("drain surviving primary", file_p2p.drain_io(other_fd))

        child = os.fork()
        if child == 0:
            read_result = registered_read(
                dev_fd, args.topology, handle, [(REG_ADDR, 512)]
            )
            unregister_result = file_p2p.unregister_mem(dev_fd, handle)
            os._exit(
                0
                if read_result == -errno.EPERM
                and unregister_result == -errno.EPERM
                else 1
            )
        _, status = os.waitpid(child, 0)
        if not os.WIFEXITED(status) or os.WEXITSTATUS(status):
            raise RuntimeError("inherited-fd process-scope check failed")

        require(
            "multi-IOV read",
            registered_read(
                dev_fd,
                args.topology,
                handle,
                [
                    (REG_ADDR, 512),
                    ((2 << 20) - 512, 1024),
                    ((2 << 20) + 512, 1536),
                ],
            ),
        )
        for index in range(16):
            require(
                f"reused registered read {index}",
                registered_read(
                    dev_fd,
                    args.topology,
                    handle,
                    [(REG_ADDR + index * 512, 512)],
                ),
            )

        require("drain", file_p2p.drain_io(dev_fd))
        require("unregister", file_p2p.unregister_mem(dev_fd, handle))

        new_handle = file_p2p.register_mem(dev_fd, REG_ADDR, REG_SIZE)
        if new_handle is None:
            raise RuntimeError(f"second register_mem returned {new_handle}")
        if new_handle == handle:
            raise RuntimeError("registered memory handle was reused")
        require(
            "stale handle",
            registered_read(dev_fd, args.topology, handle, [(REG_ADDR, 512)]),
            -errno.ENOENT,
        )
        require("unregister replacement", file_p2p.unregister_mem(dev_fd, new_handle))
        handle = 0
    finally:
        file_p2p.drain_io(other_fd)
        file_p2p.drain_io(dev_fd)
        if second_handle:
            file_p2p.unregister_mem(dev_fd, second_handle)
        if handle:
            file_p2p.unregister_mem(dev_fd, handle)
        file_p2p.close_p2p_fd(other_fd)
        file_p2p.close_p2p_fd(dev_fd)

    print("Python memory registration tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
