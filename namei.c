// SPDX-License-Identifier: GPL-2.0-only
/*
 * Layered lookup with AUFS whiteout semantics.  AUFS marks a name
 * deleted with a SEPARATE sibling ".wh.<name>" (a 0444 regular file),
 * not with the name itself as overlayfs does.  So a branch lookup for
 * "foo" always probes ".wh.foo" first: found, the name is deleted from
 * that branch down and the search stops.  A directory holding
 * ".wh..wh..opq" is opaque and hides every lower branch.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include "aufsng.h"

/*
 * Probe @dir for the marker @name: 1 present as a regular file, 0
 * absent, negative errno on error.  @strict maps a non-regular
 * occupant to -EIO instead of treating it as absent.
 */
static int aufsng_probe_marker(struct vfsmount *mnt, struct dentry *dir,
			    struct qstr *name, bool strict)
{
	struct dentry *whd;
	int ret;

	whd = lookup_one_positive_unlocked(mnt_idmap(mnt), name, dir);
	if (IS_ERR(whd))
		return PTR_ERR(whd) == -ENOENT ? 0 : PTR_ERR(whd);

	ret = d_is_reg(whd) ? 1 : (strict ? -EIO : 0);
	dput(whd);
	return ret;
}

/*
 * Does @parent hold a whiteout for @name?  1/0, or negative errno.
 * A lookup of the SIBLING ".wh.<name>", not of @name's own dentry.
 */
