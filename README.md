# aufs-ng

`aufs-ng` is a standalone, from-scratch union filesystem, written to be a
drop-in kernel-side replacement for `aufs`, aimed mainly at live distros.
It builds against a **stock, unpatched kernel tree** — no `aufs` or `OverlayFS`
patch, nothing outside this directory.

It registers with the kernel as filesystem type **`aufs`** (not `aufs-ng`)
and speaks `aufs`'s own mount option grammar and on-disk whiteout format, so
that any script issuing original `aufs` `mount`/`remount` commands should
work (see [usage](#usage) below).

It also carries over one of `aufs`'s defining abilities: layers can be
added to or removed from the union while it's mounted, with no unmount
or reboot required. This is what lets distros like PorteuX load and unload
modules on an already-running system.

## Why this exists

Original [`aufs`](https://github.com/sfjro/aufs-standalone) is ~28,000 lines across `fs/aufs/` plus patches touching ~25
core kernel files to export internal symbols, and needs its own branch per
kernel minor version. `aufs-ng` stays compatible with the on-disk format
and mount grammar, while providing the following improvements:

- **No kernel patches** — every symbol it uses is a standard, currently
  exported kernel API.
- **A much smaller surface** — ~5,000 lines, vs. `aufs`'s ~28,000.
- **Modern I/O passthrough** — reads/writes/splice/mmap go through the
  kernel's `backing_file_*` API (the same infrastructure FUSE passthrough
  and `OverlayFS` use) instead of taking a filesystem-wide lock on every
  read and write.

## Performance

Estimated from code comparison against the original `aufs`, not from
benchmarks.

**CPU** — biggest gain shows on cached, syscall-heavy workloads.

- **Reads and writes** — `aufs` takes a filesystem-wide lock on every
  call; `aufs-ng` hands opened files to the kernel's backing-file API
  and runs at near-native speed.
- **Lookups and stat** — `aufs` probes each layer for the name and
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

A union shows several folders as if they were one. The first is writable
and takes everything you create, change or delete; the rest are read-only
and only provide content. When the same name exists in more than one, the
first one listed wins. The mount options call such a folder a branch.

```
mount -t aufs -o br:/memory/changes=rw,udba=reval aufs /union
mount -o remount,add=1:/path/to/layer=ro aufs /union   # add a layer
mount -o remount,del=/path/to/layer aufs /union        # remove it
```

A layer can be added or removed while the union is mounted, which is what
lets a live distro load and unload modules on a running system. A new one
goes on top of the read-only layers, so it wins over the older ones. There
is no preset limit on how many (up to 32767).

Each layer is `rw` (writable, first layer only) or `ro`/`rr` (read-only).
A later layer asking for `rw` is accepted but stays read-only, with a note
in the kernel log. Layers may not overlap: one cannot be inside another.

- `udba=` — whether to notice changes made directly inside a layer, behind
  the union's back. `reval` (the default) notices them; `none` trusts the
  cache and is a bit faster.
- `xino=`, `noxino`, `dirperm1`, `nowarn_perm` — taken for compatibility;
  they change nothing here.

Unknown options are rejected when mounting and ignored when remounting.

`/proc/mounts` shows the mount id and the options, not the list of layers —
same as `aufs`.

Removing a layer works even while a file it provides is open, unless that
file is memory-mapped: the open file keeps working until it is closed, and
new lookups find another layer. The same goes for something deleted through
the union but still held open, a folder in use as a working directory
included.

## On-disk format

Identical to `aufs`, byte for byte, so tools that read or write a layer
directly keep working. A deleted name that a lower layer still provides is
covered by a file called `.wh.<name>`; a folder that hides everything below
it carries `.wh..wh..opq`. Files named `.wh..wh.*` can show up inside a
layer while an operation runs; they never appear in the union, and leftovers
after a crash are harmless.

One step order differs: a rename writes its marker after the rename instead
of before, so a crash cannot hide the file halfway. Deletes keep `aufs`'s
order, so a deleted file can never quietly come back.

`aufs-ng` writes one thing `aufs` does not. A file copied into the writable
layer gets a `trusted.aufs_ng.origin` xattr naming the file it came from,
which is what keeps its inode number the same afterwards — the job `aufs`
uses its `xino` tables for. It is hidden from the union. Only a real copy
gets it, so a file that merely ends up with the same name as one below is a
different file with its own inode number, as on any normal filesystem.

A writable layer that cannot store xattrs still works. Copied files just get
a new inode number once the kernel drops them from its cache, and the log
says so once. (`OverlayFS` refuses to mount at all.)

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
- **Multiple writable layers** — only one writable location is used at
  a time; all your changes go there, you can't spread them across
  several disks.
- **NFS export** — the union filesystem isn't meant to be shared out to
  other computers over the network; it's for local use only.
- **FHSM** (automatic storage tiering) — not needed: `aufs-ng` only ever
  has one writable location, so there's nothing to move files between.

## Building

Builds **into the kernel** (`CONFIG_AUFSNG_FS=y`), not as a loadable
module — a live-boot sequence typically needs this filesystem type
mounted before any loadable module can be reached at all.

To integrate into a kernel source tree (any anchor line in `fs/Kconfig`/
`fs/Makefile` works; the `OverlayFS` entry is just a convenient, stable one):

```sh
git clone https://github.com/fulalas/aufs-ng fs/aufs-ng
sed -i '/source "fs\/overlayfs\/Kconfig"/a source "fs/aufs-ng/Kconfig"' fs/Kconfig
sed -i '/obj-\$(CONFIG_OVERLAY_FS)\s*+= overlayfs\//a obj-$(CONFIG_AUFSNG_FS)\t+= aufs-ng/' fs/Makefile
echo "CONFIG_AUFSNG_FS=y" >> .config
```

Then build the kernel as usual.

For a quick out-of-tree test build against an already-built kernel tree
(producing a loadable `.ko` instead, no `fs/Kconfig`/`fs/Makefile` edits
needed):

```sh
make -C /path/to/kernel/build M=$PWD CONFIG_AUFSNG_FS=m W=1 modules
```

## Status

Boot, runtime layer add/remove, and copy-up have been verified
end-to-end on a real live system (PorteuX), including layer changes
on an already-running union.

## License

`aufs-ng` is free software, released under the **GNU General Public
License, version 2** (`GPL-2.0-only`) — the same license as the Linux
kernel it builds against, so the project is fully GPL-friendly and can
be distributed as part of a GPL kernel tree.
