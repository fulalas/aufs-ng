# aufs-ng

`aufs-ng` is a standalone, from-scratch union filesystem, written to be a
drop-in kernel-side replacement for `aufs`, aimed mainly at live distros.
It builds against a **stock, unpatched kernel tree** — no `aufs` or `OverlayFS`
patch, nothing outside this directory.

It registers with the kernel as filesystem type **`aufs`** (not `aufs-ng`)
and speaks `aufs`'s own mount option grammar and on-disk whiteout format, so
that any script issuing original `aufs` `mount`/`remount` commands should
work (see [usage](#usage) below).

It also carries over one of `aufs`'s defining abilities: branches can be
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
mount -t aufs -o br:/memory/changes=rw,udba=reval aufs /union
mount -no remount,add=1:/path/to/module=rr aufs /    # add a module to the live union
mount -t aufs -o remount,del=/path/to/module aufs /  # remove the layer
```

`add=1:` inserts the new branch right below the writable one, so the
newest layer wins over older ones — the `aufs` convention. It is also
the only insert position `aufs-ng` implements: any other index is
rejected at mount time rather than silently merged in the wrong order.
Branches must not overlap — a branch that is the same directory as, or
nested inside, another branch is rejected, as original `aufs` does.

Each branch gets a mode: `rw` (writable — only the first branch can be)
or `ro` (read-only). For compatibility, `aufs-ng` also accepts `aufs`'s
other read-only spelling `rr` (meant for natively read-only filesystems
like squashfs) and mode suffixes such as `+wh` or `+nolwh` — the
suffixes are ignored; as in `aufs`, only the base token (`rw` vs
`ro`/`rr`) decides. A later branch declared `rw` is accepted but demoted
to read-only, with a warning in the kernel log; `/proc/mounts` reports
it as `ro`.

- `udba=` — `reval` (the default) shows changes made directly inside a
  branch; `none` skips that detection (faster and safe if branches are
  never modified directly); `notify` (and its `fsnotify` spelling) is
  accepted but behaves as `reval`. Any other value is rejected, as
  original `aufs` rejects it — a typo must not silently disable
  revalidation.
- `xino=`, `noxino` — where original `aufs` writes its inode-number table,
  and the switch that turns it off; `aufs-ng` keeps inode numbers stable
  without a table (see [on-disk format](#on-disk-format)), so both are
  ignored.
- `dirperm1` — makes original `aufs` check only the topmost branch's
  permissions for a directory; `aufs-ng` always behaves that way, so the
  option changes nothing.
- `nowarn_perm` — silences original `aufs`'s warnings about branches with
  differing owner/permissions; `aufs-ng` never prints those warnings.

On remount, unknown options are silently ignored; at mount time,
options outside the list above — including `aufs` options `aufs-ng`
doesn't implement, such as `dirs=` — are rejected. Unlike original
`aufs`, there is no `/sys/fs/aufs` tree; the branch list appears
directly in `/proc/mounts`.

Unlike original `aufs`, removing a branch doesn't fail with `EBUSY`
just because a file it provides is open (only a memory-mapped one still
does): an open file keeps working from the removed branch until closed
— the usual open-files rule — while fresh lookups resolve to a
surviving branch, if any. Same for an object already deleted through the
union while still held open — a directory (a process's cwd) as much as a
file: it keeps the removed branch until the last user lets go.

## On-disk format

The format is identical to original `aufs`: a deleted name still
provided by a lower branch is masked by a sibling regular file
`.wh.<name>` (mode `0444`, not a character device); a directory that
fully shadows lower content carries a `.wh..wh..opq` marker. Verified
byte-for-byte against original `aufs`, so external tools that read or
edit a branch directly work unchanged.

The markers themselves are the same; what can differ is the order in
which they are written when an operation needs more than one step:

- Deleting a file that also exists in a read-only branch below behaves
  exactly as in `aufs`: no matter what goes wrong — a crash or a full
  disk — a deleted file can never quietly come back.

- Renaming such a file is where `aufs-ng` differs: it renames first and
  writes the marker second — the reverse of `aufs` — so a crash can't
  hide the file mid-rename. If the marker fails, the rename rolls back
  cleanly (rare edge cases keep it instead, with a warning).

- Deleting a directory renames it to a hidden temp name first and
  cleans it up after — `aufs`'s own ordering — so no failure can bring
  deleted names back; at worst an invisible leftover stays in the
  writable branch.

Helper files named `.wh..wh.*` may briefly appear inside a branch
during an operation, same as in `aufs`. They are never visible in the
union, and any leftovers after a crash are harmless and get removed
together with their directory (a non-empty leftover directory needs
removing by hand, as in `aufs`).

`aufs-ng` writes one thing `aufs` does not. A file copied into the
writable branch gets a `trusted.aufs_ng.origin` xattr naming the file it
was copied from. That is what keeps its inode number the same afterwards
— the job `aufs` uses its `xino` tables for. It is hidden from the union
and changes nothing about the whiteout format above.

Only a real copy gets it. A file that merely ends up with the same name
as one in a lower branch — created after that name was deleted, moved
onto it, or hard-linked to a copy — is a different file and gets its own
inode number, as on any normal filesystem.

A writable branch that cannot store xattrs still works. Files copied up
there just get a new inode number once the kernel drops them from its
cache, and the log says so once. (`OverlayFS` refuses to mount at all.)

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

Boot, runtime branch add/remove, and copy-up have been verified
end-to-end on a real live system (PorteuX), including branch changes
on an already-running union.

## License

`aufs-ng` is free software, released under the **GNU General Public
License, version 2** (`GPL-2.0-only`) — the same license as the Linux
kernel it builds against, so the project is fully GPL-friendly and can
be distributed as part of a GPL kernel tree.