int aufsng_check_whiteout(struct vfsmount *mnt, struct dentry *parent,
		       const struct qstr *name)
{
	char buf[NAME_MAX + 1];
	struct qstr wh;
	int ret;

	/*
	 * A name too long for a ".wh." sibling cannot be whited out, so
	 * answer "no whiteout" rather than fail lookups of long lower
	 * names.  Deleting one still fails, as on real AUFS.
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
 * Look up @name in one branch dir: NULL if negative, a referenced
 * dentry if positive, or *whiteout = 1 (and NULL) if this branch
 * whites it out, in which case the search must stop here.
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

	this = lookup_one_positive_unlocked(mnt_idmap(mnt), &q, base);
	if (IS_ERR(this) && PTR_ERR(this) == -ENOENT)
		return NULL;
	return this;
}

/*
 * The topmost lower still visibly providing @name under @poe: an upper
 * object's copy-up origin, and what resurfaces if the upper name goes.
 * 1 with a reference in @out, 0 if no lower provides it, negative
 * errno on error.  Caller holds pfs->dyn_lock (any mode).
 *
 * @skip_layer is ignored during the walk (branch removal re-resolves
 * against survivors only).  @mode, when non-zero, requires the first
 * positive hit to be that type: a same-named lower of another type is
 * an independent object, and it hides anything deeper, so the walk
 * STOPS there rather than continuing.
 */
int aufsng_find_origin_ex(struct aufsng_entry *poe, const struct qstr *name,
		       const struct aufsng_layer *skip_layer, umode_t mode,
		       struct aufsng_path *out)
{
	unsigned int i;

	for (i = 0; poe && i < poe->numlower; i++) {
		struct aufsng_path *lower = &poe->lowerstack[i];
		struct dentry *this;
		int wh = 0;

		if (skip_layer && lower->layer == skip_layer)
			continue;
		this = aufsng_lookup_once(lower->mnt, lower->dentry,
				       name, &wh);
		if (IS_ERR(this))
			return PTR_ERR(this);
		if (wh)
			return 0;
		if (this) {
			if (mode && !aufsng_origin_type_ok(this, mode)) {
				dput(this);
				return 0;
			}
			out->layer = lower->layer;
			out->dentry = this;
			out->mnt = lower->mnt;
			return 1;
		}
	}
	return 0;
}

/*
 * The per-level merge rule, in ONE place: walk @poe's lowers from
 * @from for @name, append every visible DIRECTORY to @m's stack, and
 * stop where the merged view ends - a whiteout, a same-named
 * non-directory, or an opaque directory.  @skip is ignored entirely.
 *
 * Both stack builders come through here - lookup and the
 * branch-removal tail merge - so a pinned directory and a fresh lookup
 * cannot answer differently.  (aufsng_find_origin_ex() stays separate:
 * it wants the FIRST provider, not a merged stack.)
 *
 * @m->oe is allocated on the first hit, so an all-negative lookup
 * allocates nothing.  A caller appending to its own stack passes it in
 * @m->oe with @m->n set, having reserved room for the rest of @poe.
 * @m->allow_top_nondir lets a non-directory be the whole stack when
 * nothing precedes it; a rebuilt directory stack never wants that.
 *
 * 0 or negative errno; either way the caller publishes @m->n into
 * @m->oe->numlower, so freeing the entry drops exactly what was taken
 * here.  Caller holds pfs->dyn_lock, under creator credentials.
 */
int aufsng_merge_dirs(struct aufsng_merge *m, struct aufsng_entry *poe,
		   unsigned int from, const struct qstr *name,
		   const struct aufsng_layer *skip)
{
	unsigned int i;

	for (i = from; poe && i < poe->numlower; i++) {
		struct aufsng_path *lower = &poe->lowerstack[i];
		struct dentry *this;
		int wh = 0, opq;

		if (skip && lower->layer == skip)
			continue;
		this = aufsng_lookup_once(lower->mnt, lower->dentry, name, &wh);
		if (IS_ERR(this))
			return PTR_ERR(this);
		if (wh)
			break;
		if (!this)
			continue;
		if (!m->oe) {
			m->oe = aufsng_alloc_entry(poe->numlower - i);
			if (!m->oe) {
				dput(this);
				return -ENOMEM;
			}
			m->oe->numlower = 0;
		}
		if (!d_is_dir(this)) {
			if (m->allow_top_nondir && !m->n) {
				m->oe->lowerstack[m->n].layer = lower->layer;
				m->oe->lowerstack[m->n].dentry = this;
				m->oe->lowerstack[m->n].mnt = lower->mnt;
				m->n++;
			} else {
				dput(this);
			}
			break;
		}
		m->oe->lowerstack[m->n].layer = lower->layer;
		m->oe->lowerstack[m->n].dentry = this;
		m->oe->lowerstack[m->n].mnt = lower->mnt;
		m->n++;

		/* On the last branch the opaque verdict changes nothing */
		if (i + 1 >= poe->numlower)
			break;
		opq = aufsng_check_diropq(lower->mnt, this);
		if (opq < 0)
			return opq;
		if (opq)
			break;
	}
	return 0;
}

/*
 * May the object @upper provides as @name be keyed on a lower?  In ONE
 * place so lookup, readdir, revalidation and branch removal cannot
 * drift apart - a disagreement shows up as readdir's d_ino
 * contradicting stat's st_ino.
 *
 * No upper is a pure lower object and always may.  A directory may
 * unless it is opaque, which is why copy-up never marks one.  Anything
 * else may only if copy-up recorded it as the copy of the lower called
 * @name - see AUFSNG_XATTR_ORIGIN.
 *
 * @upper must be NULL or positive.  Runs under creator credentials,
 * not from an RCU walk.  A failed probe answers "no": the object keeps
 * its own identity.
 */
bool aufsng_upper_claims_origin(struct aufsng_fs *pfs, struct dentry *upper,
			     const struct qstr *name)
{
	if (!upper)
		return true;
	if (d_is_dir(upper))
		return aufsng_check_diropq(aufsng_upper_mnt(pfs), upper) == 0;
	return aufsng_has_origin(pfs, upper, name);
}

/*
 * Does any lower of @dir still visibly provide @name?  The boolean
 * form of aufsng_find_origin().  1/0, or negative errno - each caller
 * picks its error policy (fatal for mutations, keep-the-dentry for
 * revalidation).
 *
 * A caller pairing the verdict with a mutation must hold dyn_lock
 * across BOTH, or a branch change between them makes it stale.  Runs
 * under creator credentials.
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
 * udba=reval negative-dentry revalidation: an out-of-band branch edit
 * may have resurrected the name.  True if it is still absent, false if
 * some branch now provides it and the dentry must be dropped.  On
 * error keep it valid.  Not from an RCU walk.
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
	percpu_down_read(&pfs->dyn_lock);

	pupper = aufsng_upperdentry(dir);
	/*
	 * No upper: whiteouts live only in the rw branch and the lowers are
	 * read-only, so an absent name stays absent until the branch set
	 * changes - and both add and remove drop affected negatives
	 * themselves.  So skip the rescan; repeated misses are the common
	 * case in read-only system directories.
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

	/* The merge's own decision; errors keep the dentry valid */
	if (aufsng_lower_covers(dir, name) > 0)
		valid = false;	/* a lower branch now provides it */

out:
	percpu_up_read(&pfs->dyn_lock);
	revert_creds(old_cred);
	return valid;
}

