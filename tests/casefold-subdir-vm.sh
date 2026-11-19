#!/bin/sh
# Run in a disposable VM with aufs-ng loaded and aufsng_test=1 on the cmdline.
set -eu
[ "$(id -u)" = 0 ] && grep -qw aufsng_test=1 /proc/cmdline || exit 2
t=$(mktemp -d /tmp/aufs-casefold.XXXXXX)
cleanup() {
    rc=$?
    trap - EXIT
    if mountpoint -q "$t/union"; then umount "$t/union" || exit 1; fi
    if mountpoint -q "$t/ext"; then umount "$t/ext" || exit 1; fi
    rm -rf -- "$t"
    exit "$rc"
}
trap cleanup EXIT
mkdir "$t/plain" "$t/union" "$t/ext"

# Prove the same mount invocation works for a supported branch.
mount -t aufs -o "br=$t/plain=rw" aufs "$t/union"
echo ok >"$t/union/probe"
grep -qx ok "$t/plain/probe"
umount "$t/union"

truncate -s 64M "$t/ext.img"
mkfs.ext4 -q -F -O casefold "$t/ext.img"
mount -t ext4 -o loop "$t/ext.img" "$t/ext"
mkdir "$t/ext/sub"
chattr +F "$t/ext/sub"
echo folded >"$t/ext/sub/Mixed"
grep -qx folded "$t/ext/sub/mixed"
echo 'PASS: backing lookup is case-insensitive'

if mount -t aufs -o "br=$t/ext=rw" aufs "$t/union"; then
    echo 'FAIL: casefold filesystem root accepted' >&2
    exit 1
fi
if mount -t aufs -o "br=$t/ext/sub=rw" aufs "$t/union"; then
    echo 'FAIL: casefold subdirectory accepted as a branch' >&2
    exit 1
fi
echo 'PASS: both the filesystem root and the casefold subdirectory are rejected'
