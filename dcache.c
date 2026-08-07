// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dentry revalidation.  Branch add/remove updates the stack surgically
 * (dynlayer.c), so what can go stale is the relation to a branch edited
 * out of band (udba=reval), plus one union-own case:
 *
 *  - a cached negative hides a name a branch now provides;
 *  - a cached positive serves an object whose real entry was unlinked
 *    (free: the branch fs unhashes the very dentry pinned here);
 *  - a lower-only positive misses an upper entry or whiteout created
 *    out of band;
 *  - a lower-only positive keeps an old branch after an add gave the
 *    name to a higher-priority one.
 *
 * The last two probe the branches, gated by two INDEPENDENT signals so
 * neither fires the other's probe: d_fsdata holds the upper dir's
 * change stamp, d_time the branch generation.  Each probe runs once
 * per observed change.  Positive revalidation also re-syncs the union
 * inode's mode/uid/gid: there is no .permission hook, so without it an
 * out-of-band chmod is never enforced.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/iversion.h>
#include <linux/cred.h>
#include <linux/rcupdate.h>
#include "aufsng.h"

/* fold the upper dir's change signal into one d_fsdata-sized stamp */
static unsigned long aufsng_dir_stamp(struct inode *dir)
{
	struct timespec64 mtime = inode_get_mtime(dir);

	/* tv_nsec needs 30 bits; a smaller shift overlaps the two fields
	 * and makes distinct (sec, nsec) pairs collide into one stamp */
	return (unsigned long)(inode_query_iversion(dir) ^
			       ((u64)mtime.tv_sec << 30) ^ mtime.tv_nsec);
}

/*
 * The d_fsdata stamp a child of @dir is revalidated against.  The
 * branch-change signal is deliberately not folded in: it lives in
 * d_time, keeping the two probes independently gated.
 */
unsigned long aufsng_reval_stamp(struct aufsng_fs *pfs, struct inode *dir)
{
	struct dentry *pupper = dir ? aufsng_upperdentry(dir) : NULL;

	return aufsng_udba_reval(pfs) && pupper ?
	       aufsng_dir_stamp(d_inode(pupper)) : 0;
}

/* Re-sync mode/uid/gid with the real inode when they drifted */
static int aufsng_attrs_valid(struct inode *inode, struct inode *real,
			      unsigned int flags)
{
	if (inode->i_mode == real->i_mode &&
	    uid_eq(inode->i_uid, real->i_uid) &&
	    gid_eq(inode->i_gid, real->i_gid))
		return 1;
	if (flags & LOOKUP_RCU)
		return -ECHILD;
	/* @real is what aufsng_path_real() would resolve; don't re-derive it */
	aufsng_copyattr_from(inode, real);
	return 1;
}

/*
 * Does @upper carry @name in @dir's upper directory?  "The inode has an
 * upper" is per-INODE (hardlink siblings share it); revalidation needs
 * the per-NAME answer.
 *
 * d_same_name() under RCU, not a hand-rolled len + memcmp: nothing
 * locks @upper, so a concurrent d_move() can swap d_name mid-compare
 * and kfree_rcu() the old external name.  d_same_name() survives that
 * and honors the branch fs's own ->d_compare.
 */
