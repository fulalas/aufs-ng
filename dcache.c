// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng dentry operations: layer-generation revalidation.
 *
 * Every dentry carries the layer stack generation it was instantiated
 * against in the high bits of d_fsdata (inherited from its parent);
 * the low bits hold AUFSNG_D_* flags.  A dentry whose generation does
 * not match pfs->dyn_gen may hide or wrongly resolve names provided
 * by the current stack: it fails revalidation and is looked up again
 * through the fresh root.  The generation is only ever bumped when a
 * layer change cannot update the cached state surgically, so this is
 * the rare path, not the common one.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include "aufsng.h"

/*
 * Restamp the generation bits of d_fsdata, preserving the flag bits
 * which may be modified concurrently with set_bit/clear_bit.
 */
void aufsng_dentry_restamp_gen(struct dentry *dentry, unsigned long gen)
{
	unsigned long *p = (unsigned long *)&dentry->d_fsdata;
	unsigned long old, new;

	old = READ_ONCE(*p);
	do {
		new = (gen << AUFSNG_D_GEN_SHIFT) | (old & AUFSNG_D_FLAGS_MASK);
	} while (!try_cmpxchg(p, &old, new));
}

/* stamp a fresh dentry with its parent's generation */
void aufsng_dentry_init_gen(struct dentry *dentry)
{
	aufsng_dentry_restamp_gen(dentry, aufsng_dentry_gen(dentry->d_parent));
}

static int aufsng_dentry_revalidate_common(struct dentry *dentry,
					unsigned int flags, bool weak)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);

	/*
	 * The root is never stale: its aufsng_entry is swapped in place
	 * when the layer stack changes.  Real dentries never need
	 * revalidation either: remote filesystems are refused as
	 * layers at mount and layer-add time.
	 */
	if (unlikely(IS_ROOT(dentry)))
		return 1;

	if (likely(!aufsng_dentry_gen_stale(pfs, dentry)))
		return 1;

	/*
	 * Mounts sitting on a stale dentry, or reachable underneath one,
	 * must not be detached by d_invalidate(): it walks the whole
	 * subtree looking for mounts to lazily detach, and a branch
	 * change elsewhere in the tree is not a reason to tear down a
	 * user's unrelated mount several directories down.  A mounted
	 * descendant is only ever reachable through cached ancestors (a
	 * dentry always holds a reference on its parent), so a quick,
	 * lockless "does this directory still have cached children"
	 * check catches the whole chain up from any mountpoint below,
	 * not only a mountpoint at this exact dentry.
	 */
	if (d_mountpoint(dentry) || !hlist_empty(&dentry->d_children))
		return 1;
	/*
	 * Weak revalidation runs on fd-based re-resolution
	 * (/proc/self/fd magic links, AT_EMPTY_PATH, jumps at the end
	 * of a walk), where returning 0 surfaces as ESTALE to userspace
	 * and a retry jumps straight back to this same dentry - it can
	 * never self-heal through a fresh lookup.  Tolerate the older
	 * but self-consistent view instead.
	 */
	if (weak)
		return 1;
	if (flags & LOOKUP_RCU)
		return -ECHILD;
	return 0;
}

static int aufsng_d_revalidate(struct inode *dir, const struct qstr *name,
			    struct dentry *dentry, unsigned int flags)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	int ret;

	ret = aufsng_dentry_revalidate_common(dentry, flags, false);
	if (ret <= 0)
		return ret;

	/*
	 * udba=reval: the generation stamp only tracks the union's own
	 * branch add/remove, not a direct out-of-band edit of a branch.
	 * A negative dentry can therefore be stale after a whiteout is
	 * removed (or the name is created) straight in the rw branch, so
	 * re-check the branches and drop it if the name is back.  The
	 * check blocks on real lookups, so leave RCU walk first.
	 */
	if (aufsng_udba_reval(pfs) && dir && d_is_negative(dentry)) {
		if (flags & LOOKUP_RCU)
			return -ECHILD;
		if (!aufsng_lookup_negative_valid(dir, name))
			return 0;
	}
	return 1;
}

static int aufsng_d_weak_revalidate(struct dentry *dentry, unsigned int flags)
{
	return aufsng_dentry_revalidate_common(dentry, flags, true);
}

const struct dentry_operations aufsng_dentry_operations = {
	.d_revalidate		= aufsng_d_revalidate,
	.d_weak_revalidate	= aufsng_d_weak_revalidate,
};
