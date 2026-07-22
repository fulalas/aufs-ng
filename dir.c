// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng directory mutations.
 *
 * Every mutation is performed in branch 0 (the rw branch) with the
 * mounter's capabilities but the caller's fsuid/fsgid (so new objects
 * get the right owner), after copying up the parent chain.  Removing
 * a name still provided by a lower branch leaves a ".wh.<name>"
 * whiteout marker behind (a plain 0444 regular file - the real AUFS
 * on-disk format, verified against fs/aufs/whout.c), created BEFORE
 * the removal so a failure or crash between the two steps preserves
 * the delete rather than resurrecting the lower name.  Creating over
 * a whiteout parks the marker under a hidden temp name and restores
 * it if the create fails.  Creating - or renaming - a directory onto
 * a name that would otherwise still show a same-named lower
 * directory's content marks the directory opaque via ".wh..wh..opq".
 * Renaming a merged directory returns -EXDEV, making mv fall back to
 * copy+delete - AUFS itself has no cross-branch atomic directory
 * rename either.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include "aufsng.h"

/*
 * Credentials for creating objects: the mounter's capabilities with
 * the caller's fsuid/fsgid, so ownership lands on the caller.
 * Returns the cred to revert to; *newp must be put after reverting.
 */
static const struct cred *aufsng_override_create_creds(struct aufsng_fs *pfs,
						    struct cred **newp)
{
	kuid_t fsuid = current_fsuid();
	kgid_t fsgid = current_fsgid();
	const struct cred *old;
	struct cred *override;

	old = override_creds(pfs->creator_cred);
	override = prepare_creds();
	if (!override) {
		revert_creds(old);
		return ERR_PTR(-ENOMEM);
	}
	override->fsuid = fsuid;
	override->fsgid = fsgid;
	override_creds(override);
	*newp = override;
	return old;
}

/*
 * Does any lower branch of @dir still visibly provide @name?  Decides
 * whether removing @name from the rw branch must leave a whiteout
 * behind.  Returns 1/0, negative errno on error - an error must never
 * be treated as "not covered", or the delete silently skips the
 * whiteout and the lower file resurrects later.
 *
 * The caller must hold pfs->dyn_lock across BOTH this check and the
 * mutation it guards: a branch spliced in or out between the two
 * would make the verdict stale (a deleted name would resurrect
 * uncovered, or a stray whiteout would mask nothing), and must run
 * under credentials able to search the branch dirs (creator creds).
 */
static int aufsng_lower_covers(struct aufsng_fs *pfs, struct inode *dir,
			    const struct qstr *name)
{
	struct aufsng_path origin;
	int found;

	found = aufsng_find_origin(AUFSNG_I_E(dir), name, &origin);
	if (found > 0)
		dput(origin.dentry);
	return found;
}

/* create one 0444 marker file (matching real AUFS's WH_MASK) */
static int aufsng_create_marker(struct aufsng_fs *pfs, struct dentry *upperdir,
			     struct qstr *wh)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct dentry *slot;
	int err;

	slot = start_creating_noperm(upperdir, wh);
	if (IS_ERR(slot))
		return PTR_ERR(slot);
	if (d_is_positive(slot)) {
		end_dirop(slot);
		return 0;	/* already marked */
	}
	err = vfs_create(idmap, slot, S_IFREG | AUFSNG_WH_MODE, NULL);
	end_dirop(slot);
	return err;
}

/* create a whiteout named ".wh.<name>" inside @upperdir */
int aufsng_create_whiteout(struct aufsng_fs *pfs, struct dentry *upperdir,
			const struct qstr *name)
{
	char buf[NAME_MAX + 1];
	struct qstr wh;
	int err;

	err = aufsng_wh_name(buf, name, &wh);
	if (err)
		return err;
	return aufsng_create_marker(pfs, upperdir, &wh);
}

/* mark @dir (already created, empty, in the rw branch) opaque */
static int aufsng_mark_diropq(struct aufsng_fs *pfs, struct dentry *dir)
{
	struct qstr opq = QSTR(AUFSNG_WH_DIROPQ);

	return aufsng_create_marker(pfs, dir, &opq);
}

