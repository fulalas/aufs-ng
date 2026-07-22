#!/bin/bash
# aufs-ng test suite - a single command that tests everything.
#
#   tests/run-tests.sh                      # kernel tree autodetected (linux-*/)
#   KERNEL_SRC=/path/to/linux tests/run-tests.sh
#
# It builds a User-Mode Linux kernel with aufs-ng built in (plus
# lockdep/RCU/atomic-sleep debugging) and boots it to run the
# behavioral checks.  No root needed, and the running system is never
# touched: the guest reads the host filesystem through hostfs and
# writes only to tmpfs it creates itself.
#
# Environment:
#   KERNEL_SRC  kernel source tree to build (an existing non-UML
#               .config is reset with mrproper)
#   JOBS        parallel make jobs (default: nproc)
#   TIMEOUT     guest wall-clock limit in seconds (default: 300)
#
# Exit status: 0 only if every check passed, the suite ran to
# completion, and the kernel log shows no lockdep/RCU/BUG findings.
#
# --------------------------------------------------------------------
# ONE FILE, TWO ROLES.  There is a single entry point on purpose - you
# never have to wonder what to run.  Internally the script plays two
# parts, which run in two different worlds:
#
#   * HOST side (host_main)  - builds and boots the UML kernel, then
#                              judges the result.  This is what runs
#                              when you invoke the script normally.
#   * GUEST side (guest_main) - the actual checks, run as PID 1 inside
#                              the throwaway UML kernel.  It mounts and
#                              deletes freely and powers the guest off
#                              when done, so it must NEVER run on a
#                              real system.
#
# The host boots the kernel with this same script as init and the
# sentinel AUFSNG_TEST_GUEST=1 in the environment; the dispatch at the
# very bottom picks the guest side only when that sentinel is set, so a
# normal invocation always takes the safe host path.
# --------------------------------------------------------------------

set -u