/* the iget5/ilookup5 hash-match rule; shared with dynlayer.c's rekey check */
int aufsng_inode_test(struct inode *inode, void *data)
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
 * Find or create the union inode for a resolved stack, keyed on the
 * top lower (stable across copy-up) or the rw inode.  Consumes
 * @upperdentry and @oe either way.  Callers hold dyn_lock (read): the
 * adoption heal parks the superseded upper on dyn_parked.
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
	key = aufsng_hash_key(oe, upperdentry, &key_idx);

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
		 * Upper mismatches on a shared key are usually healable.
		 *
		 * No upper here but one cached: a racing copy-up, or a
		 * hardlink sibling of a copied-up name - the cached state
		 * wins, as in AUFS, but only while that upper is alive.  A
		 * dead one is shed instead, or it would serve the deleted
		 * upper's content forever and a write would land in the
		 * unlinked inode.
		 *
		 * An upper the cached inode lacks, or a different one (a
		 * save-by-rename gives the rw copy a new inode each time,
		 * and the union inode must follow it rather than go
		 * ESTALE): adopt it through dynlayer.c, which also parks
		 * the superseded upper for lockless readers.
		 */
		if (!ok && !new_u && oe->numlower) {
			if (aufsng_dentry_alive(cached))
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
	unsigned long gen;
	bool stopped = false;
	int wh, err = 0;

	/*
	 * ".wh." names are bookkeeping.  Refusing them here, as AUFS
	 * does, also blocks creating one: creation needs a negative
	 * dentry from lookup first.
	 */
	if (aufsng_is_wh_name(dentry->d_name.name, dentry->d_name.len))
		return ERR_PTR(-EPERM);

	/* A branch add/remove may swap the stacks; exclude it for the lookup */
	percpu_down_read(&pfs->dyn_lock);
	old_cred = override_creds(pfs->creator_cred);

	poe = AUFSNG_E(dentry->d_parent);
	pupper = aufsng_upperdentry(dir);
	/* sampled before the branch probes; see the priming below */
	stamp = aufsng_reval_stamp(pfs, dir);
	gen = atomic_long_read(&pfs->branch_gen);

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
			/* @stopped only matters when there is a lower stack to stop */
			if (d_is_dir(upper) && poe && poe->numlower) {
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
		 * A non-directory upper ends the merge, but its origin is
		 * still collected: the inode is hashed by it, which keeps
		 * st_ino stable across copy-up and eviction.
		 *
		 * Only a real copy-up gets that link; sharing a name is not
		 * enough, or a file created over a whiteout, renamed onto
		 * the name or hardlinked would inherit the identity of what
		 * it replaced.  A different-type lower is excluded too, so
		 * a marker outliving its object cannot alias two types.
		 */
		struct aufsng_path origin = { NULL, NULL };
		int found = 0;

		if (aufsng_upper_claims_origin(pfs, upper, &dentry->d_name)) {
			found = aufsng_find_origin_ex(poe, &dentry->d_name,
						   NULL,
						   d_inode(upper)->i_mode,
						   &origin);
			if (found < 0) {
				err = found;
				goto out;
			}
		}
		oe = aufsng_entry_from_origin(found, &origin);
		if (IS_ERR(oe)) {
			err = PTR_ERR(oe);
			oe = NULL;
			goto out;
		}
	} else if (!stopped && poe && poe->numlower) {
		/*
		 * The shared rule: only a top non-directory can stand alone
		 * as the stack, and only with no upper in that place.
		 */
		struct aufsng_merge m = { .allow_top_nondir = !upper };

		err = aufsng_merge_dirs(&m, poe, 0, &dentry->d_name, NULL);
		oe = m.oe;
		if (oe)
			oe->numlower = m.n;
		if (err)
			goto out;
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
	percpu_up_read(&pfs->dyn_lock);
	aufsng_free_entry(oe);
	dput(upper);
	if (err)
		return ERR_PTR(err);

	/*
	 * Prime both stamps with what this lookup resolved against,
	 * sampled before the probes so a change landing mid-lookup still
	 * forces a re-check.  Both are per-dentry: the winning-branch
	 * decision is per-name.  Negatives are primed too, so a cached
	 * miss costs no branch lookups until something changes.
	 */
	aufsng_store_reval_stamps(dentry, stamp, gen);

	return d_splice_alias(inode, dentry);
}

const char *aufsng_get_link(struct dentry *dentry, struct inode *inode,
			 struct delayed_call *done)
{
	struct path realpath;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	/* aufsng_path_real() owns the lockless upper/stack read protocol */
	aufsng_path_real(inode, &realpath);
	if (!realpath.dentry)
		return ERR_PTR(-ESTALE);

	return vfs_get_link(realpath.dentry, done);
}