/* remove a ".wh.<name>" whiteout marker from @upperdir, if present */
int aufsng_remove_whiteout(struct aufsng_fs *pfs, struct dentry *upperdir,
			const struct qstr *name)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	char buf[NAME_MAX + 1];
	struct qstr wh;
	struct dentry *slot;
	int err;

	err = aufsng_wh_name(buf, name, &wh);
	if (err)
		return err;

	slot = start_removing_noperm(upperdir, &wh);
	if (IS_ERR(slot)) {
		/*
		 * start_removing_noperm() returns -ENOENT (not a negative
		 * dentry) when the name does not exist: no whiteout to remove
		 * is the common, expected case, not an error.
		 */
		return PTR_ERR(slot) == -ENOENT ? 0 : PTR_ERR(slot);
	}
	err = vfs_unlink(idmap, d_inode(upperdir), slot, NULL);
	end_dirop(slot);
	return err;
}

/*
 * Get a locked negative dentry for creating @name in @upperdir; a
 * positive occupant means EEXIST.  A stale whiteout for @name is the
 * caller's business: it is parked via aufsng_park_whiteout() first so
 * that it can be restored if the create then fails.
 */
struct dentry *aufsng_create_slot(struct dentry *upperdir,
			       const struct qstr *name)
{
	struct qstr q = QSTR_LEN(name->name, name->len);
	struct dentry *slot;

	slot = start_creating_noperm(upperdir, &q);
	if (IS_ERR(slot) || d_is_negative(slot))
		return slot;

	end_dirop(slot);
	return ERR_PTR(-EEXIST);
}

/*
 * Best-effort removal of an upper object that a failed or superseded
 * multi-step operation left behind (a partially created object, a
 * copy-up temp, a parked whiteout).  Left in place it would leak:
 * hidden behind a restored whiteout it makes every retry fail with
 * EEXIST, and visible it shadows lower content with a half-built
 * object.  A removal failure means the branch fs is in serious
 * trouble; log it, the leftover stays.
 */
void aufsng_remove_object(struct aufsng_fs *pfs, struct dentry *upperdir,
		       const struct qstr *name, bool is_dir)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct qstr q = QSTR_LEN(name->name, name->len);
	struct dentry *slot;
	int err;

	slot = start_removing_noperm(upperdir, &q);
	err = PTR_ERR_OR_ZERO(slot);
	if (!err) {
		if (is_dir)
			err = vfs_rmdir(idmap, d_inode(upperdir), slot, NULL);
		else
			err = vfs_unlink(idmap, d_inode(upperdir), slot, NULL);
		end_dirop(slot);
	}
	if (err)
		pr_err("aufs (aufs-ng): failed to remove leftover '%.*s' (%d)\n",
		       name->len, name->name, err);
}

static atomic_t aufsng_whtmp_seq = ATOMIC_INIT(0);

/*
 * Move an existing ".wh.<name>" whiteout aside to a hidden temp name
 * (".wh..wh.tmp.<seq>", inside AUFS's own ".wh..wh." bookkeeping
 * namespace, so it is invisible to lookup and readdir) instead of
 * deleting it up front: if the operation that is about to take over
 * the name then fails - ENOSPC being the everyday case - the whiteout
 * is renamed back, so a failed create/rename can never cancel an
 * earlier, successful delete.  Deleting it outright would need a new
 * inode to restore it, exactly what a full branch cannot provide;
 * renaming back allocates nothing.  Real AUFS parks whiteouts the
 * same way (au_whtmp).
 *
 * Returns 1 with @tmp/@tmpbuf (NAME_MAX + 1 bytes) filled if parked,
 * 0 if there was no whiteout, negative errno.
 */
