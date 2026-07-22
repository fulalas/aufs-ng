# `aufs-ng` test suite

Behavioral tests for the union filesystem, run inside a **User-Mode
Linux** guest so they exercise the real kernel code with no root
privileges, no VM images, and no risk to the running system. Every
mutation happens on tmpfs mounts the suite creates itself.

## Running

```sh
run-tests.sh
```

No root is required.

The kernel source tree is looked for as `linux-*/` first in `tests/`,
then in the `aufs-ng/` folder above it. If neither has one, the latest
stable kernel is downloaded from kernel.org into the aufs-ng folder,
so the very first run on a fresh checkout is self-contained.

It's also possible to specify a kernel source path:

```sh
KERNEL_SRC=/path/to/linux run-tests.sh
```

## How it works

`run-tests.sh` is a single self-dispatching script that plays two
roles in two different worlds:

- the **host side** builds and boots the UML kernel, then judges the
  output — this is what runs when you invoke the script;
- the **guest side** is the actual checks, run as PID 1 *inside* the
  throwaway kernel. It mounts and deletes freely and powers the guest
  off when done, so it must never run on a real system.

It wires `fs/aufs-ng` into the kernel tree, builds it as `ARCH=um`
with `CONFIG_AUFSNG_FS=y`, boots the resulting kernel and runs the
checks. Each check streams to your terminal as it runs.

The host boots the kernel with this same script as init and a sentinel
(`AUFSNG_TEST_GUEST=1`) in the environment; the dispatch at the bottom
of the script picks the guest side only when that sentinel is set, so
a normal invocation always takes the safe host path.

The first build takes a few minutes; reruns are incremental. On failure
the full guest log is kept at `tests-guest.log` in the repo root.

## Adding a test

Append a block to the **guest section** of `run-tests.sh`
(`guest_main()`) using the `ok`/`bad`/`check`/`checkfail` helpers, on
tmpfs branches you create (`$M tmpfs tmpfs <dir>`). Keep each check's
description unique — a failure's `TEST-FAIL:` marker is grepped
verbatim by the host judge. Then bump **`TOTAL`** in `guest_main()`;
the suite asserts `TOTAL` matches the number of checks actually run,
so a stale count fails the run loudly rather than silently.
