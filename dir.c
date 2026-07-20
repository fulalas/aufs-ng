// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng directory mutations.
 *
 * Every mutation is performed in branch 0 (the rw branch) with the
 * mounter's capabilities but the caller's fsuid/fsgid (so new objects
 * get the right owner), after copying up the parent chain.  Removing
 * a name still provided by a lower branch leaves a ".wh.<name>"
 * whiteout marker behind (a plain 0444 regular file - the real AUFS
 * on-disk format, verified against fs/aufs/whout.c); creating over a
 * whiteout removes the marker first.  Creating a directory that would
 * otherwise still show a same-named lower directory's content marks
 * the new directory opaque via ".wh..wh..opq".  Renaming a merged
 * directory returns -EXDEV, making mv fall back to copy+delete - AUFS
 * itself has no cross-branch atomic directory rename either.
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
 */
static int aufsng_lower_covers(struct aufsng_fs *pfs, struct inode *dir,
			    const struct qstr *name)
{
	const struct cred *old_cred = override_creds(pfs->creator_cred);
	struct aufsng_path origin;
	int found;

	down_read(&pfs->dyn_lock);
	found = aufsng_find_origin(AUFSNG_I_E(dir), name, &origin);
	up_read(&pfs->dyn_lock);
	revert_creds(old_cred);
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
 * Get a locked negative dentry for creating @name in @upperdir.  A
 * stale whiteout for @name (left behind by an earlier delete, now
 * being recreated) is removed first: leaving both a whiteout and a
 * real object for the same name in the same branch would make
 * lookup/readdir results ambiguous.
 */
static struct dentry *aufsng_create_slot(struct aufsng_fs *pfs,
				      struct dentry *upperdir,
				      const struct qstr *name)
{
	struct qstr q = QSTR_LEN(name->name, name->len);
	struct dentry *slot;
	int wh, err;

	wh = aufsng_check_whiteout(aufsng_upper_mnt(pfs), upperdir, name);
	if (wh < 0)
		return ERR_PTR(wh);
	if (wh) {
		err = aufsng_remove_whiteout(pfs, upperdir, name);
		if (err)
			return ERR_PTR(err);
	}

	slot = start_creating_noperm(upperdir, &q);
	if (IS_ERR(slot) || d_is_negative(slot))
		return slot;

	end_dirop(slot);
	return ERR_PTR(-EEXIST);
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
	 */
	down_read(&pfs->dyn_lock);
	found = aufsng_find_origin(AUFSNG_I_E(dir), &dentry->d_name, &origin);
	up_read(&pfs->dyn_lock);
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

	slot = aufsng_create_slot(pfs, pupper, &dentry->d_name);
	if (IS_ERR(slot)) {
		err = PTR_ERR(slot);
		goto out_origin;
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
			goto out_origin;
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
		goto out_origin;
	}

	upper = dget(slot);
	end_dirop(slot);

	if (is_dir && found) {
		err = aufsng_mark_diropq(pfs, upper);
		if (err) {
			dput(upper);
			goto out_origin;
		}
	}

	oe = aufsng_alloc_entry(!is_dir && found ? 1 : 0);
	if (!oe) {
		dput(upper);
		err = -ENOMEM;
		goto out_origin;
	}
	if (oe->numlower) {
		oe->lowerstack[0] = origin;
		origin.dentry = NULL;
	}
	down_read(&pfs->dyn_lock);
	inode = aufsng_get_inode(dentry->d_sb, upper, oe);
	up_read(&pfs->dyn_lock);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_origin;
	}
	d_instantiate(dentry, inode);

	atomic64_inc(&AUFSNG_I(dir)->version);
	aufsng_copyattr(dir);

out_origin:
	dput(origin.dentry);
out_creds:
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

	slot = aufsng_create_slot(pfs, pupper, &new->d_name);
	if (IS_ERR(slot)) {
		err = PTR_ERR(slot);
		goto out_creds;
	}

	err = vfs_link(oldupper, idmap, d_inode(pupper), slot, NULL);
	end_dirop(slot);
	if (err)
		goto out_creds;

	inode = d_inode(old);
	ihold(inode);
	d_instantiate(new, inode);

	atomic64_inc(&AUFSNG_I(dir)->version);
	aufsng_copyattr(dir);
	aufsng_copyattr(inode);

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

	covered = aufsng_lower_covers(pfs, dir, &dentry->d_name);
	if (covered < 0)
		return covered;

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return err;

	old_cred = override_creds(pfs->creator_cred);

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

	if (upper) {
		struct qstr q = QSTR_LEN(dentry->d_name.name,
					 dentry->d_name.len);
		struct dentry *slot;

		slot = start_removing_noperm(pupper, &q);
		if (IS_ERR(slot)) {
			err = PTR_ERR(slot);
			goto out;
		}
		if (is_dir) {
			/* union-empty: only whiteouts/opq may remain inside */
			err = aufsng_clear_whiteouts(pfs, upper);
			if (!err)
				err = vfs_rmdir(idmap, d_inode(pupper), slot,
						NULL);
		} else {
			err = vfs_unlink(idmap, d_inode(pupper), slot, NULL);
		}
		end_dirop(slot);
		if (err)
			goto out;
	}

	if (covered) {
		err = aufsng_create_whiteout(pfs, pupper, &dentry->d_name);
		if (err)
			goto out;
	}

	if (is_dir)
		clear_nlink(inode);
	else
		drop_nlink(inode);
	atomic64_inc(&AUFSNG_I(dir)->version);
	aufsng_copyattr(dir);

out:
	mutex_unlock(&oi->lock);
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
	bool replace_dir = d_is_positive(new) && d_is_dir(new);
	int covered;
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

	covered = aufsng_lower_covers(pfs, olddir, &old->d_name);
	if (covered < 0)
		return covered;

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return err;

	old_cred = override_creds(pfs->creator_cred);

	newupperdir = aufsng_upperdentry(newdir);

	/*
	 * A stale ".wh.<newname>" from an earlier delete of a name a
	 * lower branch also provides would otherwise keep hiding the
	 * name after this rename gives it fresh upper content - the
	 * same reason copy-up clears it before its own rename.  A
	 * union-empty target directory being replaced still physically
	 * contains its own whiteouts/opaque marker; vfs_rename() would
	 * fail with -ENOTEMPTY on the real upper directory otherwise.
	 */
	err = aufsng_remove_whiteout(pfs, newupperdir, &new->d_name);
	if (!err && replace_dir && d_inode(new) && aufsng_upperdentry(d_inode(new)))
		err = aufsng_clear_whiteouts(pfs, aufsng_upperdentry(d_inode(new)));
	if (err)
		goto out;

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
	if (err)
		goto out;
	err = vfs_rename(&rd);
	end_renaming(&rd);
	if (err)
		goto out;

	if (covered) {
		int wherr = aufsng_create_whiteout(pfs, aufsng_upperdentry(olddir),
						&old->d_name);
		/*
		 * The rename already committed; report success either
		 * way; failing here (e.g. ENOSPC) would tell the VFS a
		 * rename that in fact happened did not, and it would
		 * never retry the whiteout on its own.
		 */
		if (wherr)
			pr_err("aufs (aufs-ng): failed to cover renamed-away '%.*s' (%d), it may resurface\n",
			       old->d_name.len, old->d_name.name, wherr);
	}

	atomic64_inc(&AUFSNG_I(olddir)->version);
	if (newdir != olddir)
		atomic64_inc(&AUFSNG_I(newdir)->version);
	aufsng_copyattr(olddir);
	if (newdir != olddir)
		aufsng_copyattr(newdir);

out:
	revert_creds(old_cred);
	mnt_drop_write(aufsng_upper_mnt(pfs));
	return err;
}
