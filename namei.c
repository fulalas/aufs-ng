// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng layered lookup, AUFS-compatible whiteout semantics.
 *
 * Unlike overlayfs (where a whiteout occupies the SAME name as the
 * file it hides), AUFS marks a name deleted in a branch with a
 * SEPARATE sibling file named ".wh.<name>" (a plain 0444 regular
 * file, verified against fs/aufs/whout.c in the upstream source).  A
 * branch lookup for "foo" is therefore always paired with a lookup
 * for ".wh.foo" in the same parent: if the whiteout is found, "foo"
 * is considered deleted from that branch downward and the search
 * stops; only then is "foo" itself looked up.  A directory is
 * "opaque" (nothing below it in lower branches is visible) if it
 * contains a file named ".wh..wh..opq".
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include "aufsng.h"

/*
 * Probe @dir (in @mnt) for the AUFS bookkeeping marker @name.
 * Returns 1 if present as a regular file, 0 if absent, negative errno
 * on lookup error.  @strict maps a positive non-regular occupant to
 * -EIO (a corrupt marker must fail the operation) instead of treating
 * it as absent.
 */
static int aufsng_probe_marker(struct vfsmount *mnt, struct dentry *dir,
			    struct qstr *name, bool strict)
{
	struct dentry *whd;
	int ret = 0;

	whd = lookup_one_unlocked(mnt_idmap(mnt), name, dir);
	if (IS_ERR(whd))
		return PTR_ERR(whd);

	if (d_is_positive(whd))
		ret = d_is_reg(whd) ? 1 : (strict ? -EIO : 0);
	dput(whd);
	return ret;
}

/*
 * Does @parent (in @mnt) contain a whiteout for @name?  Returns 1 if
 * yes, 0 if no, negative errno on error.  This is a lookup for the
 * SIBLING name ".wh.<name>", not a property of @name's own dentry.
 */
int aufsng_check_whiteout(struct vfsmount *mnt, struct dentry *parent,
		       const struct qstr *name)
{
	char buf[NAME_MAX + 1];
	struct qstr wh;
	int ret;

	/*
	 * A name too long to have a ".wh." sibling at all cannot be
	 * whited out - the branch fs could never store the marker - so
	 * the probe answers "no whiteout" rather than failing lookups
	 * of legally existing long lower names with ENAMETOOLONG.
	 * (Deleting such a name still fails in aufsng_create_whiteout(),
	 * as it does on real AUFS.)
	 */
	ret = aufsng_wh_name(buf, name, &wh);
	if (ret)
		return 0;

	/* a corrupt (non-regular) whiteout entry is -EIO, not "absent" */
	return aufsng_probe_marker(mnt, parent, &wh, true);
}

/* is @dir (in @mnt) marked opaque? */
int aufsng_check_diropq(struct vfsmount *mnt, struct dentry *dir)
{
	struct qstr opq = QSTR(AUFSNG_WH_DIROPQ);

	return aufsng_probe_marker(mnt, dir, &opq, false);
}

/*
 * Look up @name in one real branch directory.  Returns NULL for a
 * negative result, the referenced dentry for a positive one, or
 * (via *whiteout) 1 if this branch whites the name out (in which
 * case NULL is always returned and the search must stop here).
 */
struct dentry *aufsng_lookup_once(struct vfsmount *mnt,
			       struct dentry *base,
			       const struct qstr *name,
			       int *whiteout)
{
	struct qstr q = QSTR_LEN(name->name, name->len);
	struct dentry *this;
	int wh;

	wh = aufsng_check_whiteout(mnt, base, name);
	if (wh < 0)
		return ERR_PTR(wh);
	if (wh) {
		*whiteout = 1;
		return NULL;
	}

	this = lookup_one_unlocked(mnt_idmap(mnt), &q, base);
	if (IS_ERR(this))
		return this;
	if (d_is_negative(this)) {
		dput(this);
		return NULL;
	}
	return this;
}

