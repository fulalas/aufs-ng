// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng dentry revalidation.
 *
 * The branch stack itself is updated surgically on runtime branch
 * add/remove (see dynlayer.c), so cached dentries never go stale from
 * the union's own operations.  What CAN go stale under udba=reval is
 * the relationship to the real branches when one is edited directly,
 * out of band:
 *
 *  - a cached negative dentry hides a name a branch now provides
 *    (typically a ".wh.<name>" whiteout removed by hand in the rw
 *    branch);
 *  - a cached positive dentry keeps serving an object whose real
 *    upper/lower entry was unlinked or renamed away;
 *  - a cached lower-only positive dentry misses an upper entry (or
 *    whiteout) created out-of-band in the rw branch directory.
 *
 * The first is a per-branch rescan (aufsng_lookup_negative_valid); the
 * second is free, because the pinned real dentries share the branch
 * filesystem's own dcache, so the out-of-band edit unhashes the very
 * dentry cached here.  Only the third needs a real lookup in the upper
 * directory, so it is gated by an mtime/iversion stamp of that
 * directory cached in d_fsdata: the probe runs once per observed
 * upper-dir change, not once per access.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/iversion.h>
#include <linux/cred.h>
#include "aufsng.h"

/* fold the upper dir's change signal into one d_fsdata-sized stamp */
static unsigned long aufsng_dir_stamp(struct inode *dir)
{
	struct timespec64 mtime = inode_get_mtime(dir);

	return (unsigned long)(inode_query_iversion(dir) ^
			       ((u64)mtime.tv_sec << 20) ^ mtime.tv_nsec);
}

static int aufsng_positive_valid(struct inode *dir, const struct qstr *name,
			      struct dentry *dentry, unsigned int flags)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	struct inode *inode = d_inode(dentry);
	struct dentry *upper = aufsng_upperdentry(inode);
	struct aufsng_entry *oe = AUFSNG_I_E(inode);
	const struct cred *old_cred;
	struct dentry *pupper;
	struct dentry *this;
	unsigned long stamp;
	int ret = 1;
	int wh = 0;

	/*
	 * An out-of-band unlink/rename of the real entry unhashes the
	 * pinned real dentry itself - detectable without any lookup,
	 * also in RCU walk.
	 */
	if (upper)
		return !(d_unhashed(upper) || d_is_negative(upper));
	if (oe && oe->numlower &&
	    (d_unhashed(oe->lowerstack[0].dentry) ||
	     d_is_negative(oe->lowerstack[0].dentry)))
		return 0;

	/*
	 * Lower-only object: an upper entry or whiteout created
	 * out-of-band in the rw branch would shadow or hide it.  Probe
	 * the upper dir, but only when its stamp says it changed since
	 * the last probe recorded in d_fsdata.
	 */
	pupper = dir ? aufsng_upperdentry(dir) : NULL;
	if (!pupper)
		return 1;
	stamp = aufsng_dir_stamp(d_inode(pupper));
	if ((unsigned long)READ_ONCE(dentry->d_fsdata) == stamp)
		return 1;
	if (flags & LOOKUP_RCU)
		return -ECHILD;

	old_cred = override_creds(pfs->creator_cred);
	down_read(&pfs->dyn_lock);

	this = aufsng_lookup_once(aufsng_upper_mnt(pfs), pupper, name, &wh);
	if (IS_ERR(this))
		goto out;	/* error: keep the dentry, re-probe later */
	if (wh) {
		ret = 0;	/* whited out: the name is deleted now */
		goto out;
	}
	if (this) {
		/*
		 * The rw branch now provides the name.  A same-type
		 * upper is adopted in place - this also refreshes a
		 * directory's merged listing and preserves submounts
		 * that a d_invalidate() would detach.  A different type
		 * means the cached inode is the wrong object: drop it.
		 */
		if ((d_inode(this)->i_mode ^ inode->i_mode) & S_IFMT)
			ret = 0;
		else
			ret = aufsng_dyn_adopt_upper(inode,
						  oe && oe->numlower ?
						  oe->lowerstack[0].dentry :
						  NULL,
						  this);
		dput(this);
	}
	if (ret)
		WRITE_ONCE(dentry->d_fsdata, (void *)stamp);
out:
	up_read(&pfs->dyn_lock);
	revert_creds(old_cred);
	return ret;
}

static int aufsng_d_revalidate(struct inode *dir, const struct qstr *name,
			    struct dentry *dentry, unsigned int flags)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);

	/*
	 * The root is never stale: its aufsng_entry is swapped in place
	 * when the layer stack changes.  udba=none trusts the cache
	 * entirely, matching AUFS.
	 */
	if (unlikely(IS_ROOT(dentry)))
		return 1;
	if (!aufsng_udba_reval(pfs))
		return 1;

	if (d_is_negative(dentry)) {
		if (!dir)
			return 1;
		if (flags & LOOKUP_RCU)
			return -ECHILD;
		return aufsng_lookup_negative_valid(dir, name) ? 1 : 0;
	}

	return aufsng_positive_valid(dir, name, dentry, flags);
}

const struct dentry_operations aufsng_dentry_operations = {
	.d_revalidate		= aufsng_d_revalidate,
};
