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
 * Does @parent (in @mnt) contain a whiteout for @name?  Returns 1 if
 * yes, 0 if no, negative errno on error.  This is a lookup for the
 * SIBLING name ".wh.<name>", not a property of @name's own dentry.
 */
int aufsng_check_whiteout(struct vfsmount *mnt, struct dentry *parent,
		       const struct qstr *name)
{
	char buf[NAME_MAX + 1];
	struct qstr wh;
	struct dentry *whd;
	int ret;

	ret = aufsng_wh_name(buf, name, &wh);
	if (ret)
		return ret;

	whd = lookup_one_unlocked(mnt_idmap(mnt), &wh, parent);
	if (IS_ERR(whd))
		return PTR_ERR(whd);

	ret = 0;
	if (d_is_positive(whd)) {
		if (d_is_reg(whd))
			ret = 1;
		else
			ret = -EIO;	/* corrupt whiteout entry */
	}
	dput(whd);
	return ret;
}

/* is @dir (in @mnt) marked opaque? */
int aufsng_check_diropq(struct vfsmount *mnt, struct dentry *dir)
{
	struct qstr opq = QSTR(AUFSNG_WH_DIROPQ);
	struct dentry *whd;
	int ret;

	whd = lookup_one_unlocked(mnt_idmap(mnt), &opq, dir);
	if (IS_ERR(whd))
		return PTR_ERR(whd);

	ret = d_is_positive(whd) && d_is_reg(whd);
	dput(whd);
	return ret;
}

/*
 * Look up @name in one real branch directory.  Returns NULL for a
 * negative result, the referenced dentry for a positive one, or
 * (via *whiteout) 1 if this branch whites the name out (in which
 * case NULL is always returned and the search must stop here).
 */
static struct dentry *aufsng_lookup_once(struct vfsmount *mnt,
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
	struct aufsng_entry *poe;
	struct dentry *pupper;
	struct dentry *this;
	bool valid = true;
	unsigned int i;
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

	poe = AUFSNG_I_E(dir);
	for (i = 0; poe && i < poe->numlower; i++) {
		wh = 0;
		this = aufsng_lookup_once(poe->lowerstack[i].layer->mnt,
				       poe->lowerstack[i].dentry, name, &wh);
		if (IS_ERR(this))
			goto out;	/* error: keep the dentry valid */
		if (wh)
			goto out;	/* a whiteout hides all lowers: valid */
		if (this) {
			dput(this);
			valid = false;	/* a lower branch now provides it */
			goto out;
		}
	}

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
	inode->i_mode = realinode->i_mode;
	inode->i_uid = realinode->i_uid;
	inode->i_gid = realinode->i_gid;
	inode->i_rdev = realinode->i_rdev;
	inode->i_size = i_size_read(realinode);
	inode_set_atime_to_ts(inode, inode_get_atime(realinode));
	inode_set_mtime_to_ts(inode, inode_get_mtime(realinode));
	inode_set_ctime_to_ts(inode, inode_get_ctime(realinode));
	set_nlink(inode, realinode->i_nlink);

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
	struct inode *realinode;
	struct inode *key;
	struct inode *inode;
	unsigned int key_idx;

	realinode = upperdentry ? d_inode(upperdentry) :
				  d_inode(oe->lowerstack[0].dentry);
	key = oe->numlower ? d_inode(oe->lowerstack[0].dentry) :
			     d_inode(upperdentry);
	key_idx = oe->numlower ? oe->lowerstack[0].layer->idx : 0;

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
		 * pseudo-links keep such names on one inode).  This
		 * lookup found an upper the cached inode lacks, or a
		 * different one: for a directory this is handled by
		 * aufsng_dyn_adopt_upper() (which also rebuilds the merged
		 * listing); for a non-directory there is a single upper
		 * location, so the freshly found upper is authoritative and
		 * replaces any stale one - an app that saves by writing a
		 * temp file and renaming it over the name (geany's config,
		 * for one) gives the rw copy a new inode on every save, and
		 * the union inode (keyed by the stable lower origin) must
		 * follow it instead of failing with ESTALE.  A replacement of
		 * a different file type (the rw copy became a dir/symlink) is
		 * NOT adopted - the cached inode's ops were fixed at its type,
		 * so that falls through to ESTALE.  The superseded upper may
		 * still be in use by a lockless aufsng_path_real() reader, so
		 * it is parked and dropped at inode eviction, not here.
		 */
		if (!ok && !new_u && oe->numlower)
			ok = true;
		if (!ok && new_u) {
			if (S_ISDIR(inode->i_mode)) {
				ok = aufsng_dyn_adopt_upper(inode,
							 oe->numlower ?
							 oe->lowerstack[0].dentry :
							 NULL,
							 upperdentry);
			} else if ((inode->i_mode & S_IFMT) ==
				   (new_u->i_mode & S_IFMT)) {
				struct aufsng_inode *ai = AUFSNG_I(inode);

				mutex_lock(&ai->lock);
				if (!ai->upperdentry) {
					WRITE_ONCE(ai->upperdentry,
						   dget(upperdentry));
					ok = true;
				} else if (d_inode(ai->upperdentry) == new_u) {
					ok = true;
				} else {
					struct aufsng_dyn_parked *pk =
						kmalloc(sizeof(*pk), GFP_KERNEL);

					if (pk) {
						pk->oe = NULL;
						pk->mnt = NULL;
						pk->upper = ai->upperdentry;
						WRITE_ONCE(ai->upperdentry,
							   dget(upperdentry));
						aufsng_copyattr(inode);
						pk->next = ai->dyn_parked;
						ai->dyn_parked = pk;
						ok = true;
					}
				}
				mutex_unlock(&ai->lock);
			}
		}
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
	struct dentry *ret;
	const struct cred *old_cred;
	unsigned int i;
	bool stopped = false;
	int wh, err = 0;

	if (dentry->d_name.len > pfs->namelen)
		return ERR_PTR(-ENAMETOOLONG);
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
	aufsng_dentry_init_gen(dentry);
	old_cred = override_creds(pfs->creator_cred);

	poe = AUFSNG_E(dentry->d_parent);
	pupper = aufsng_upperdentry(dir);

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
		if (found && (d_inode(origin.dentry)->i_mode & S_IFMT) !=
			     (d_inode(upper)->i_mode & S_IFMT)) {
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
		err = -ENOMEM;
		oe = aufsng_alloc_entry(poe->numlower);
		if (!oe)
			goto out;
		err = 0;
		oe->numlower = 0;

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

	ret = d_splice_alias(inode, dentry);
	/*
	 * d_splice_alias() may return a preexisting alias of this
	 * inode instead of the fresh dentry stamped above.  The alias
	 * may still carry a stale generation from before a branch
	 * change, but this lookup just resolved it against the current
	 * stack, so restamp it - otherwise a pinned directory alias
	 * could fail revalidation forever, as no fresh lookup can ever
	 * replace it.
	 */
	if (!IS_ERR_OR_NULL(ret))
		aufsng_dentry_restamp_gen(ret, aufsng_dentry_gen(dentry));

	return ret;
}

const char *aufsng_get_link(struct dentry *dentry, struct inode *inode,
			 struct delayed_call *done)
{
	struct aufsng_entry *oe;
	struct dentry *real;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	real = aufsng_upperdentry(inode);
	if (!real) {
		oe = AUFSNG_I_E(inode);
		real = oe->numlower ? oe->lowerstack[0].dentry : NULL;
	}
	if (!real)
		return ERR_PTR(-ESTALE);

	return vfs_get_link(real, done);
}