static bool aufsng_upper_carries_name(struct inode *dir, struct dentry *upper,
				      const struct qstr *name)
{
	struct dentry *pupper = aufsng_upperdentry(dir);
	bool same;

	if (!pupper || READ_ONCE(upper->d_parent) != pupper)
		return false;

	rcu_read_lock();
	same = d_same_name(upper, pupper, name);
	rcu_read_unlock();
	return same;
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
	unsigned long gen;
	int ret = 1;
	int found;
	int wh = 0;

	/*
	 * An out-of-band unlink/rename unhashes the pinned real dentry -
	 * no lookup needed, RCU walk included.  Out-of-band signals are
	 * udba=reval only; the branch-change signal below is not.
	 */
	if (upper) {
		if (!reval)
			return 1;	/* upper-backed: no branch outranks it */
		if (!aufsng_dentry_alive(upper))
			return 0;
		/*
		 * Per-NAME: the shared inode's upper may carry a different
		 * name (a hardlink sibling).  A sibling name is still
		 * lower-provided and must run the gates below, or a branch
		 * change whiting it out is never seen.
		 */
		if (!dir || aufsng_upper_carries_name(dir, upper, name))
			return aufsng_attrs_valid(inode, d_inode(upper), flags);
	}
	if (reval && !upper && oe && oe->numlower) {
		struct dentry *lower = oe->lowerstack[0].dentry;

		if (!aufsng_dentry_alive(lower))
			return 0;
		if (aufsng_attrs_valid(inode, d_inode(lower), flags) < 0)
			return -ECHILD;
	}

	/*
	 * Lower-only: two signals, each gating its own probe.  An upper
	 * entry or whiteout created out of band would shadow it (d_fsdata
	 * stamp, udba=reval only); a branch add can give the name to a
	 * higher-priority branch (branch_gen vs. d_time).
	 *
	 * Directories run the generation re-check too.  The in-place
	 * splice normally keeps them right, but it legitimately skips a
	 * directory on its fallbacks while branch_gen moves anyway;
	 * exempting them made every skip permanent.  The stamp is
	 * per-DENTRY: a branch can hide one hardlink sibling name only.
	 */
	if (!dir)
		return 1;
	pupper = aufsng_upperdentry(dir);
	stamp = aufsng_reval_stamp(pfs, dir);
	gen = atomic_long_read(&pfs->branch_gen);
	stamp_hit = (unsigned long)READ_ONCE(dentry->d_fsdata) == stamp;
	gen_hit = READ_ONCE(dentry->d_time) == gen;
	if (stamp_hit && gen_hit)
		return 1;
	if (flags & LOOKUP_RCU)
		return -ECHILD;

	old_cred = override_creds(pfs->creator_cred);
	percpu_down_read(&pfs->dyn_lock);

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
			 * upper is adopted in place, which also preserves
			 * submounts a d_invalidate() would detach.  Not
			 * adopted, and dropped so a fresh lookup rebuilds the
			 * view: a different type, an OPAQUE directory (lookups
			 * hide every lower, an adopt would keep this dir's
			 * lower stack alive), and an upper claiming no copy-up
			 * origin (it appeared out of band, so the cached
			 * inode's key is not the one a fresh lookup and
			 * readdir's d_ino would use).
			 */
			bool adopt = aufsng_origin_type_ok(this, inode->i_mode);

			if (adopt && d_is_dir(this)) {
				int opq = aufsng_check_diropq(aufsng_upper_mnt(pfs),
							   this);

				if (opq < 0) {
					/* error: keep, re-probe later */
					dput(this);
					goto out;
				}
				adopt = !opq;
			} else if (adopt) {
				/*
				 * Open-coded for directories above so a
				 * failed probe can keep the dentry.
				 */
				adopt = aufsng_upper_claims_origin(pfs, this,
								name);
			}
			if (adopt)
				ret = aufsng_dyn_adopt_upper(inode,
							  oe && oe->numlower ?
							  oe->lowerstack[0].dentry :
							  NULL,
							  this);
			else
				ret = 0;
			dput(this);
			goto out_stamp;
		}
	}

	/*
	 * The generation moved: re-run the "which branch wins this name"
	 * decision.  A branch added above the cached origin, or a whiteout
	 * it carries, means this dentry resolves the wrong object.  For a
	 * directory the compare normally just passes and re-stamps; a
	 * mismatch means the splice skipped it, and dropping is the heal.
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
	if (ret)
		aufsng_store_reval_stamps(dentry, stamp, gen);
out:
	percpu_up_read(&pfs->dyn_lock);
	revert_creds(old_cred);
	return ret;
}

static int aufsng_d_revalidate(struct inode *dir, const struct qstr *name,
			    struct dentry *dentry, unsigned int flags)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);

	/*
	 * The root is never stale: its entry is swapped in place on a
	 * branch change.  udba=none trusts the cache out of band, as
	 * AUFS; the union's own branch changes are still honored.
	 */
	if (unlikely(IS_ROOT(dentry)))
		return 1;

	if (d_is_negative(dentry)) {
		unsigned long stamp;
		unsigned long gen;

		if (!aufsng_udba_reval(pfs) || !dir)
			return 1;
		/*
		 * Same two signals as the positive path.  Without the gate
		 * every access to a cached miss pays an upper probe plus a
		 * full lower-stack scan.
		 */
		stamp = aufsng_reval_stamp(pfs, dir);
		gen = atomic_long_read(&pfs->branch_gen);
		if (aufsng_reval_stamps_match(dentry, stamp, gen))
			return 1;
		if (flags & LOOKUP_RCU)
			return -ECHILD;
		if (!aufsng_lookup_negative_valid(dir, name))
			return 0;
		aufsng_store_reval_stamps(dentry, stamp, gen);
		return 1;
	}

	return aufsng_positive_valid(dir, name, dentry, flags);
}

const struct dentry_operations aufsng_dentry_operations = {
	.d_revalidate		= aufsng_d_revalidate,
};
