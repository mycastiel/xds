# p2p_dev: nvme_setup_cmd Tracepoint Lookup

## Summary

Remove the requirement that userspace passes `tp_nvme_setup_cmd_addr` to
`p2p_dev.ko`. `p2p_dev.c` should locate and register the
`nvme_setup_cmd` tracepoint from inside the kernel.

## Current Gap

`p2p_dev.c` currently treats `tp_nvme_setup_cmd_addr` as a module parameter and
casts it to `struct tracepoint *`. `go.sh` obtains the address from
`/proc/kallsyms`. This is fragile and fails when symbols are hidden, when the
address changes, or when NVMe core is loaded after `p2p_dev.ko`.

## Proposal

- Keep using the `nvme_setup_cmd` tracepoint instead of directly calling or
  hooking the `nvme_setup_cmd()` function.
- On `p2p_dev.ko` load, scan built-in tracepoints with
  `for_each_kernel_tracepoint()` and register the tracepoint whose name is
  `nvme_setup_cmd`.
- Also register a tracepoint module notifier so the same tracepoint can be
  found if NVMe host core is built as a module and is loaded after `p2p_dev.ko`.
- In the module notifier, scan the loaded module tracepoint list and register
  only the tracepoint named `nvme_setup_cmd`.
- If the NVMe module unloads or the tracepoint disappears, unregister the probe
  and mark the hook unavailable.
- Fail P2P read ioctls with `-ENODEV` when the hook is unavailable, rather than
  issuing NVMe requests without the SGL metadata flag update.

## Rationale

Directly depending on the function symbol is not stable across kernel versions.
The local kernel trees have different `nvme_setup_cmd()` signatures, while the
tracepoint keeps the needed request and command arguments. The tracepoint
module notifier covers the case where the NVMe host core is a module.

## Acceptance Criteria

- `p2p_dev.ko` no longer requires `tp_nvme_setup_cmd_addr`.
- Built-in NVMe and modular NVMe both work.
- Loading `p2p_dev.ko` before NVMe core does not require reloading `p2p_dev.ko`.
- Read ioctls fail clearly with `-ENODEV` if `nvme_setup_cmd` is unavailable.
- `go.sh` no longer parses `/proc/kallsyms` for this address.
