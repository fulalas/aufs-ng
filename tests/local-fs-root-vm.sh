#!/bin/bash
# Run only in a disposable test VM booted with aufsng_test=1.
set -euo pipefail
if [ "${EUID}" != 0 ] || ! grep -qw aufsng_test=1 /proc/cmdline; then
    echo 'Requires root inside a disposable aufsng_test=1 VM.' >&2
    exit 2
fi
WORK=$(mktemp -d /tmp/aufsng-local-root.XXXXXX)
cleanup() {
    result=$?
    trap - EXIT
    for path in nested union alias ext ext2 btrfs fold; do
        if mountpoint -q "$WORK/$path"; then umount "$WORK/$path" || result=1; fi
    done
    if ! findmnt -rn -o TARGET | grep -Fq "$WORK/"; then rm -rf -- "$WORK"; fi
    exit "$result"
}
trap cleanup EXIT
mkdir -p "$WORK"/{nested,union,alias,ext,ext2,btrfs,lower,upper,fold}
test_local_root() {
    fs=$1
    shift
    truncate -s 256M "$WORK/$fs.img"
    "$@" "$WORK/$fs.img"
    mount -t "$fs" -o loop "$WORK/$fs.img" "$WORK/$fs"
    mount -t aufs -o "br=$WORK/$fs=rw" aufs "$WORK/union"
    printf '%s\n' "$fs" >"$WORK/union/probe"
    grep -qx "$fs" "$WORK/$fs/probe"
    umount "$WORK/union"
    umount "$WORK/$fs"
    echo "PASS: $fs filesystem root is a writable local branch"
}
truncate -s 128M "$WORK/ext4.img"
mkfs.ext4 -F -q "$WORK/ext4.img"
mount -t ext4 -o loop "$WORK/ext4.img" "$WORK/ext"
printf 'lower\n' >"$WORK/lower/lower-file"
printf 'ext4\n' >"$WORK/ext/ext4-file"
mount -t aufs -o "br=$WORK/ext=rw:$WORK/lower=ro" aufs "$WORK/union"
grep -qx lower "$WORK/union/lower-file"
printf 'changed\n' >"$WORK/union/lower-file"
grep -qx changed "$WORK/ext/lower-file"
grep -qx lower "$WORK/lower/lower-file"
rm "$WORK/union/lower-file"
[ ! -e "$WORK/union/lower-file" ]
[ -e "$WORK/ext/.wh.lower-file" ]
umount "$WORK/union"
echo 'PASS: ext4 filesystem root is writable upper with copy-up and whiteout'
mount --bind "$WORK/ext" "$WORK/alias"
mount -t aufs -o "br=$WORK/alias=rw" aufs "$WORK/union"
grep -qx ext4 "$WORK/union/ext4-file"
umount "$WORK/union"
umount "$WORK/alias"
echo 'PASS: bind-mounted ext4 filesystem root'
mount -t aufs -o "br=$WORK/upper=rw:$WORK/ext=ro" aufs "$WORK/union"
grep -qx ext4 "$WORK/union/ext4-file"
umount "$WORK/union"
echo 'PASS: ext4 filesystem root as read-only lower'
mount -t aufs -o "br=$WORK/upper=rw" aufs "$WORK/union"
mount -t aufs -o "remount,add=1:$WORK/ext=ro" aufs "$WORK/union"
grep -qx ext4 "$WORK/union/ext4-file"
mount -t aufs -o "remount,del=$WORK/ext" aufs "$WORK/union"
[ ! -e "$WORK/union/ext4-file" ]
umount "$WORK/union"
echo 'PASS: ext4 filesystem root dynamic add and remove'
mkdir "$WORK/ext/sub"
mount -t aufs -o "br=$WORK/ext/sub=rw" aufs "$WORK/union"
printf 'subdir\n' >"$WORK/union/probe"
umount "$WORK/union"
grep -qx subdir "$WORK/ext/sub/probe"
echo 'PASS: ext4 subdirectory behavior unchanged'
test_local_root ext2 mkfs.ext2 -F -q
if command -v mkfs.btrfs >/dev/null; then
    test_local_root btrfs mkfs.btrfs -f -q
else
    echo 'SKIP: mkfs.btrfs is unavailable'
fi
truncate -s 128M "$WORK/fold.img"
mkfs.ext4 -F -q -O casefold "$WORK/fold.img"
mount -t ext4 -o loop "$WORK/fold.img" "$WORK/fold"
if mount -t aufs -o "br=$WORK/fold=rw" aufs "$WORK/union"; then
    echo 'FAIL: casefold root was unexpectedly accepted' >&2; exit 1
fi
echo 'PASS: casefold root remains rejected'
mount -t aufs -o "br=$WORK/upper=rw" aufs "$WORK/union"
if mount -t aufs -o "br=$WORK/union=rw" aufs "$WORK/nested"; then
    echo 'FAIL: revalidating AUFS root was unexpectedly accepted' >&2; exit 1
fi
umount "$WORK/union"
echo 'PASS: non-fscrypt revalidating AUFS root remains rejected'
printf 'LOCAL_FS_ROOT_%s_OK\n' REGRESSIONS