static int aufsng_park_whiteout(struct aufsng_fs *pfs, struct dentry *upperdir,
			     const struct qstr *name, struct qstr *tmp,
			     char *tmpbuf)
{
	struct renamedata rd = {};
	char whbuf[NAME_MAX + 1];
	struct qstr wh;
	int present;
	int err;

	present = aufsng_check_whiteout(aufsng_upper_mnt(pfs), upperdir, name);
	if (present <= 0)
		return present;

	err = aufsng_wh_name(whbuf, name, &wh);
	if (err)
		return err;
	*tmp = QSTR_LEN(tmpbuf, snprintf(tmpbuf, NAME_MAX + 1, ".wh..wh.tmp.%u",
					 atomic_inc_return(&aufsng_whtmp_seq)));

	rd.mnt_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	rd.old_parent = upperdir;
	rd.new_parent = upperdir;
	err = start_renaming(&rd, 0, &wh, tmp);
	if (err)
		return err;
	err = vfs_rename(&rd);
	end_renaming(&rd);
	return err ? err : 1;
}

/* on success drop the parked whiteout; on failure rename it back */
static void aufsng_unpark_whiteout(struct aufsng_fs *pfs,
				struct dentry *upperdir,
				const struct qstr *name,
				struct qstr *tmp, bool restore)
{
	struct renamedata rd = {};
	char whbuf[NAME_MAX + 1];
	struct qstr wh;
	int err;

	if (!restore) {
		/* the drop is a plain leftover removal; failure logged there */
		aufsng_remove_object(pfs, upperdir, tmp, false);
		return;
	}

	err = aufsng_wh_name(whbuf, name, &wh);
	if (!err) {
		rd.mnt_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
		rd.old_parent = upperdir;
		rd.new_parent = upperdir;
		err = start_renaming(&rd, 0, tmp, &wh);
	}
	if (!err) {
		err = vfs_rename(&rd);
		end_renaming(&rd);
	}
	/*
	 * A metadata-only operation on an entry this call just parked;
	 * failure means the branch fs is in serious trouble.  The
	 * parked name stays hidden either way.
	 */
	if (err)
		pr_err("aufs (aufs-ng): failed to restore parked whiteout '%.*s' (%d)\n",
		       tmp->len, tmp->name, err);
}

struct aufsng_create_args {
	umode_t mode;
	dev_t rdev;
	const char *link;
};