/*
 * Find the topmost lower branch entry still visibly providing @name
 * under the parent stack @poe: the copy-up origin of an upper object,
 * and the entry that would resurface if the upper name were removed.
 * Returns 1 with a reference in @out, 0 if no lower provides the name
 * (absent or whited out), negative errno on error.  Caller must hold
 * pfs->dyn_lock (any mode).
 */
int aufsng_find_origin(struct aufsng_entry *poe, const struct qstr *name,
		    struct aufsng_path *out)
{
	unsigned int i;

	for (i = 0; poe && i < poe->numlower; i++) {
		struct aufsng_path *lower = &poe->lowerstack[i];
		struct dentry *this;
		int wh = 0;

		this = aufsng_lookup_once(lower->layer->mnt, lower->dentry,
				       name, &wh);
		if (IS_ERR(this))
			return PTR_ERR(this);
		if (wh)
			return 0;
		if (this) {
			out->layer = lower->layer;
			out->dentry = this;
			return 1;
		}
	}
	return 0;
}

/*
 * Does any lower branch of @dir still visibly provide @name?  The
 * boolean form of aufsng_find_origin() for callers that only need the
 * verdict, not the origin itself.  Returns 1/0, negative errno on
 * error - each call site decides its own error policy (mutations must
 * treat an error as fatal, or a delete silently skips its whiteout;
 * revalidation keeps the dentry instead of thrashing it).
 *
 * A caller pairing this verdict with a mutation must hold
 * pfs->dyn_lock across BOTH: a branch spliced in or out between the
 * two would make the verdict stale (a deleted name would resurrect
 * uncovered, or a stray whiteout would mask nothing).  Must run under
 * credentials able to search the branch dirs (creator creds).
 */
int aufsng_lower_covers(struct inode *dir, const struct qstr *name)
{
	struct aufsng_path origin;
	int found;

	found = aufsng_find_origin(AUFSNG_I_E(dir), name, &origin);
	if (found > 0)
		dput(origin.dentry);
	return found;
}

/*
 * udba=reval negative-dentry revalidation.  A cached negative name may
 * have been resurrected by a direct branch change the union never saw
 * (typically a ".wh.<name>" whiteout removed by hand in the rw branch).
 * Re-run the merge's "does any branch still provide this name" decision
 * for @name under parent @dir: return true if it is still absent (the
 * negative dentry remains valid), false if some branch now provides it
 * (the dentry must be dropped and looked up again).  On error, keep the
 * dentry valid rather than thrash it.  Caller must not be in RCU walk.
 */
bool aufsng_lookup_negative_valid(struct inode *dir, const struct qstr *name)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dir->i_sb);
	const struct cred *old_cred;
	struct dentry *pupper;
	struct dentry *this;
	bool valid = true;
	int wh;

	old_cred = override_creds(pfs->creator_cred);
	down_read(&pfs->dyn_lock);

	pupper = aufsng_upperdentry(dir);
	/*
	 * No upper: whiteouts live only in the rw branch, and the lowers are
	 * read-only, so a name absent when this negative dentry was created
	 * stays absent until the branch set changes - and both add and remove
	 * drop affected negative children through the dynamic path (a removed
	 * branch may have carried a whiteout that hid a lower-priority name,
	 * so removal calls aufsng_dyn_drop_neg_children() too).  A surviving
	 * negative is therefore still absent: skip the per-branch rescan.
	 * This is the common case for read-only system directories, where
	 * repeated misses (PATH, include and library probes) would otherwise
	 * scan every lower.
	 */
	if (!pupper)
		goto out;

	wh = 0;
	this = aufsng_lookup_once(aufsng_upper_mnt(pfs), pupper, name, &wh);
	if (IS_ERR(this) || wh)
		goto out;	/* error, or still whited out: valid */
	if (this) {
		dput(this);
		valid = false;		/* the upper now provides the name */
		goto out;
	}

	/*
	 * Same "does any lower still provide this name" decision the
	 * merge itself makes; errors keep the dentry valid rather than
	 * thrash it.
	 */
	if (aufsng_lower_covers(dir, name) > 0)
		valid = false;	/* a lower branch now provides it */

