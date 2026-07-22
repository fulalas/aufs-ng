# `aufs-ng` test suite

Behavioral tests for the union filesystem, run inside a **User-Mode
Linux** guest so they exercise the real kernel code with no root
privileges, no VM images, and no risk to the running system. The
guest's root is the host filesystem (read-only in practice, via
hostfs); every mutation happens on tmpfs mounts the suite creates
itself.

## Running

There is **one command** — `tests/run-tests.sh`:

```sh
tests/run-tests.sh                      # kernel tree autodetected as linux-*/
KERNEL_SRC=/path/to/linux tests/run-tests.sh
```

It wires `fs/aufs-ng` into the kernel tree (the same two-line
Kconfig/Makefile integration the top-level README documents), builds
it as `ARCH=um` with `CONFIG_AUFSNG_FS=y` plus **lockdep
(`PROVE_LOCKING`, which implies `PROVE_RCU`) and
`DEBUG_ATOMIC_SLEEP`**, boots the resulting kernel, runs the checks,
and fails unless every check passes *and* the kernel log is free of
lockdep/RCU/BUG findings. Each check streams to your terminal as it
runs:

```
1/55 - mount... PASSED
2/55 - merge: l1 wins dir/file... PASSED
...
RESULT: PASS=55 FAIL=0
run-tests: OK - all checks passed, kernel log clean
```

The kernel tree is looked for as `linux-*/` first in `tests/`, then in
the aufs-ng folder above it. If neither has one, the latest stable
kernel is downloaded from kernel.org (via `curl` or `wget`) into the
aufs-ng folder — so the very first run on a fresh checkout is
self-contained, at the cost of a one-time download plus full build.

The first build takes a few minutes; reruns are incremental. Knobs:
`KERNEL_SRC` (skip the search and use this tree), `JOBS` (default
`nproc`), `TIMEOUT` (guest limit, default 300 s). On failure the full
guest log is kept at `tests-guest.log` in the repo root.

## How it works

`run-tests.sh` is a single self-dispatching script that plays two
roles in two different worlds:

- the **host side** builds and boots the UML kernel, then judges the
  output — this is what runs when you invoke the script;
- the **guest side** is the actual checks, run as PID 1 *inside* the
  throwaway kernel. It mounts and deletes freely and powers the guest
  off when done, so it must never run on a real system.

The host boots the kernel with this same script as init and a sentinel
(`AUFSNG_TEST_GUEST=1`) in the environment; the dispatch at the bottom
of the script picks the guest side only when that sentinel is set, so
a normal invocation always takes the safe host path. The mount helper
the guest needs (the host's `mount(8)` is unusable through hostfs for
an unprivileged user) is embedded in the script as a C heredoc,
compiled at build time — which is why `tests/` is just this one script
plus this README.

## What is covered

The suite currently runs **55 checks**:

- mount, merge order, whiteout semantics (delete/resurrect, readdir
  hiding, opaque directories)
- runtime branch add/remove: last-added-wins for **already-cached**
  positive and negative dentries, lookup/readdir agreement, cached
  directories hidden by a whiteout or shadowed by a file in the added
  branch (and restored on removal)
- `udba=reval` out-of-band detection: chmod through the branch
  enforced against a non-root reader; `udba=` changes applied on
  remount
- failure-path atomicity under ENOSPC (inode exhaustion): mkdir over
  a whiteout fails cleanly and is retryable; rename onto a lower-only
  victim rolls back
- copy-up fidelity: a multi-block file's data copied byte-for-byte,
  with mode and owner preserved and `st_ino` stable across the copy-up
- copy-up of lower hardlink siblings; per-name revalidation of
  hardlink siblings when a branch add whiteouts only one of them;
  rmdir when the upper directory vanished out-of-band
- reading a lower symlink through the union (`get_link`) and copying a
  symlink up (`set_attr_from` skips `ATTR_MODE` for symlinks); merged
  directory link count across branches (`getattr`)
- an add/del stress loop with a pinned cwd and concurrent
  stat/readdir//proc/mounts readers, as a race regression check
  (meaningful mainly because lockdep and PROVE_RCU are watching)

## Adding a test

Append a block to the **guest section** of `run-tests.sh`
(`guest_main()`) using the `ok`/`bad`/`check`/`checkfail` helpers, on
tmpfs branches you create (`$M tmpfs tmpfs <dir>`). Keep each check's
description unique — a failure's `TEST-FAIL:` marker is grepped
verbatim by the host judge. Then bump **`TOTAL`** in `guest_main()`
(and the check count at the top of "What is covered"); the suite
asserts `TOTAL` matches the number of checks actually run, so a stale
count fails the run loudly rather than silently.