static int aufsng_create_object(struct dentry *dentry,
			     const struct aufsng_create_args *a)
{
	struct dentry *parent = dentry->d_parent;
	struct inode *dir = d_inode(parent);
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	const struct cred *old_cred;
	struct cred *create_cred = NULL;
	struct dentry *pupper, *slot, *upper, *made;
	struct aufsng_path origin = { NULL, NULL };
	struct aufsng_entry *oe;
	struct inode *inode;
	bool is_dir = (a->mode & S_IFMT) == S_IFDIR;
	char whtmpbuf[NAME_MAX + 1];
	struct qstr whtmp;
	int parked = 0;
	int found;
	int err;

	err = aufsng_copy_up(parent);
	if (err)
		return err;
	pupper = aufsng_upperdentry(dir);

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return err;

	old_cred = aufsng_override_create_creds(pfs, &create_cred);
	if (IS_ERR(old_cred)) {
		err = PTR_ERR(old_cred);
		goto out_write;
	}

	/*
	 * The topmost lower this name would resurface as: a new
	 * directory over it must be marked opaque, and a new
	 * non-directory keeps it as the union inode's hash key origin,
	 * exactly as a copy-up of that lower would - so the keying is
	 * identical no matter which path created the upper object.
	 * dyn_lock stays held from this verdict through the mutation
	 * and the inode hashing that consume it, so a concurrent
	 * branch add/remove cannot invalidate either the opaque/keying
	 * decision or the origin's branch pinning in between.
	 */
	down_read(&pfs->dyn_lock);
	found = aufsng_find_origin(AUFSNG_I_E(dir), &dentry->d_name, &origin);
	if (found < 0) {
		err = found;
		goto out_creds;
	}
	/*
	 * A same-named lower of a different type is not this object's
	 * copy-up origin (e.g. creating a symlink where a lower regular
	 * file exists); the new upper shadows it as an independent object
	 * and must be keyed by itself, not aliased onto the lower's
	 * identity.  Directory creates use @found only for opaque marking,
	 * which is type-independent, so this is scoped to non-directories.
	 */
	if (!is_dir && found && !aufsng_origin_type_ok(origin.dentry, a->mode)) {
		dput(origin.dentry);
		origin.dentry = NULL;
		found = 0;
	}

	parked = aufsng_park_whiteout(pfs, pupper, &dentry->d_name,
				   &whtmp, whtmpbuf);
	if (parked < 0) {
		err = parked;
		goto out_origin;
	}

	slot = aufsng_create_slot(pupper, &dentry->d_name);
	if (IS_ERR(slot)) {
		err = PTR_ERR(slot);
		goto out_unpark;
	}

	switch (a->mode & S_IFMT) {
	case S_IFREG:
		err = vfs_create(idmap, slot, a->mode, NULL);
		break;
	case S_IFDIR:
		made = vfs_mkdir(idmap, d_inode(pupper), slot, a->mode, NULL);
		if (IS_ERR(made)) {
			/* vfs_mkdir consumed the dentry and the lock */
			err = PTR_ERR(made);
			goto out_unpark;
		}
		slot = made;
		err = 0;
		break;
	case S_IFLNK:
		err = vfs_symlink(idmap, d_inode(pupper), slot, a->link,
				  NULL);
		break;
	default:
		err = vfs_mknod(idmap, d_inode(pupper), slot, a->mode,
				a->rdev, NULL);
		break;
	}
	if (err) {
		end_dirop(slot);
		goto out_unpark;
	}

	upper = dget(slot);
	end_dirop(slot);

	if (is_dir && found) {
		err = aufsng_mark_diropq(pfs, upper);
		if (err) {
			dput(upper);
			goto out_remove;
		}
	}

	oe = aufsng_alloc_entry(!is_dir && found ? 1 : 0);
	if (!oe) {
		dput(upper);
		err = -ENOMEM;
		goto out_remove;
	}
	if (oe->numlower) {
		oe->lowerstack[0] = origin;
		origin.dentry = NULL;
	}
	inode = aufsng_get_inode(dentry->d_sb, upper, oe);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_remove;
	}
	d_instantiate(dentry, inode);

	atomic64_inc(&AUFSNG_I(dir)->version);
	aufsng_copyattr(dir);
	goto out_unpark;

out_remove:
	/*
	 * The object was created but the operation failed after it:
	 * without this removal it stays behind - hidden by the restored
	 * whiteout it turns every retry into EEXIST, and unhidden it is
	 * a directory missing its opaque marker.
	 */
	aufsng_remove_object(pfs, pupper, &dentry->d_name, is_dir);
out_unpark:
	if (parked)
		aufsng_unpark_whiteout(pfs, pupper, &dentry->d_name, &whtmp,
				    err != 0);
out_origin:
	dput(origin.dentry);
out_creds:
	up_read(&pfs->dyn_lock);
	revert_creds(old_cred);
	put_cred(create_cred);
out_write:
	mnt_drop_write(aufsng_upper_mnt(pfs));
	return err;
}

int aufsng_create(struct mnt_idmap *idmap, struct inode *dir,
	       struct dentry *dentry, umode_t mode, bool excl)
{
	struct aufsng_create_args a = { .mode = (mode & 07777) | S_IFREG };

	return aufsng_create_object(dentry, &a);
}

struct dentry *aufsng_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode)
{
	struct aufsng_create_args a = { .mode = (mode & 07777) | S_IFDIR };
	int err;

	err = aufsng_create_object(dentry, &a);
	return err ? ERR_PTR(err) : NULL;
}

int aufsng_mknod(struct mnt_idmap *idmap, struct inode *dir,
	      struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct aufsng_create_args a = { .mode = mode, .rdev = rdev };

	return aufsng_create_object(dentry, &a);
}