out:
	up_read(&pfs->dyn_lock);
	revert_creds(old_cred);
	return valid;
}

static int aufsng_inode_test(struct inode *inode, void *data)
{
	return inode->i_private == data;
}

static int aufsng_inode_set(struct inode *inode, void *data)
{
	inode->i_private = data;
	return 0;
}

static void aufsng_fill_inode(struct inode *inode, struct inode *realinode)
{
	inode->i_rdev = realinode->i_rdev;
	aufsng_copyattr_from(inode, realinode);

	switch (inode->i_mode & S_IFMT) {
	case S_IFDIR:
		inode->i_op = &aufsng_dir_inode_operations;
		inode->i_fop = &aufsng_dir_operations;
		break;
	case S_IFLNK:
		inode->i_op = &aufsng_symlink_inode_operations;
		break;
	case S_IFREG:
		inode->i_op = &aufsng_file_inode_operations;
		inode->i_fop = &aufsng_file_operations;
		break;
	default:
		inode->i_op = &aufsng_special_inode_operations;
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	}
}

/*
 * Find or create the union inode for a resolved branch stack.  The
 * inode hash key is the top lower inode when a lower exists (stable
 * across copy-up), the rw-branch inode for pure-rw files.  Consumes
 * @upperdentry and @oe on both success and failure.  Callers must hold
 * pfs->dyn_lock (read): the upper-adoption heal parks the superseded
 * upper on dyn_parked, which is otherwise mutated only under dyn_lock
 * for writing (aufsng_dyn_commit_rebuild).
 */
struct inode *aufsng_get_inode(struct super_block *sb,
			    struct dentry *upperdentry,
			    struct aufsng_entry *oe)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);
	struct inode *realinode;
	struct inode *key;
	struct inode *inode;
	unsigned int key_idx;

	realinode = upperdentry ? d_inode(upperdentry) :
				  d_inode(oe->lowerstack[0].dentry);
	key = oe->numlower ? d_inode(oe->lowerstack[0].dentry) :
			     d_inode(upperdentry);
	key_idx = oe->numlower ?
		  aufsng_layer_idx(pfs, oe->lowerstack[0].layer) : 0;

	inode = iget5_locked(sb, (unsigned long)key, aufsng_inode_test,
			     aufsng_inode_set, key);
	if (!inode) {
		dput(upperdentry);
		aufsng_free_entry(oe);
		return ERR_PTR(-ENOMEM);
	}

	if (!(inode_state_read_once(inode) & I_NEW)) {
		struct dentry *cached = aufsng_upperdentry(inode);
		struct inode *cached_u = cached ? d_inode(cached) : NULL;
		struct inode *new_u = upperdentry ? d_inode(upperdentry) : NULL;
		bool ok = cached_u == new_u;

		/*
		 * Upper mismatches on a shared hash key are usually
		 * healable, not fatal.  This lookup found no upper while
		 * the cached inode has one: a copy-up racing this
		 * lookup, or a lower hardlink sibling of a copied-up
		 * name - the cached state wins (matching AUFS, where
		 * pseudo-links keep such names on one inode), but ONLY
		 * while the cached upper is still alive.  A dead one
		 * (unhashed by an out-of-band unlink/rename in the rw
		 * branch - the exact udba=reval scenario) must be shed
		 * instead: re-adopting it would serve the deleted upper's
		 * content forever, and an open for write would skip
		 * copy-up and write into the unlinked inode.  Shedding
		 * lets the lower resurface, as the reval contract
		 * promises.  This lookup found an upper the cached inode
		 * lacks, or a different one (an app that saves by
		 * writing a temp file and renaming it over the name
		 * gives the rw copy a new inode on every save, and the
		 * union inode - keyed by the stable lower origin - must
		 * follow it instead of failing with ESTALE): adopt it
		 * via the shared adopt-or-park machinery in dynlayer.c,
		 * which also handles type mismatches (not adopted, fall
		 * through to ESTALE) and parking the superseded upper
		 * for lockless aufsng_path_real() readers.
		 */
		if (!ok && !new_u && oe->numlower) {
			if (!d_unhashed(cached) && !d_is_negative(cached))
				ok = true;
			else
				ok = aufsng_dyn_shed_upper(inode);
		}
		if (!ok && new_u)
			ok = aufsng_dyn_adopt_upper(inode,
						 oe->numlower ?
						 oe->lowerstack[0].dentry :
						 NULL,
						 upperdentry);
		dput(upperdentry);
		aufsng_free_entry(oe);
		if (!ok) {
			iput(inode);
			return ERR_PTR(-ESTALE);
		}
		return inode;
	}

	aufsng_fill_inode(inode, realinode);
	inode->i_ino = aufsng_map_ino(key->i_ino, key_idx);
	AUFSNG_I(inode)->oe = oe;
	AUFSNG_I(inode)->upperdentry = upperdentry;
	unlock_new_inode(inode);

	return inode;
}