# ====================================================================
# GUEST SIDE - runs as PID 1 inside UML.  Every check prints one
# "N/TOTAL - name... PASSED|FAILED" line (green/red); a failure also
# emits a plain "TEST-FAIL:" marker the host judge greps for.  The
# suite ends with a RESULT: summary and TESTS-COMPLETE, then powers
# off.  TOTAL must match the number of checks on the all-pass path -
# the tail asserts it, so a stale count fails the suite loudly.
# ====================================================================
guest_main() {
	export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
	# mount/umount/poweroff helper, compiled and staged beside this
	# script by the host side: m TYPE SRC TGT [DATA] [FLAGS] | -u TGT | -o
	local M
	M="$(dirname "$0")/mnt"

	$M proc proc /proc
	$M tmpfs tmpfs /dev
	mknod /dev/null c 1 3 2>/dev/null
	$M tmpfs tmpfs /mnt

	N=0; TOTAL=49; PASS=0; FAIL=0
	ok()  { N=$((N+1)); PASS=$((PASS+1))
		printf '%d/%d - %s... \033[1;32mPASSED\033[0m\n' "$N" "$TOTAL" "$1"; }
	bad() { N=$((N+1)); FAIL=$((FAIL+1))
		printf '%d/%d - %s... \033[1;31mFAILED\033[0m\n' "$N" "$TOTAL" "$1"
		echo "TEST-FAIL: $1"; }
	check() { local desc="$1"; shift
		if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi; }
	checkfail() { local desc="$1"; shift
		if "$@" >/dev/null 2>&1; then bad "$desc"; else ok "$desc"; fi; }

	W=/mnt/w; L1=/mnt/l1; L2=/mnt/l2; L3=/mnt/l3; U=/mnt/union
	mkdir -p $W $L1 $L2 $L3 $U
	for d in $W $L1 $L2 $L3; do $M tmpfs tmpfs $d; done

	### populate lowers (before mount; lowers are never written through union)
	mkdir -p $L1/dir $L1/bin $L2/dir
	echo l1-file  > $L1/dir/file
	echo l1-only  > $L1/only1
	echo l2-file  > $L2/dir/file
	echo l2-only  > $L2/only2
	echo oldtool  > $L1/bin/tool
	echo linkdata > $L1/a
	ln $L1/a $L1/b

	echo "=== 1. mount + basic merge ==="
	$M aufs aufs $U "br:$W=rw:$L1=ro:$L2=ro,udba=reval" || { bad "mount"; echo FATAL; $M -o; }
	ok "mount"
	check "merge: l1 wins dir/file"      grep -q l1-file $U/dir/file
	check "merge: only1 visible"         test -f $U/only1
	check "merge: only2 visible"         test -f $U/only2
	ls $U | grep -q '^\.wh\.' && bad "no .wh. in readdir" || ok "no .wh. in readdir"

	echo "=== 2. write / whiteout / resurrect ==="
	echo upper > $U/newfile
	check "create lands in rw branch"    test -f $W/newfile
	rm $U/only1
	check "whiteout created"             test -f $W/.wh.only1
	checkfail "deleted name gone"        test -e $U/only1
	echo recreated > $U/only1
	check "recreate over whiteout"       grep -q recreated $U/only1
	check "whiteout removed on recreate" test ! -e $W/.wh.only1

	echo "=== 3. last-added-wins on cached positive (the add=1 fix) ==="
	cat $U/bin/tool >/dev/null                      # cache the dentry
	grep -q oldtool $U/bin/tool || bad "precondition"
	mkdir -p $L3/bin; echo newtool > $L3/bin/tool
	$M aufs aufs $U "add=1:$L3=rr" 32 || bad "remount add=1"
	grep -q newtool $U/bin/tool \
		&& ok "cached positive re-resolves to new branch" \
		|| bad "cached positive re-resolves to new branch (still $(cat $U/bin/tool))"
	$M aufs aufs $U "del=$L3" 32 || bad "remount del"
	grep -q oldtool $U/bin/tool \
		&& ok "removal restores old resolution" \
		|| bad "removal restores old resolution"

	echo "=== 4. negative dentry + branch add ==="
	test -e $U/bin/ghost; :                          # cache negative
	echo boo > $L3/bin/ghost
	$M aufs aufs $U "add=1:$L3=rr" 32
	check "cached negative re-resolves"  grep -q boo $U/bin/ghost
	$M aufs aufs $U "del=$L3" 32

	echo "=== 5. out-of-band chmod enforcement (attr re-sync fix) ==="
	echo secret > $U/dir/priv                        # goes to upper
	chmod 644 $U/dir/priv
	cat $U/dir/priv >/dev/null                       # cache with 0644
	chmod 000 $W/dir/priv                            # out-of-band, via branch
	chroot --userspec=65534:65534 / /bin/cat $U/dir/priv >/dev/null 2>&1 \
		&& bad "chmod-through-branch enforced for non-root" \
		|| ok  "chmod-through-branch enforced for non-root"
	chmod 644 $W/dir/priv
	chroot --userspec=65534:65534 / /bin/cat $U/dir/priv >/dev/null 2>&1 \
		&& ok "chmod back to 644 re-allows" || bad "chmod back to 644 re-allows"

	echo "=== 6. hardlink sibling unlink (plink-less fix) ==="
	echo modified > $U/a                             # copy-up of a
	check "sibling b still visible"      test -f $U/b
	rm $U/b && ok "unlink of hardlink sibling succeeds" || bad "unlink of hardlink sibling succeeds"
	checkfail "b gone from union"        test -e $U/b
	check "a keeps upper content"        grep -q modified $U/a

	echo "=== 7. ENOSPC mkdir-over-whiteout retry (cleanup fix) ==="
	mkdir -p /mnt/tiny /mnt/tinyl /mnt/tinyu
	$M tmpfs tmpfs /mnt/tiny "nr_inodes=16"
	$M tmpfs tmpfs /mnt/tinyl
	mkdir -p /mnt/tinyl/d; echo x > /mnt/tinyl/d/inside
	$M aufs aufs /mnt/tinyu "br:/mnt/tiny=rw:/mnt/tinyl=ro"
	rm -rf /mnt/tinyu/d                              # delete covered dir -> whiteout
	test -f /mnt/tiny/.wh.d || bad "precondition: whiteout for d"
	i=0; while echo > /mnt/tiny/f$i 2>/dev/null; do i=$((i+1)); done   # exhaust inodes
	rm /mnt/tiny/f$((i-1))                           # leave exactly 1 free inode
	mkdir /mnt/tinyu/d 2>/dev/null && bad "mkdir must fail (marker ENOSPC)" \
		|| ok "mkdir fails cleanly under ENOSPC"
	test -f /mnt/tiny/.wh.d && ok "whiteout restored after failure" || bad "whiteout restored after failure"
	test -e /mnt/tiny/d && bad "no leftover upper dir" || ok "no leftover upper dir"
	rm -f /mnt/tiny/f*                               # free space
	mkdir /mnt/tinyu/d && ok "retry after ENOSPC succeeds (was EEXIST)" \
		|| bad "retry after ENOSPC succeeds (was EEXIST)"
	checkfail "opaque: lower 'inside' hidden" test -e /mnt/tinyu/d/inside
	$M -u /mnt/tinyu

	echo "=== 8. rename rollback with lower-only victim (had_victim fix) ==="
	$M -u /mnt/tiny; $M tmpfs tmpfs /mnt/tiny "nr_inodes=16"
	mkdir -p /mnt/tinyl2; $M tmpfs tmpfs /mnt/tinyl2
	echo lower-old > /mnt/tinyl2/old; echo lower-target > /mnt/tinyl2/target
	$M aufs aufs /mnt/tinyu "br:/mnt/tiny=rw:/mnt/tinyl2=ro"
	echo upper-old > /mnt/tinyu/old                  # upper 'old' shadows lower 'old'
	grep -q upper-old /mnt/tinyu/old || bad "precondition: upper old"
	i=0; while echo > /mnt/tiny/f$i 2>/dev/null; do i=$((i+1)); done   # 0 free inodes
	mv /mnt/tinyu/old /mnt/tinyu/target 2>/dev/null \
		&& bad "mv must fail (whiteout ENOSPC, rollback)" \
		|| ok  "mv fails cleanly when whiteout cannot be created"
	grep -q upper-old /mnt/tinyu/old 2>/dev/null \
		&& ok "rename rolled back: old keeps upper content" \
		|| bad "rename rolled back: old keeps upper content (got: $(cat /mnt/tinyu/old 2>&1))"
	grep -q lower-target /mnt/tinyu/target 2>/dev/null \
		&& ok "target still shows lower content" \
		|| bad "target still shows lower content"
	rm -f /mnt/tiny/f*
	$M -u /mnt/tinyu; $M -u /mnt/tiny

	echo "=== 9. udba remount + negative reval flip ==="
	mkdir -p /mnt/nu /mnt/nw /mnt/nl
	$M tmpfs tmpfs /mnt/nw; $M tmpfs tmpfs /mnt/nl
	echo x > /mnt/nl/seed
	$M aufs aufs /mnt/nu "br:/mnt/nw=rw:/mnt/nl=ro,udba=none"
	grep " /mnt/nu " /proc/mounts | grep -q udba=none && ok "udba=none shown" || bad "udba=none shown"
	test -e /mnt/nu/oob; :                          # cache negative
	echo surprise > /mnt/nw/oob                     # out-of-band create in rw branch
	test -e /mnt/nu/oob && bad "udba=none keeps stale negative" || ok "udba=none keeps stale negative"
	$M aufs aufs /mnt/nu "udba=reval" 32 || bad "remount udba"
	grep " /mnt/nu " /proc/mounts | grep -q udba=reval && ok "remounted udba shown" || bad "remounted udba shown"
	test -e /mnt/nu/oob && ok "udba=reval detects out-of-band create (remount applied)" \
		|| bad "udba=reval detects out-of-band create (remount applied)"
	$M -u /mnt/nu

	echo "=== 10. read-only rw-branch rejected at mount ==="
	mkdir -p /mnt/robr /mnt/rou
	$M tmpfs tmpfs /mnt/robr
	$M tmpfs none /mnt/robr "" 33                   # remount,ro
	$M aufs aufs /mnt/rou "br:/mnt/robr=rw:$L1=ro" 2>/dev/null \
		&& bad "ro rw-branch mount rejected" || ok "ro rw-branch mount rejected"

	echo "=== 11. add/del + pinned cwd stress (race regression) ==="
	( cd $U/dir && for i in $(seq 1 400); do stat $U >/dev/null 2>&1; stat . >/dev/null 2>&1; ls $U/bin >/dev/null 2>&1; cat /proc/mounts >/dev/null 2>&1; done ) &
	SPID=$!
	for i in $(seq 1 25); do
		$M aufs aufs $U "add=1:$L3=rr" 32 2>/dev/null
		stat $U >/dev/null 2>&1; ls $U >/dev/null 2>&1
		$M aufs aufs $U "del=$L3" 32 2>/dev/null
	done
	wait $SPID
	ok "add/del stress with pinned cwd survived"

	echo "=== 12. lookup/readdir agreement after add ==="
	mkdir -p $L3/dir; echo l3wins > $L3/dir/file
	cat $U/dir/file >/dev/null                       # cache l1's file
	$M aufs aufs $U "add=1:$L3=rr" 32
	CAT=$(cat $U/dir/file)
	[ "$CAT" = "l3wins" ] && ok "lookup honors new branch after add" || bad "lookup honors new branch after add (got $CAT)"
	$M aufs aufs $U "del=$L3" 32

	echo "=== 13. branch add hides a cached lower-only dir (whiteout) ==="
	mkdir -p $L1/wdir; echo winside > $L1/wdir/wfile
	cat $U/wdir/wfile >/dev/null 2>&1                # cache dir + child
	test -d $U/wdir || bad "precondition: wdir cached"
	touch $L3/.wh.wdir                               # whiteout in the to-be-top branch
	$M aufs aufs $U "add=1:$L3=rr" 32
	test -e $U/wdir && bad "whiteouted dir hidden after add" || ok "whiteouted dir hidden after add"
	ls $U | grep -q '^wdir$' && bad "readdir agrees dir is hidden" || ok "readdir agrees dir is hidden"
	$M aufs aufs $U "del=$L3" 32
	grep -q winside $U/wdir/wfile 2>/dev/null \
		&& ok "dir returns after branch removal" || bad "dir returns after branch removal"
	rm -f $L3/.wh.wdir

	echo "=== 14. branch add shadows a cached dir with a file ==="
	mkdir -p $L1/sdir; echo sinside > $L1/sdir/sfile
	cat $U/sdir/sfile >/dev/null 2>&1                # cache dir + child
	echo shadow > $L3/sdir                           # same-named FILE in new top branch
	$M aufs aufs $U "add=1:$L3=rr" 32
	grep -q shadow $U/sdir 2>/dev/null \
		&& ok "file shadows cached dir after add" \
		|| bad "file shadows cached dir after add (got: $(ls -ld $U/sdir 2>&1))"
	test -d $U/sdir && bad "shadowed path is not a dir" || ok "shadowed path is not a dir"
	$M aufs aufs $U "del=$L3" 32
	grep -q sinside $U/sdir/sfile 2>/dev/null \
		&& ok "dir returns after shadow branch removal" || bad "dir returns after shadow branch removal"
	rm -f $L3/sdir

	echo "=== 15. rmdir of dir whose upper vanished out-of-band still fails ==="
	mkdir -p $L1/odir                                # empty lower-only dir
	mkdir $U/odir/sub 2>/dev/null                    # copy-up odir (gains upper)
	rmdir $U/odir/sub
	test -d $W/odir || bad "precondition: odir upper exists"
	mv $W/odir $W/odir-renamed                       # out-of-band rename in rw branch
	rmdir $U/odir 2>/dev/null && bad "rmdir fails when upper dir vanished" \
		|| ok "rmdir fails when upper dir vanished"
	test -f $W/.wh.odir && bad "whiteout rolled back on failed rmdir" \
		|| ok "whiteout rolled back on failed rmdir"
	mv $W/odir-renamed $W/odir                       # restore

	echo "=== 16. copy-up fidelity: data, mode, owner, st_ino ==="
	# A multi-block file (~288 KiB) so the copy-up data loop runs many
	# iterations; mode/owner set on the lower so we can prove they survive.
	seq 1 50000 > $L1/cu
	chmod 640 $L1/cu
	chown 65534:65534 $L1/cu
	INO1=$(stat -c %i $U/cu)                          # union ino before copy-up
	exec 4>>"$U/cu"; exec 4>&-                        # append-open (no data) -> copy-up
	check "copy-up occurred (upper exists)"      test -f $W/cu
	# compare the UPPER copy against the lower origin (not via the union,
	# which would be vacuous if copy-up silently did nothing)
	check "copy-up preserves file data byte-for-byte" cmp -s $W/cu $L1/cu
	[ "$(stat -c %a $W/cu)" = 640 ] \
		&& ok "copy-up preserves mode" || bad "copy-up preserves mode"
	[ "$(stat -c %u $W/cu)" = 65534 ] \
		&& ok "copy-up preserves owner" || bad "copy-up preserves owner"
	[ "$(stat -c %i $U/cu)" = "$INO1" ] \
		&& ok "st_ino stable across copy-up" \
		|| bad "st_ino stable across copy-up (was $INO1, now $(stat -c %i $U/cu))"

	echo "=== 17. symlink read through the union (get_link) ==="
	# Guards aufsng_get_link()/aufsng_path_real() resolving a lower-only
	# symlink (numlower>0, no upper) - the path hardened against a torn
	# upper/lower snapshot.  (Copying a symlink UP is a separate, known
	# limitation - aufsng_set_attr_from() rejects ATTR_MODE on a symlink -
	# so it is deliberately not asserted here.)
	ln -s /some/target/path $L1/sl
	[ "$(readlink $U/sl)" = /some/target/path ] \
		&& ok "lower symlink read through union" \
		|| bad "lower symlink read through union"

	echo "=== 18. merged directory link count (getattr) ==="
	# md exists in two lower branches with distinct subdirs; the union's
	# nlink must fold in every branch, as aufsng_getattr computes it.
	mkdir -p $L1/md/s1 $L1/md/s2 $L2/md/s3
	exp=$(( $(stat -c %h $L1/md) + $(stat -c %h $L2/md) - 2 ))
	[ "$(stat -c %h $U/md)" = "$exp" ] \
		&& ok "merged dir nlink counts all branches" \
		|| bad "merged dir nlink counts all branches (got $(stat -c %h $U/md), want $exp)"

	# A miscount here means a check was added/removed without updating
	# TOTAL - fail loudly so the "N/TOTAL" numbering stays honest.
	if [ "$N" != "$TOTAL" ]; then
		echo "TEST-FAIL: ran $N checks but TOTAL=$TOTAL (update TOTAL and the README count)"
		FAIL=$((FAIL+1))
	fi

	echo "==================================================="
	echo "RESULT: PASS=$PASS FAIL=$FAIL"
	dmesg | grep -iE "BUG|WARN|lockdep|suspicious|circular|use-after-free|KASAN|sleeping function" | head -25
	echo "TESTS-COMPLETE"
	$M -o
	sleep 30
}