int aufsng_symlink(struct mnt_idmap *idmap, struct inode *dir,
		struct dentry *dentry, const char *link)
{
	struct aufsng_create_args a = { .mode = S_IFLNK | 0777, .link = link };

	return aufsng_create_object(dentry, &a);
}

/*
 * Hardlink @old to @new.  Real AUFS supports link(2) via its pseudo-
 * link mechanism when the two names would otherwise land in different
 * branches; that mechanism is explicitly out of scope here (see
 * project notes), so aufs-ng only ever links within the rw branch,
 * after copying @old up if needed - a plain vfs_link, same as linking
 * two names already in the same real directory tree.
 */
int aufsng_link(struct dentry *old, struct inode *dir, struct dentry *new)
{
	struct dentry *parent = new->d_parent;
	struct aufsng_fs *pfs = AUFSNG_FS(old->d_sb);
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	const struct cred *old_cred;
	struct cred *create_cred = NULL;
	struct dentry *pupper, *oldupper, *slot;
	struct inode *inode;
	char whtmpbuf[NAME_MAX + 1];
	struct qstr whtmp;
	int parked = 0;
	int err;

	err = aufsng_copy_up(old);
	if (!err)
		err = aufsng_copy_up(parent);
	if (err)
		return err;

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return err;

	old_cred = aufsng_override_create_creds(pfs, &create_cred);
	if (IS_ERR(old_cred)) {
		err = PTR_ERR(old_cred);
		goto out_write;
	}

	pupper = aufsng_upperdentry(dir);
	oldupper = aufsng_upperdentry(d_inode(old));
	if (WARN_ON(!pupper || !oldupper)) {
		err = -ENOENT;
		goto out_creds;
	}

	parked = aufsng_park_whiteout(pfs, pupper, &new->d_name,
				   &whtmp, whtmpbuf);
	if (parked < 0) {
		err = parked;
		goto out_creds;
	}

	slot = aufsng_create_slot(pupper, &new->d_name);
	if (IS_ERR(slot)) {
		err = PTR_ERR(slot);
		goto out_unpark;
	}

	err = vfs_link(oldupper, idmap, d_inode(pupper), slot, NULL);
	end_dirop(slot);
	if (err)
		goto out_unpark;

	inode = d_inode(old);
	ihold(inode);
	d_instantiate(new, inode);

	atomic64_inc(&AUFSNG_I(dir)->version);
	aufsng_copyattr(dir);
	aufsng_copyattr(inode);

out_unpark:
	if (parked)
		aufsng_unpark_whiteout(pfs, pupper, &new->d_name, &whtmp,
				    err != 0);
out_creds:
	revert_creds(old_cred);
	put_cred(create_cred);
out_write:
	mnt_drop_write(aufsng_upper_mnt(pfs));
	return err;
}