struct dentry *aufsng_lookup(struct inode *dir, struct dentry *dentry,
			  unsigned int flags)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	struct aufsng_entry *poe;
	struct aufsng_entry *oe = NULL;
	struct dentry *upper = NULL;
	struct dentry *this;
	struct dentry *pupper;
	struct inode *inode = NULL;
	const struct cred *old_cred;
	unsigned long stamp;
	u64 gen;
	unsigned int i;
	bool stopped = false;
	int wh, err = 0;

	/*
	 * ".wh."-prefixed names are AUFS bookkeeping: refusing them here
	 * (as real AUFS does) also blocks every way of creating one
	 * through the union, since creation needs the lookup to return
	 * a negative dentry first.  A user-created ".wh.<x>" would
	 * otherwise land on disk as a real file and permanently hide
	 * "<x>".
	 */
	if (aufsng_is_wh_name(dentry->d_name.name, dentry->d_name.len))
		return ERR_PTR(-EPERM);

	/*
	 * The parent's branch stack (and the root stack every lookup
	 * ultimately derives from) may be swapped by a runtime branch
	 * add/remove; exclude that for the whole lookup.
	 */
	down_read(&pfs->dyn_lock);
	old_cred = override_creds(pfs->creator_cred);

	poe = AUFSNG_E(dentry->d_parent);
	pupper = aufsng_upperdentry(dir);
	/* sampled before the branch probes; see the priming below */
	stamp = aufsng_reval_stamp(pfs, dir);
	gen = atomic64_read(&pfs->branch_gen);

	if (pupper) {
		wh = 0;
		this = aufsng_lookup_once(aufsng_upper_mnt(pfs), pupper,
				       &dentry->d_name, &wh);
		if (IS_ERR(this)) {
			err = PTR_ERR(this);
			goto out;
		}
		if (wh) {
			stopped = true;
		} else if (this) {
			upper = this;
			if (d_is_dir(upper)) {
				int opq = aufsng_check_diropq(aufsng_upper_mnt(pfs),
							   upper);
				if (opq < 0) {
					err = opq;
					goto out;
				}
				if (opq)
					stopped = true;
			}
		}
	}

	if (!stopped && upper && !d_is_dir(upper)) {
		/*
		 * A non-directory upper ends the merge, but its copy-up
		 * origin (the topmost lower still visibly providing this
		 * name) is still collected: the union inode is hashed by
		 * the origin, which keeps the inode - and st_ino -
		 * stable across copy-up and cache eviction.  A same-named
		 * lower of a *different* type is not an origin, though: the
		 * upper is then an independent object shadowing it (e.g. a
		 * symlink created over a lower regular file), and keying it
		 * onto the lower's identity would alias two unrelated
		 * objects of conflicting type - so it is keyed by its upper.
		 */
		struct aufsng_path origin = { NULL, NULL };
		int found;

		found = aufsng_find_origin(poe, &dentry->d_name, &origin);
		if (found < 0) {
			err = found;
			goto out;
		}
		if (found && !aufsng_origin_type_ok(origin.dentry,
						    d_inode(upper)->i_mode)) {
			dput(origin.dentry);
			origin.dentry = NULL;
			found = 0;
		}
		err = -ENOMEM;
		oe = aufsng_alloc_entry(found);
		if (!oe) {
			dput(origin.dentry);
			goto out;
		}
		err = 0;
		if (found)
			oe->lowerstack[0] = origin;
	} else if (!stopped && poe && poe->numlower) {
		for (i = 0; i < poe->numlower; i++) {
			struct aufsng_path *lower = &poe->lowerstack[i];

			wh = 0;
			this = aufsng_lookup_once(lower->layer->mnt,
					       lower->dentry, &dentry->d_name,
					       &wh);
			if (IS_ERR(this)) {
				err = PTR_ERR(this);
				goto out;
			}
			if (wh)
				break;
			if (!this)
				continue;
			/*
			 * Allocated on the first hit, not up front, so
			 * an all-negative lookup (PATH/include probes)
			 * allocates nothing; the remaining branch count
			 * still bounds the stack size.
			 */
			if (!oe) {
				oe = aufsng_alloc_entry(poe->numlower - i);
				if (!oe) {
					dput(this);
					err = -ENOMEM;
					goto out;
				}
				oe->numlower = 0;
			}
			/*
			 * Merging continues only through directories: a
			 * non-directory match is the end of the stack,
			 * and only counts at all if it is the top of it.
			 */
			if (!d_is_dir(this)) {
				if (!upper && !oe->numlower) {
					oe->lowerstack[0].layer = lower->layer;
					oe->lowerstack[0].dentry = this;
					oe->numlower = 1;
				} else {
					dput(this);
				}
				break;
			}
			oe->lowerstack[oe->numlower].layer = lower->layer;
			oe->lowerstack[oe->numlower].dentry = this;
			oe->numlower++;

			{
				int opq = aufsng_check_diropq(lower->layer->mnt,
							   this);
				if (opq < 0) {
					err = opq;
					goto out;
				}
				if (opq)
					break;
			}
		}
	}

	if (upper || (oe && oe->numlower)) {
		if (!oe) {
			err = -ENOMEM;
			oe = aufsng_alloc_entry(0);
			if (!oe)
				goto out;
			err = 0;
		}

		inode = aufsng_get_inode(dentry->d_sb, upper, oe);
		upper = NULL;
		oe = NULL;
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			inode = NULL;
			goto out;
		}
	}

