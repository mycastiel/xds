# p2p_dev: Use Block Queue max_sectors_kb

## Summary

Replace the hard-coded 128 KiB read split limit with the limit from the selected
block device queue.

## Current Gap

`p2p_dev.c` defines `HW_LIMIT_SIZE` as 128 KiB and `calc_read_size()` uses that
constant for every device. This ignores the per-device
`queue/max_sectors_kb` setting.

## Proposal

- After resolving and validating the bdev, read the request queue with
  `bdev_get_queue(bdev)`.
- Use `queue_max_sectors(q)` as the soft request-size limit. This corresponds
  to `/sys/block/<disk>/queue/max_sectors_kb`.
- Store the resulting sector limit in `struct p2p_io_context`.
- Replace the hard-coded `(128 << 10) >> SECTOR_SHIFT` limit in
  `calc_read_size()` with the context limit.
- Keep the split bounded by:
  - the queue max sectors,
  - the remaining sectors in the current physical-address page,
  - the remaining extent length,
  - the NVMe command length field limit.
- Fail with `-EINVAL` if the computed queue limit is zero.

## Rationale

The block layer already exposes the usable per-request soft limit. Using it
keeps P2P request splitting aligned with the selected device instead of assuming
all devices should use 128 KiB.

## Acceptance Criteria

- The hard-coded 128 KiB limit is removed from read splitting.
- Different block devices can produce different P2P read split sizes.
- No request exceeds the selected queue's `max_sectors_kb`.
- Existing physical-address page boundary handling is preserved.