static int aufsng_do_remove(struct dentry *dentry, bool is_dir)
{
	struct dentry *parent = dentry->d_parent;
	struct inode *dir = d_inode(parent);
	struct inode *inode = d_inode(dentry);
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	const struct cred *old_cred;
	struct dentry *pupper, *upper;
	int covered;
	int err;

	err = aufsng_copy_up(parent);
	if (err)
		return err;

	if (is_dir) {
		err = aufsng_check_empty_dir(dentry);
		if (err)
			return err;
	}

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return err;

	old_cred = override_creds(pfs->creator_cred);

	/*
	 * dyn_lock is held from the coverage verdict through the
	 * mutation it decides: a branch spliced in between the two
	 * could provide the name being deleted, and skipping the
	 * whiteout against the old stack would resurrect it the moment
	 * the delete returns.
	 */
	down_read(&pfs->dyn_lock);
	covered = aufsng_lower_covers(pfs, dir, &dentry->d_name);
	if (covered < 0) {
		err = covered;
		goto out_dyn;
	}

	/*
	 * Serialize against a concurrent copy-up of this same inode
	 * (aufsng_copy_up_one() also takes oi->lock around its own
	 * oi->upperdentry read/write): without it, an in-flight copy-up
	 * that hasn't set oi->upperdentry yet is invisible here, so this
	 * removal skips the real unlink and only writes a whiteout,
	 * while the copy-up goes on to give the name fresh upper content
	 * right after - both a whiteout and a real entry for the same
	 * name end up in the branch at once.
	 */
	mutex_lock(&oi->lock);
	pupper = aufsng_upperdentry(dir);
	upper = oi->upperdentry;

	/*
	 * Whiteout FIRST, removal second - AUFS's own ordering.  If the
	 * whiteout cannot be created (ENOSPC), the object is untouched
	 * and the delete fails cleanly; the reverse order would destroy
	 * the upper copy and then fail, silently replacing the file
	 * with its stale lower version.  In the transient window where
	 * both the whiteout and the real entry exist, the whiteout wins
	 * in both lookup and readdir, so a crash between the two steps
	 * preserves the delete.
	 */
	if (covered) {
		err = aufsng_create_whiteout(pfs, pupper, &dentry->d_name);
		if (err)
			goto out;
	}

	if (upper) {
		struct qstr q = QSTR_LEN(dentry->d_name.name,
					 dentry->d_name.len);
		struct dentry *slot;

		slot = start_removing_noperm(pupper, &q);
		if (IS_ERR(slot)) {
			err = PTR_ERR(slot);
			/*
			 * No upper entry under THIS name: the inode's
			 * upperdentry is a copied-up lower hardlink
			 * sibling (lower links share one union inode,
			 * whose single upperdentry carries whichever
			 * name was copied up first).  The name itself is
			 * lower-only, so the whiteout created above is
			 * the whole removal.  Directories cannot be
			 * hardlinks, so this never applies to them: a
			 * dir upper missing its name (out-of-band
			 * rename in the rw branch) keeps failing, as it
			 * always did, rather than skipping the
			 * clear_whiteouts + rmdir it still needs.
			 */
			if (err == -ENOENT && !is_dir)
				err = 0;
		} else {
			if (is_dir) {
				/* union-empty: only whiteouts/opq remain inside */
				err = aufsng_clear_whiteouts(pfs, upper);
				if (!err)
					err = vfs_rmdir(idmap, d_inode(pupper),
							slot, NULL);
			} else {
				err = vfs_unlink(idmap, d_inode(pupper), slot,
						 NULL);
			}
			end_dirop(slot);
		}
		if (err) {
			/* roll the pre-created whiteout back */
			if (covered) {
				int wherr = aufsng_remove_whiteout(pfs, pupper,
							&dentry->d_name);

				/*
				 * Unrecoverable: the whiteout now masks a
				 * name whose removal just failed, so still-
				 * live content is hidden until the marker is
				 * removed from the branch by hand.
				 */
				if (wherr)
					pr_err("aufs (aufs-ng): failed to roll back whiteout '%.*s' (%d), the name stays hidden\n",
					       dentry->d_name.len,
					       dentry->d_name.name, wherr);
			}
			goto out;
		}
	}

	if (is_dir)
		clear_nlink(inode);
	else
		drop_nlink(inode);
	atomic64_inc(&AUFSNG_I(dir)->version);
	aufsng_copyattr(dir);

out:
	mutex_unlock(&oi->lock);
out_dyn:
	up_read(&pfs->dyn_lock);
	revert_creds(old_cred);
	mnt_drop_write(aufsng_upper_mnt(pfs));
	return err;
}

int aufsng_unlink(struct inode *dir, struct dentry *dentry)
{
	return aufsng_do_remove(dentry, false);
}

int aufsng_rmdir(struct inode *dir, struct dentry *dentry)
{
	return aufsng_do_remove(dentry, true);
}

/*
 * Undo a just-committed upper rename: move the (still-locked-out from
 * the union's point of view) new name back to the old one.  Metadata-
 * only, needs no space, so it succeeds even on the full branch that
 * typically made the caller want to undo.
 */