# ====================================================================
# HOST SIDE - build the UML kernel, boot the guest, judge the output.
# ====================================================================

# The mount/umount/poweroff helper the guest needs, kept here as a
# heredoc so tests/ is a single self-contained script.  The host's own
# mount(8) is unusable inside the guest (often unreadable through
# hostfs to an unprivileged user), so the guest gets this tiny tool.
aufsng_write_helper() {
	cat > "$1" <<'EOF'
/* mount/umount/poweroff helper for the UML test guest (see run-tests.sh) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "-o")) {	/* poweroff */
		sync();
		reboot(RB_POWER_OFF);
		perror("reboot");
		return 1;
	}
	if (argc >= 3 && !strcmp(argv[1], "-u")) {	/* umount TGT */
		if (umount2(argv[2], 0)) {
			perror("umount");
			return 1;
		}
		return 0;
	}
	/* mnt TYPE SRC TGT [DATA] [FLAGS] */
	if (argc < 4) {
		fprintf(stderr, "usage: mnt TYPE SRC TGT [DATA] [FLAGS] | mnt -u TGT | mnt -o\n");
		return 2;
	}
	{
		const char *data = (argc > 4 && argv[4][0]) ? argv[4] : NULL;
		unsigned long flags = argc > 5 ? strtoul(argv[5], NULL, 0) : 0;

		if (mount(argv[2], argv[3], argv[1], flags, data)) {
			perror("mount");
			return 1;
		}
	}
	return 0;
}
EOF
}

