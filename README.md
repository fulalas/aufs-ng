# aufs-ng

`aufs-ng` is a standalone, from-scratch union filesystem, written to be a
drop-in kernel-side replacement for AUFS to be used mainly by live distros.
It builds against a **stock, unpatched kernel tree** — no AUFS patch set,
no OverlayFS patch, nothing outside this directory.

It registers with the kernel as filesystem type **`aufs`** (not `aufs-ng`)
and speaks AUFS's own mount option grammar and on-disk whiteout format, so
that any script issuing original AUFS `mount`/`remount` commands works
completely unmodified. `aufs-ng` is just the project name; `aufs` is
the filesystem name the kernel and userspace see.

It also carries over one of AUFS's defining abilities: branches can be
added to or removed from the union while it's mounted, with no unmount
or reboot required — and without disturbing open files, working
directories, or mounts nested inside the union. This is what lets
distros such as PorteuX load and unload `.xzm` modules on an
already-running system.

## Why this exists

[Original AUFS](https://github.com/sfjro/aufs-standalone) is ~28,000 lines across `fs/aufs/` plus patches touching ~25
core kernel files to export internal symbols, and needs its own branch per
kernel minor version. `aufs-ng` targets the same on-disk format and mount
grammar with:

- **No kernel patches** — every symbol it uses is a standard, currently
  exported kernel API.
- **A much smaller surface** — ~4,000 lines, vs. AUFS's ~28,000.
- **Modern I/O passthrough** — reads/writes/splice/mmap go through the
  kernel's `backing_file_*` API (the same infrastructure FUSE passthrough
  and OverlayFS use) instead of taking a filesystem-wide lock on every
  read and write.

## Trade-offs

This is new code, not a driver hardened by two decades of real-world use.
Also, some AUFS features are intentionally out of scope:

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
- **FHSM** (automatic storage tiering) — not needed: aufs-ng only ever
  has one writable location, so there's nothing to move files between.

## Usage

A union stacks one writable directory on top of any number of read-only
ones and presents them as a single filesystem: everything you create,
modify or delete lands in the writable branch; the read-only branches
below provide the rest of the content. Branches are listed
highest-priority first — when the same name exists in several branches,
the one listed first wins. The option syntax is original AUFS's:

```
mount -t aufs -o nowarn_perm,xino=/memory/xino/.aufs.xino,br:/memory/changes=rw,udba=reval aufs /union
mount -no remount,dirperm1,add=1:module=rr aufs /    # add a layer to the live union
mount -t aufs -o remount,del=module aufs /      # remove one
```

`add=1:` inserts the new branch right below the writable one, so the
newest layer wins over older ones — the AUFS convention.

Each branch gets a mode: `rw` (writable — only the first branch can be)
or `ro` (read-only). For compatibility, aufs-ng also accepts AUFS's
other read-only spelling `rr` (meant for natively read-only filesystems
like squashfs) and mode suffixes such as `+wh` or `+nolwh` — they all
simply mean read-only here.

- `udba=` — what happens when a branch is edited directly, behind the
  union's back: `reval` (the default) picks such edits up; `none` trusts
  the cache (slightly faster, fine when branches only ever change
  through the union); `notify` is accepted and behaves as `reval`.
- `xino=`, `dirperm1`, `nowarn_perm` — accepted so existing AUFS mount
  lines work unchanged; they have no effect.

On remount, unknown options are silently ignored. Unlike original AUFS,
there is no `/sys/fs/aufs` tree; the branch list appears directly in
`/proc/mounts`.

## On-disk format

Identical to original AUFS: a deleted name still provided by a lower
branch is masked by a sibling regular file `.wh.<name>` (mode `0444`,
not a character device); a directory that fully shadows lower content
carries a `.wh..wh..opq` marker. Verified byte-for-byte against
original AUFS, so external tools that read or edit a branch directly
work unchanged.

Deletion is whiteout-first, as in AUFS: the `.wh.<name>` marker is
created before the object is removed (and rolled back on failure), so
a crash or a full branch can never silently resurrect stale lower
content. Transient bookkeeping lives in AUFS's own hidden `.wh..wh.`
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
end-to-end, including adding a branch on an already-running system.
The full-tree review rework (whiteout ordering, udba=reval positive
revalidation, branch-add splicing, readdir caching) compiles clean but
has not yet been re-verified on a live system.

## License

`aufs-ng` is free software, released under the **GNU General Public
License, version 2** (`GPL-2.0-only`) — the same license as the Linux
kernel it builds against, so the project is fully GPL-friendly and can
be distributed as part of a GPL kernel tree.