static int aufsng_rename_back(struct aufsng_fs *pfs, struct dentry *oldparent,
			   const struct qstr *oldname,
			   struct dentry *newparent,
			   const struct qstr *newname)
{
	struct renamedata rd = {};
	struct qstr oldq = QSTR_LEN(oldname->name, oldname->len);
	struct qstr newq = QSTR_LEN(newname->name, newname->len);
	int err;

	rd.mnt_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	rd.old_parent = newparent;
	rd.new_parent = oldparent;
	err = start_renaming(&rd, 0, &newq, &oldq);
	if (!err) {
		err = vfs_rename(&rd);
		end_renaming(&rd);
	}
	return err;
}

int aufsng_rename(struct mnt_idmap *idmap, struct inode *olddir,
	       struct dentry *old, struct inode *newdir,
	       struct dentry *new, unsigned int flags)
{
	struct aufsng_fs *pfs = AUFSNG_FS(old->d_sb);
	struct mnt_idmap *upper_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	const struct cred *old_cred;
	struct renamedata rd = {};
	struct aufsng_entry *oe = AUFSNG_I_E(d_inode(old));
	struct dentry *newupperdir;
	/*
	 * Rollback safety hinges on whether the underlying upper rename
	 * destroyed something, not on union visibility: a victim that
	 * exists only in a lower branch has no upper entry, so the
	 * rename created the upper name and undoing it is loss-free.
	 */
	bool had_victim = d_is_positive(new) &&
			  aufsng_upperdentry(d_inode(new));
	bool replace_dir = d_is_positive(new) && d_is_dir(new);
	char whtmpbuf[NAME_MAX + 1];
	struct qstr whtmp;
	int parked = 0;
	int covered, covered_new;
	int err;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	/*
	 * Renaming a directory still merged with lower branches would
	 * detach it from its lower content; AUFS has no cross-branch
	 * redirect mechanism either, so tell mv to fall back to
	 * copy+delete.
	 */
	if (d_is_dir(old) && oe && oe->numlower)
		return -EXDEV;
	if (replace_dir) {
		err = aufsng_check_empty_dir(new);
		if (err)
			return err;
		oe = AUFSNG_I_E(d_inode(new));
		if (oe && oe->numlower)
			return -EXDEV;
	}

	err = aufsng_copy_up(old->d_parent);
	if (!err)
		err = aufsng_copy_up(new->d_parent);
	if (!err)
		err = aufsng_copy_up(old);
	if (err)
		return err;

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return err;

	old_cred = override_creds(pfs->creator_cred);

	/*
	 * dyn_lock is held from both coverage verdicts (does the old
	 * name need a whiteout? would lower content show through the
	 * new name?) through the rename and markers they decide, so a
	 * concurrent branch add/remove cannot make either stale.
	 */
	down_read(&pfs->dyn_lock);
	covered = aufsng_lower_covers(pfs, olddir, &old->d_name);
	if (covered < 0) {
		err = covered;
		goto out;
	}
	covered_new = d_is_dir(old) ?
		aufsng_lower_covers(pfs, newdir, &new->d_name) : 0;
	if (covered_new < 0) {
		err = covered_new;
		goto out;
	}

	newupperdir = aufsng_upperdentry(newdir);

	/*
	 * A stale ".wh.<newname>" from an earlier delete of a name a
	 * lower branch also provides would otherwise keep hiding the
	 * name after this rename gives it fresh upper content.  It is
	 * parked, not deleted: if the rename then fails, it is renamed
	 * back, so the earlier delete survives.  A union-empty target
	 * directory being replaced still physically contains its own
	 * whiteouts/opaque marker; vfs_rename() would fail with
	 * -ENOTEMPTY on the real upper directory otherwise.  (Those
	 * marker deletions cannot be parked without AUFS's whole-dir
	 * whtmp machinery; the residual window is a rename that fails
	 * AFTER clear_whiteouts succeeded, resurrecting lower names
	 * inside the surviving, union-empty target dir.)
	 */
	parked = aufsng_park_whiteout(pfs, newupperdir, &new->d_name,
				   &whtmp, whtmpbuf);
	if (parked < 0) {
		err = parked;
		goto out;
	}
	if (replace_dir && d_inode(new) && aufsng_upperdentry(d_inode(new))) {
		err = aufsng_clear_whiteouts(pfs,
					  aufsng_upperdentry(d_inode(new)));
		if (err)
			goto out_unpark;
	}

