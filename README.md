# `aufs-ng`

`aufs-ng` is a standalone, from-scratch union filesystem, written to be a
drop-in kernel-side replacement for `aufs` to be used mainly by live distros.
It builds against a **stock, unpatched kernel tree** — no `aufs` patch set,
no OverlayFS patch, nothing outside this directory.

It registers with the kernel as filesystem type **`aufs`** (not `aufs-ng`)
and speaks `aufs`'s own mount option grammar and on-disk whiteout format, so
that any script issuing original `aufs` `mount`/`remount` commands works
completely unmodified. `aufs-ng` is just the project name; `aufs` is
the filesystem name the kernel and userspace see.

It also carries over one of `aufs`'s defining abilities: branches can be
added to or removed from the union while it's mounted, with no unmount
or reboot required — and without disturbing open files, working
directories, or mounts nested inside the union. This is what lets
distros such as PorteuX load and unload `.xzm` modules on an
already-running system.

## Why this exists

[Original `aufs`](https://github.com/sfjro/aufs-standalone) is ~28,000 lines across `fs/aufs/` plus patches touching ~25
core kernel files to export internal symbols, and needs its own branch per
kernel minor version. `aufs-ng` targets the same on-disk format and mount
grammar with:

- **No kernel patches** — every symbol it uses is a standard, currently
  exported kernel API.
- **A much smaller surface** — ~4,000 lines, vs. `aufs`'s ~28,000.
- **Modern I/O passthrough** — reads/writes/splice/mmap go through the
  kernel's `backing_file_*` API (the same infrastructure FUSE passthrough
  and OverlayFS use) instead of taking a filesystem-wide lock on every
  read and write.

## Trade-offs

This is new code, not a driver hardened by two decades of real-world use.
Also, some `aufs` features are intentionally out of scope:

- **Pseudo-links (`plink`)** — hard-linking (not to be confused with
  symlinking) a file that comes from a layer (module) still works, just
  makes a full copy behind the scenes instead of a true link.
- **Directory-rename metadata (`dirren`)** — renaming a folder that comes
  from a layer still works, just slower (a copy instead of an instant
  rename) for large folders.
- **`shwh`** (flattening layers into one) — no built-in way to merge a
  stack of read-only layers into one clean image.
- **RDU** (readdir speed-up helper) — no userspace listing accelerator;
  only a directory with tens of thousands of entries spread across many
  layers would notice.
- **Multiple writable branches** — only one writable location is used at
  a time; all your changes go there, you can't spread them across
  several disks.
- **NFS export** — the union filesystem isn't meant to be shared out to
  other computers over the network; it's for local use only.
- **FHSM** (automatic storage tiering) — not needed: `aufs-ng` only ever
  has one writable location, so there's nothing to move files between.

## Performance

Estimated from code comparison against the original `aufs`, not from
benchmarks.

**CPU** — biggest difference shows on cached, syscall-heavy workloads.

- **Reads and writes** — `aufs` takes a filesystem-wide lock on every
  call; `aufs-ng` hands opened files to the kernel's backing-file API
  and runs at near-native speed.
- **Lookups and stat** — `aufs` probes each branch for the name and
  its whiteout, takes several locks along the way, and writes an
  inode-table entry on a file's first lookup; `aufs-ng` does the same
  probing (that part costs the same) with lighter locking and no
  table to write.
- **Copy-up** — `aufs` copies file data through a page-at-a-time
  buffer; `aufs-ng` moves it with in-kernel splice, faster for large
  files.

**RAM**

- `aufs` attaches a lock-carrying tracking structure to every cached
  dentry, inode and open file, and maintains inode-number tables on
  disk; `aufs-ng` tracks inodes only, so a warm system holds the union
  in roughly a third of the memory.

## Usage

A union stacks one writable directory on top of any number of read-only
ones and presents them as a single filesystem: everything you create,
modify or delete lands in the writable branch; the read-only branches
below provide the rest of the content. Branches are listed
highest-priority first — when the same name exists in several branches,
the one listed first wins. The option syntax is original `aufs`'s:

```
mount -t aufs -o nowarn_perm,xino=/memory/xino/.aufs.xino,br:/memory/changes=rw,udba=reval aufs /union
mount -no remount,dirperm1,add=1:module=rr aufs /    # add a layer to the live union
mount -t aufs -o remount,del=module aufs /      # remove one
```

`add=1:` inserts the new branch right below the writable one, so the
newest layer wins over older ones — the `aufs` convention.

Each branch gets a mode: `rw` (writable — only the first branch can be)
or `ro` (read-only). For compatibility, `aufs-ng` also accepts `aufs`'s
other read-only spelling `rr` (meant for natively read-only filesystems
like squashfs) and mode suffixes such as `+wh` or `+nolwh` — they all
simply mean read-only here.

- `udba=` — `reval` (the default) shows changes made directly inside a
  branch; `none` skips that detection (faster and safe if branches are
  never modified directly); `notify` is accepted but behaves as `reval`.
- `xino=` — where original `aufs` writes its inode-number table; `aufs-ng`
  keeps inode numbers stable without a table, so this is ignored.
- `dirperm1` — makes original `aufs` check only the topmost branch's
  permissions for a directory; `aufs-ng` always behaves that way, so the
  option changes nothing.
- `nowarn_perm` — silences original `aufs`'s warnings about branches with
  differing owner/permissions; `aufs-ng` never prints those warnings.

On remount, unknown options are silently ignored. Unlike original `aufs`,
there is no `/sys/fs/aufs` tree; the branch list appears directly in
`/proc/mounts`.

## On-disk format

Identical to original `aufs`: a deleted name still provided by a lower
branch is masked by a sibling regular file `.wh.<name>` (mode `0444`,
not a character device); a directory that fully shadows lower content
carries a `.wh..wh..opq` marker. Verified byte-for-byte against
original `aufs`, so external tools that read or edit a branch directly
work unchanged.

Deletion is whiteout-first, as in `aufs`: the `.wh.<name>` marker is
created before the object is removed (and rolled back on failure), so
a crash or a full branch can never silently resurrect stale lower
content. Transient bookkeeping lives in `aufs`'s own hidden `.wh..wh.`
namespace (`.wh..wh.pxu<seq>` copy-up temps, `.wh..wh.tmp.<seq>` parked
whiteouts): invisible to the merged view, cleaned up within the
operation, and — after a crash — swept with the directory like any
other stale marker.

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

Boot, runtime branch add/remove, and copy-up have been verified
end-to-end on a real live system (PorteuX), including branch changes
on an already-running union.

## License

`aufs-ng` is free software, released under the **GNU General Public
License, version 2** (`GPL-2.0-only`) — the same license as the Linux
kernel it builds against, so the project is fully GPL-friendly and can
be distributed as part of a GPL kernel tree.
