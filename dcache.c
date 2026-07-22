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
 *    whiteout) created out-of-band in the rw branch directory;
 *  - a cached lower-only positive dentry keeps resolving an old branch
 *    after a runtime branch add handed the name to a higher-priority
 *    one (directory inodes are spliced in place by dynlayer.c, but a
 *    non-directory's winning branch can only change via re-lookup).
 *
 * The first is a per-branch rescan (aufsng_lookup_negative_valid); the
 * second is free, because the pinned real dentries share the branch
 * filesystem's own dcache, so the out-of-band edit unhashes the very
 * dentry cached here.  The third and fourth need real branch lookups,
 * gated by two INDEPENDENT signals so neither triggers the other's
 * probe: d_fsdata carries the upper dir's mtime/iversion stamp (gates
 * the upper probe), and the inode's origin_gen carries the branch
 * generation (gates the winning-branch re-check) - an ordinary write
 * in a directory must not send every cached lower-only sibling on an
 * all-branch rescan.  Each probe runs once per observed change, not
 * once per access.  Positive revalidation also re-syncs the union
 * inode's permission-relevant attributes with the real inode, so an
 * out-of-band chmod/chown is enforced (there is no .permission hook;
 * the VFS checks the union inode's cached mode, while getattr already
 * passes through - without the re-sync the two disagree forever).
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

/*
 * The d_fsdata stamp a child of @dir is revalidated against: the upper
 * dir's change signal (out-of-band creates/whiteouts, udba=reval
 * only).  The branch-change signal is deliberately NOT folded in - it
 * lives in the inode's origin_gen, so the two probes stay
 * independently gated.  Also used by lookup to prime a fresh dentry
 * with the state it just resolved against.
 */
unsigned long aufsng_reval_stamp(struct aufsng_fs *pfs, struct inode *dir)
{
	struct dentry *pupper = dir ? aufsng_upperdentry(dir) : NULL;

	return aufsng_udba_reval(pfs) && pupper ?
	       aufsng_dir_stamp(d_inode(pupper)) : 0;
}

/*
 * Re-sync the union inode's permission-relevant attributes with the
 * real inode when they drifted (out-of-band chmod/chown, or a branch
 * change that swapped the top real object).
 */
static int aufsng_attrs_valid(struct inode *inode, struct inode *real,
			      unsigned int flags)
{
	if (inode->i_mode == real->i_mode &&
	    uid_eq(inode->i_uid, real->i_uid) &&
	    gid_eq(inode->i_gid, real->i_gid))
		return 1;
	if (flags & LOOKUP_RCU)
		return -ECHILD;
	aufsng_copyattr(inode);
	return 1;
}

static int aufsng_positive_valid(struct inode *dir, const struct qstr *name,
			      struct dentry *dentry, unsigned int flags)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	struct inode *inode = d_inode(dentry);
	struct dentry *upper = aufsng_upperdentry(inode);
	struct aufsng_entry *oe = AUFSNG_I_E(inode);
	struct aufsng_path origin = { NULL, NULL };
	const struct cred *old_cred;
	bool reval = aufsng_udba_reval(pfs);
	struct dentry *pupper;
	struct dentry *this;
	unsigned long stamp;
	bool stamp_hit, gen_hit;
	u64 gen;
	int ret = 1;
	int found;
	int wh = 0;

	/*
	 * An out-of-band unlink/rename of the real entry unhashes the
	 * pinned real dentry itself - detectable without any lookup,
	 * also in RCU walk.  All out-of-band signals (this, the attr
	 * re-sync, the upper-dir probe below) are udba=reval only; the
	 * branch-change signal further down applies in every mode, since
	 * a runtime branch add is the union's own operation.
	 */
	if (upper) {
		if (!reval)
			return 1;	/* upper-backed: no branch outranks it */
		if (d_unhashed(upper) || d_is_negative(upper))
			return 0;
		return aufsng_attrs_valid(inode, d_inode(upper), flags);
	}
	if (reval && oe && oe->numlower) {
		struct dentry *lower = oe->lowerstack[0].dentry;

		if (d_unhashed(lower) || d_is_negative(lower))
			return 0;
		ret = aufsng_attrs_valid(inode, d_inode(lower), flags);
		if (ret < 0)
			return ret;
		ret = 1;
	}

	/*
	 * Lower-only object: two signals can invalidate it, each gating
	 * its own probe.  An upper entry or whiteout created out-of-band
	 * in the rw branch would shadow or hide it (the upper dir's
	 * mtime/iversion stamp in d_fsdata, udba=reval only), and a
	 * runtime branch add can hand the name to a higher-priority
	 * branch (branch_gen vs. the inode's origin_gen; directories
	 * are exempt - the splice keeps them right or unhashes them).
	 * Each probe runs once per observed change, not once per access.
	 */
	if (!dir)
		return 1;
	pupper = aufsng_upperdentry(dir);
	stamp = aufsng_reval_stamp(pfs, dir);
	gen = atomic64_read(&pfs->branch_gen);
	stamp_hit = (unsigned long)READ_ONCE(dentry->d_fsdata) == stamp;
	gen_hit = S_ISDIR(inode->i_mode) ||
		  READ_ONCE(AUFSNG_I(inode)->origin_gen) == gen;
	if (stamp_hit && gen_hit)
		return 1;
	if (flags & LOOKUP_RCU)
		return -ECHILD;

	old_cred = override_creds(pfs->creator_cred);
	down_read(&pfs->dyn_lock);

	if (!stamp_hit && reval && pupper) {
		this = aufsng_lookup_once(aufsng_upper_mnt(pfs), pupper, name,
				       &wh);
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
			goto out_stamp;
		}
	}

	/*
	 * No upper claim.  Directory stacks are spliced in place (or the
	 * dentry unhashed when a new branch hides them) by branch
	 * changes (dynlayer.c), so a directory dentry stays right; for a
	 * non-directory whose generation moved, re-run the merge's
	 * "which branch wins this name" decision - a branch added above
	 * the cached origin (or a whiteout it carries) means this dentry
	 * resolves the wrong object now and must be dropped for a fresh
	 * lookup.
	 */
	if (!gen_hit) {
		found = aufsng_find_origin(AUFSNG_I_E(dir), name, &origin);
		if (found < 0)
			goto out;	/* error: keep the dentry, re-probe later */
		if (!found) {
			ret = 0;	/* vanished or whited out */
			goto out;
		}
		ret = oe && oe->numlower &&
		      d_inode(origin.dentry) ==
		      d_inode(oe->lowerstack[0].dentry);
		dput(origin.dentry);
	}

out_stamp:
	if (ret) {
		WRITE_ONCE(dentry->d_fsdata, (void *)stamp);
		WRITE_ONCE(AUFSNG_I(inode)->origin_gen, gen);
	}
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
	 * for everything out-of-band, matching AUFS; the union's own
	 * branch changes are still honored by the positive path (its
	 * negative counterpart is aufsng_dyn_drop_neg_children(), which
	 * the branch-change paths run in every udba mode).
	 */
	if (unlikely(IS_ROOT(dentry)))
		return 1;

	if (d_is_negative(dentry)) {
		if (!aufsng_udba_reval(pfs) || !dir)
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