	rd.mnt_idmap = upper_idmap;
	rd.old_parent = aufsng_upperdentry(olddir);
	rd.new_parent = newupperdir;
	rd.flags = flags;
	{
		struct qstr oldq = QSTR_LEN(old->d_name.name,
					    old->d_name.len);
		struct qstr newq = QSTR_LEN(new->d_name.name,
					    new->d_name.len);

		err = start_renaming(&rd, 0, &oldq, &newq);
	}
	if (!err) {
		err = vfs_rename(&rd);
		end_renaming(&rd);
	}
	if (err)
		goto out_unpark;

	if (covered) {
		int wherr = aufsng_create_whiteout(pfs, aufsng_upperdentry(olddir),
						&old->d_name);
		/*
		 * Without the whiteout, lower content the old name was
		 * shadowing resurfaces beside the renamed file.  Undo the
		 * rename - metadata-only, needs no space even on the ENOSPC
		 * that typically lands here - and fail cleanly.  Only
		 * possible when the rename replaced nothing: a replaced
		 * victim is already destroyed, so undoing would lose the
		 * new name too; then (or if the undo itself fails) the
		 * rename stands, is reported as success (the VFS would
		 * never retry the whiteout on its own), and the resurfacing
		 * is warned about.
		 */
		if (wherr && !had_victim &&
		    !aufsng_rename_back(pfs, aufsng_upperdentry(olddir),
				     &old->d_name, newupperdir,
				     &new->d_name)) {
			err = wherr;
			goto out_unpark;
		}
		if (wherr)
			pr_err("aufs (aufs-ng): failed to cover renamed-away '%.*s' (%d), it may resurface\n",
			       old->d_name.len, old->d_name.name, wherr);
	}

	/*
	 * A directory moved onto a name that lower branches still
	 * provide must be marked opaque, exactly as mkdir over that
	 * name would be (AUFS's rename DIROPQ): without the marker the
	 * deleted lower directory's content would merge into - and
	 * "resurrect" inside - the renamed directory.  A marker failure
	 * is unwound like a whiteout failure above, one step deeper:
	 * remove the whiteout just created for the old name, then undo
	 * the rename (AUFS's rename reverts here too).  If any undo
	 * step fails, the rename stands and is reported as success with
	 * a warning.
	 */
	if (covered_new) {
		int opqerr = aufsng_mark_diropq(pfs,
					aufsng_upperdentry(d_inode(old)));

		if (opqerr && !had_victim) {
			int backerr = 0;

			if (covered)
				backerr = aufsng_remove_whiteout(pfs,
						aufsng_upperdentry(olddir),
						&old->d_name);
			if (!backerr)
				backerr = aufsng_rename_back(pfs,
						aufsng_upperdentry(olddir),
						&old->d_name, newupperdir,
						&new->d_name);
			if (!backerr) {
				err = opqerr;
				goto out_unpark;
			}
		}
		if (opqerr)
			pr_err("aufs (aufs-ng): failed to mark renamed dir '%.*s' opaque (%d), deleted lower content may show through\n",
			       new->d_name.len, new->d_name.name, opqerr);
	}

	atomic64_inc(&AUFSNG_I(olddir)->version);
	if (newdir != olddir)
		atomic64_inc(&AUFSNG_I(newdir)->version);
	aufsng_copyattr(olddir);
	if (newdir != olddir)
		aufsng_copyattr(newdir);

out_unpark:
	if (parked)
		aufsng_unpark_whiteout(pfs, newupperdir, &new->d_name, &whtmp,
				    err != 0);
out:
	up_read(&pfs->dyn_lock);
	revert_creds(old_cred);
	mnt_drop_write(aufsng_upper_mnt(pfs));
	return err;
}
