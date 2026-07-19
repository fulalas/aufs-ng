# aufs-ng

`aufs-ng` is a standalone, from-scratch union filesystem, written to be a
drop-in kernel-side replacement for AUFS. It builds against a **stock,
unpatched kernel tree** — no AUFS patch set, no OverlayFS patch, nothing
outside this directory.

It registers with the kernel as filesystem type **`aufs`** (not `aufs-ng`)
and speaks AUFS's own mount option grammar and on-disk whiteout format, so
that any script issuing original AUFS `mount`/`remount` commands works
completely unmodified. `aufs-ng` is the project/module name; `aufs`
remains the wire-compatible identity the kernel and userspace see.

It also carries over one of AUFS's defining abilities: branches can be
added to or removed from the union while it's mounted, with no unmount
or reboot required. This is what lets distros such as PorteuX load and
unload `.xzm` modules on an already-running system.

## Why this exists

[Original AUFS](https://github.com/sfjro/aufs-standalone) is ~28,000 lines across `fs/aufs/` plus patches touching ~25
core kernel files to export internal symbols, and needs its own branch per
kernel minor version. `aufs-ng` targets the same on-disk format and mount
grammar with:

- **No kernel patches** — every symbol it uses is a standard, currently
  exported kernel API.
- **A much smaller surface** — ~3,500 lines, vs. AUFS's ~28,000.
- **Modern I/O passthrough** — reads/writes/splice/mmap go through the
  kernel's `backing_file_*` API (the same infrastructure FUSE passthrough
  and OverlayFS use) instead of a per-superblock rwsem on the hot path.

## Trade-offs

This is new code, not a driver hardened by two decades of real-world use.
Also, some AUFS features are intentionally out of scope:

- **Pseudo-links (`plink`)** — linking a file that comes from a module
  still works, just makes a full copy behind the scenes instead of a
  true link.
- **Directory-rename metadata (`dirren`)** — renaming a folder that comes
  from a module still works, just slower (a copy instead of an instant
  rename) for large folders.
- **FHSM** (automatic storage tiering) — not needed: aufs-ng only ever
  has one writable location, so there's nothing to move files between.
- **`shwh`** (flattening layers into one) — no built-in way to merge a
  stack of read-only layers into one clean image.
- **Multiple writable branches** — only one writable location is used at
  a time; all your changes go there, you can't spread them across
  several disks.
- **NFS export** — the union filesystem isn't meant to be shared out to
  other computers over the network; it's for local use only.
- **RDU** (readdir performance helper) — listing very large folders
  always happens the standard way, with no optional speed-up helper;
  only noticeable for unusually large folders.

## Mount syntax

Identical to original AUFS — `br:`, `add=N:PATH=MODE`, `del=PATH`,
`dirperm1`, `udba=`, `xino=`, `nowarn_perm`:

```
mount -t aufs -o br:/path/to/rw=rw[:/path/to/lower=ro[:...]] aufs /mnt
mount -o remount,dirperm1,add=1:/path/to/new-lower=rr aufs /mnt
mount -t aufs -o remount,del=/path/to/lower aufs /mnt
```

`udba=` accepts `none`, `reval` (the default), and `notify` (accepted
for AUFS compatibility but currently behaves as `reval`).
`xino=` is accepted and stored for `show_options` fidelity but has no
behavioral effect here.
`dirperm1` and `nowarn_perm` are accepted for AUFS compatibility but
are no-ops here.

All other mount parameters behave as they would under original AUFS.

## On-disk format

Identical to original AUFS: a deleted name still provided by a lower
branch is masked by a sibling regular file `.wh.<name>` (mode `0444`,
not a character device); a directory that fully shadows lower content
carries a `.wh..wh..opq` marker. Verified byte-for-byte against the
upstream `aufs-standalone` source, so `save-changes`, `dump-session`,
and anything else that scans a branch directly need no changes.

## Architecture (one file per concern)

| File | Responsibility |
|---|---|
| `super.c` | Module init, `file_system_type`, superblock/inode lifecycle, `statfs`, `show_options` |
| `params.c` | Mount option parsing (AUFS grammar) |
| `namei.c` | Layered lookup, whiteout/opaque detection, inode hashing |
| `dcache.c` | Dentry revalidation against the branch-stack generation counter, plus negative-dentry revalidation under `udba=reval` |
| `file.c` | Regular-file I/O via the kernel's `backing_file_*` passthrough API |
| `readdir.c` | Merged directory listing (rbtree + list cache), invalidated by an inode version counter |
| `inode.c` | `getattr`/`setattr`/xattr passthrough, copy-up-on-write triggers |
| `copy_up.c` | Give a lower-backed file/dir a real upper copy on first write |
| `dir.c` | `create`/`mkdir`/`mknod`/`symlink`/`link`/`unlink`/`rmdir`/`rename`, whiteout creation/removal |
| `dynlayer.c` | Runtime branch add/remove, including **surgical add**: splicing a new branch into every already-cached directory in place, so pinned directories (open fds, cwds) see new content without a `d_invalidate()` that would detach nested mounts |
| `compat.h` | `LINUX_VERSION_CODE` guards across kernel versions |

Concurrency: a `dyn_lock` rwsem excludes lookup/readdir during a
branch-stack change; a per-dentry generation counter catches stale
cached state; superseded per-directory stacks are "parked" on the inode
(with the removed branch's mount pinned) until eviction, since an
in-flight operation may still hold a pointer into the old stack.

## Building

Builds **into the kernel** (`CONFIG_AUFSNG_FS=y`), not as a loadable
module — a live-boot sequence typically needs this filesystem type
mounted before any loadable module can be reached at all.

To integrate into a kernel source tree (any anchor line in `fs/Kconfig`/
`fs/Makefile` works; the overlayfs entry is just a convenient, stable one):

```sh
git clone --depth 1 https://github.com/fulalas/aufs-ng fs/aufs-ng
sed -i '/source "fs\/overlayfs\/Kconfig"/a source "fs/aufs-ng/Kconfig"' fs/Kconfig
sed -i '/obj-\$(CONFIG_OVERLAY_FS)\s*+= overlayfs\//a obj-$(CONFIG_AUFSNG_FS)\t+= aufs-ng/' fs/Makefile
echo "CONFIG_AUFSNG_FS=y" >> .config
```

Then build the kernel as usual (`make olddefconfig && make`).

For a quick out-of-tree test build against an already-built kernel tree
(producing a loadable `.ko` instead, no `fs/Kconfig`/`fs/Makefile` edits
needed):

```sh
make -C /path/to/kernel/build M=$PWD CONFIG_AUFSNG_FS=m W=1 modules
```

## Status

Boot, module activation/deactivation, and copy-up have been verified
end-to-end, including activating a module on an already-running system.

## License

`aufs-ng` is free software, released under the **GNU General Public
License, version 2** (`GPL-2.0-only`) — the same license as the Linux
kernel it builds against, so the project is fully GPL-friendly and can
be distributed as part of a GPL kernel tree.