out:
	revert_creds(old_cred);
	up_read(&pfs->dyn_lock);
	aufsng_free_entry(oe);
	dput(upper);
	if (err)
		return ERR_PTR(err);

	/*
	 * Prime both revalidation stamps with the state this lookup
	 * just resolved against (sampled before the branch probes, so a
	 * change landing mid-lookup still forces a re-check): the
	 * upper-dir stamp in d_fsdata and the branch generation in
	 * d_time.  Both are per-dentry - the generation deliberately so,
	 * because lower hardlink siblings share one union inode while
	 * the winning-branch decision is per-name (dcache.c).  Negative
	 * dentries are primed too: their revalidation gates the
	 * per-branch rescan on the same two stamps, so a cached miss
	 * costs no branch lookups until something observable changes.
	 */
	dentry->d_fsdata = (void *)stamp;
	dentry->d_time = (unsigned long)gen;

	return d_splice_alias(inode, dentry);
}

const char *aufsng_get_link(struct dentry *dentry, struct inode *inode,
			 struct delayed_call *done)
{
	struct path realpath;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	/*
	 * aufsng_path_real() owns the lockless upper/stack read protocol
	 * (torn-snapshot re-read behind smp_rmb); open-coding the reads
	 * here would silently miss any future change to that protocol.
	 */
	aufsng_path_real(inode, &realpath);
	if (!realpath.dentry)
		return ERR_PTR(-ESTALE);

	return vfs_get_link(realpath.dentry, done);
}