host_main() {
	local self here repo JOBS TIMEOUT KDIR work log fails warnpat status d

	self=$(readlink -f "$0")
	here=$(dirname "$self")
	repo=$(dirname "$here")
	JOBS=${JOBS:-$(nproc)}
	TIMEOUT=${TIMEOUT:-300}

	KDIR=${KERNEL_SRC:-}
	if [ -z "$KDIR" ]; then
		for d in "$repo"/linux-*/; do
			if [ -f "${d}Makefile" ] && [ -d "${d}fs" ]; then
				KDIR=${d%/}
				break
			fi
		done
	fi
	if [ -z "$KDIR" ] || [ ! -f "$KDIR/Makefile" ]; then
		echo "run-tests: no kernel source tree found; set KERNEL_SRC=/path/to/linux" >&2
		exit 2
	fi
	echo "run-tests: kernel tree: $KDIR"

	work=$(mktemp -d)
	trap 'rm -rf "$work"' EXIT

	# 1. Wire aufs-ng into the tree (idempotent; the README's documented
	#    integration, with the overlayfs entries as anchors).
	mkdir -p "$KDIR/fs/aufs-ng"
	cp "$repo"/*.c "$repo"/*.h "$repo"/Kconfig "$repo"/Makefile "$KDIR/fs/aufs-ng/" || exit
	grep -q 'fs/aufs-ng/Kconfig' "$KDIR/fs/Kconfig" ||
		sed -i '/source "fs\/overlayfs\/Kconfig"/a source "fs/aufs-ng/Kconfig"' "$KDIR/fs/Kconfig"
	grep -q 'aufs-ng/' "$KDIR/fs/Makefile" ||
		sed -i '/obj-\$(CONFIG_OVERLAY_FS)\s*+= overlayfs\//a obj-$(CONFIG_AUFSNG_FS)\t+= aufs-ng/' "$KDIR/fs/Makefile"

	# 2. Configure for ARCH=um with aufs-ng and the debug instrumentation
	#    the suite relies on (lockdep also implies PROVE_RCU).  An
	#    existing UML config with aufs-ng enabled is reused so repeated
	#    runs are incremental; anything else is reset.
	if ! grep -qs '^CONFIG_UML=y' "$KDIR/.config" ||
	   ! grep -qs '^CONFIG_AUFSNG_FS=y' "$KDIR/.config"; then
		if [ -f "$KDIR/.config" ]; then
			echo "run-tests: existing .config is not a UML+aufs-ng config, resetting the tree"
			make -C "$KDIR" mrproper >/dev/null || exit
		fi
		make -C "$KDIR" ARCH=um defconfig >/dev/null || exit
		"$KDIR/scripts/config" --file "$KDIR/.config" \
			--enable AUFSNG_FS \
			--enable TMPFS \
			--enable PROVE_LOCKING \
			--enable DEBUG_ATOMIC_SLEEP || exit
	fi
	make -C "$KDIR" ARCH=um olddefconfig >/dev/null || exit

	echo "run-tests: building UML kernel (-j$JOBS)..."
	make -C "$KDIR" ARCH=um -j"$JOBS" >"$work/build.log" 2>&1 || {
		echo "run-tests: kernel build FAILED; last lines:" >&2
		tail -25 "$work/build.log" >&2
		exit 1
	}

	# 3. Stage the guest pieces side by side: this very script (so it
	#    re-runs as the guest via the dispatch below) and the compiled
	#    helper it locates through its own dirname.
	aufsng_write_helper "$work/mnt.c"
	gcc -O2 -o "$work/mnt" "$work/mnt.c" || exit
	cp "$self" "$work/run-tests.sh"
	chmod +x "$work/run-tests.sh"

	# 4. Boot the guest with this script as init and the sentinel set,
	#    so the dispatch takes the guest path.  The full console is
	#    captured to a log for judging, while the live view is filtered
	#    to just the section headers, per-check progress, and summary.
	log="$work/guest.log"
	echo "run-tests: booting UML guest (timeout ${TIMEOUT}s)..."
	timeout -k 5 "$TIMEOUT" "$KDIR/linux" mem=1024M rootfstype=hostfs rw \
		init="$work/run-tests.sh" AUFSNG_TEST_GUEST=1 2>&1 \
		| tee "$log" \
		| grep --line-buffered -E '^===|^[0-9]+/[0-9]+ |^RESULT:'

	# 5. Judge: the suite must have completed, with zero failures and a
	#    kernel log free of lockdep/RCU/BUG findings.
	status=0
	if ! grep -q "TESTS-COMPLETE" "$log"; then
		echo "run-tests: suite did NOT run to completion; last lines:" >&2
		tail -30 "$log" >&2
		status=1
	fi
	fails=$(sed -n 's/.*RESULT: PASS=[0-9]* FAIL=\([0-9]*\).*/\1/p' "$log" | tail -1)
	if [ "${fails:-1}" != 0 ]; then
		echo "run-tests: ${fails:-?} check(s) FAILED:" >&2
		grep "TEST-FAIL:" "$log" | sed 's/^/  /' >&2
		status=1
	fi
	warnpat='possible (recursive|circular) locking|\*\*\* DEADLOCK|BUG:|use-after-free|sleeping function called|suspicious RCU usage|WARNING: .* at .*\.c'
	if grep -qE "$warnpat" "$log"; then
		echo "run-tests: kernel log contains debug findings:" >&2
		grep -E "$warnpat" "$log" | head -10 >&2
		status=1
	fi

	if [ "$status" = 0 ]; then
		echo "run-tests: OK - all checks passed, kernel log clean"
	else
		cp "$log" "$repo/tests-guest.log" 2>/dev/null &&
			echo "run-tests: full guest log kept at tests-guest.log" >&2
	fi
	exit $status
}

# ====================================================================
# DISPATCH - guest side only when the UML boot line set the sentinel;
# every normal invocation takes the host side.
# ====================================================================
if [ -n "${AUFSNG_TEST_GUEST:-}" ]; then
	guest_main
else
	host_main "$@"
fi
