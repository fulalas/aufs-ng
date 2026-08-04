// SPDX-License-Identifier: GPL-2.0-only
/*
 * Directory mutations.  All of them happen in branch 0 with the
 * mounter's capabilities but the caller's fsuid/fsgid, after copying
 * up the parent chain.
 *
 * Removing a name a lower still provides leaves a ".wh.<name>" marker,
 * created BEFORE the removal so a crash between the two preserves the
 * delete.  Creating over a whiteout parks the marker and restores it
 * if the create fails.  A directory created or renamed onto a name a
 * lower directory also provides is marked opaque.  Renaming a merged
 * directory returns -EXDEV, so mv falls back to copy+delete - AUFS has
 * no cross-branch directory rename either.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include "aufsng.h"

/*
 * Creation credentials: the mounter's capabilities with the caller's
 * fsuid/fsgid.  Returns the cred to revert to; *newp is put after.
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
static int aufsng_create_whiteout(struct aufsng_fs *pfs,
			       struct dentry *upperdir,
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
static int aufsng_remove_whiteout(struct aufsng_fs *pfs,
			       struct dentry *upperdir,
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
		/* -ENOENT here means no whiteout to remove: the common case */
		return PTR_ERR(slot) == -ENOENT ? 0 : PTR_ERR(slot);
	}
	err = vfs_unlink(idmap, d_inode(upperdir), slot, NULL);
	end_dirop(slot);
	return err;
}

/*
 * A locked negative dentry for creating @name in @upperdir; a positive
 * occupant means EEXIST.  A stale whiteout is the caller's business:
 * it parks it first, so a failed create can restore it.
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
 * Remove one upper entry by name, sweeping a directory's own markers
 * first: a leftover dir may carry them, and vfs_rmdir() on a
 * physically non-empty dir is ENOTEMPTY.  Error policy stays with the
 * callers.
 */
static int aufsng_do_remove_object(struct aufsng_fs *pfs,
				struct dentry *upperdir,
				const struct qstr *name, bool is_dir)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct qstr q = QSTR_LEN(name->name, name->len);
	struct dentry *slot;
	int err;

	slot = start_removing_noperm(upperdir, &q);
	if (IS_ERR(slot))
		return PTR_ERR(slot);
	if (is_dir) {
		err = aufsng_clear_whiteouts(pfs, slot);
		if (!err)
			err = vfs_rmdir(idmap, d_inode(upperdir), slot, NULL);
	} else {
		err = vfs_unlink(idmap, d_inode(upperdir), slot, NULL);
	}
	end_dirop(slot);
	return err;
}

/*
 * Best-effort removal of what a failed multi-step operation left
 * behind.  Left in place it leaks: hidden behind a restored whiteout
 * it fails every retry with EEXIST, visible it shadows lower content.
 * A failure is logged and returned, for the callers that must escalate
 * rather than report the original error; the rest ignore it.
 */
int aufsng_remove_object(struct aufsng_fs *pfs, struct dentry *upperdir,
		       const struct qstr *name, bool is_dir)
{
	int err = aufsng_do_remove_object(pfs, upperdir, name, is_dir);

	if (err)
		pr_err("aufs (aufs-ng): failed to remove leftover '%.*s' (%d)\n",
		       name->len, name->name, err);
	return err;
}

/*
 * Rename one entry inside the rw branch - the single home of the
 * renamedata convention, shared by whiteout parking and rename undo.
 * Metadata-only, so it still works on a full branch.
 */
static int aufsng_branch_rename(struct aufsng_fs *pfs,
			     struct dentry *src_parent,
			     const struct qstr *src,
			     struct dentry *dst_parent,
			     const struct qstr *dst)
{
	struct renamedata rd = {};
	struct qstr srcq = QSTR_LEN(src->name, src->len);
	struct qstr dstq = QSTR_LEN(dst->name, dst->len);
	int err;

	rd.mnt_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	rd.old_parent = src_parent;
	rd.new_parent = dst_parent;
	err = start_renaming(&rd, 0, &srcq, &dstq);
	if (!err) {
		err = vfs_rename(&rd);
		end_renaming(&rd);
	}
	return err;
}

static atomic_t aufsng_whtmp_seq = ATOMIC_INIT(0);

/*
 * Mint a hidden ".wh..wh.tmp.<seq>" name into @buf (NAME_MAX + 1) -
 * one definition of the parked-entry format, inside the ".wh..wh."
 * namespace so lookup and readdir never show it.
 */
static void aufsng_whtmp_name(char *buf, struct qstr *tmp)
{
	*tmp = QSTR_LEN(buf, snprintf(buf, NAME_MAX + 1, ".wh..wh.tmp.%u",
				      atomic_inc_return(&aufsng_whtmp_seq)));
}

/*
 * Move a ".wh.<name>" whiteout aside to a hidden temp name instead of
 * deleting it: if the operation taking over the name then fails, it is
 * renamed back, so a failed create can never cancel an earlier
 * successful delete.  Restoring a deleted one would need a new inode -
 * exactly what a full branch cannot give; renaming back allocates
 * nothing.  AUFS parks whiteouts the same way.
 *
 * 1 with @tmp/@tmpbuf (NAME_MAX + 1) filled if parked, 0 if there was
 * no whiteout, negative errno.
 */
static int aufsng_park_whiteout(struct aufsng_fs *pfs, struct dentry *upperdir,
			     const struct qstr *name, struct qstr *tmp,
			     char *tmpbuf)
{
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
	aufsng_whtmp_name(tmpbuf, tmp);

	err = aufsng_branch_rename(pfs, upperdir, &wh, upperdir, tmp);
	return err ? err : 1;
}

/* on success drop the parked whiteout; on failure rename it back */
static void aufsng_unpark_whiteout(struct aufsng_fs *pfs,
				struct dentry *upperdir,
				const struct qstr *name,
				struct qstr *tmp, bool restore)
{
	char whbuf[NAME_MAX + 1];
	struct qstr wh;
	int err;

	if (!restore) {
		/* the drop is a plain leftover removal; failure logged there */
		aufsng_remove_object(pfs, upperdir, tmp, false);
		return;
	}

	err = aufsng_wh_name(whbuf, name, &wh);
	if (!err)
		err = aufsng_branch_rename(pfs, upperdir, tmp, upperdir, &wh);
	/* Metadata-only, on an entry just parked; the temp stays hidden */
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
	struct aufsng_entry *oe;
	struct inode *inode;
	bool is_dir = (a->mode & S_IFMT) == S_IFDIR;
	char whtmpbuf[NAME_MAX + 1];
	struct qstr whtmp;
	int parked = 0;
	int hides_lower = 0;
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
	 * Would a lower resurface under this name?  Only a new DIRECTORY
	 * cares, to hide it with an opaque marker.  Nothing created here
	 * is a copy-up, so nothing takes a lower into its stack: a new
	 * object is keyed by itself, or a delete-then-recreate would hand
	 * it the deleted file's identity.  dyn_lock is held from the
	 * verdict through the mutation that consumes it.
	 */
	percpu_down_read(&pfs->dyn_lock);
	if (is_dir) {
		hides_lower = aufsng_lower_covers(dir, &dentry->d_name);
		if (hides_lower < 0) {
			err = hides_lower;
			goto out_creds;
		}
	}

	parked = aufsng_park_whiteout(pfs, pupper, &dentry->d_name,
				   &whtmp, whtmpbuf);
	if (parked < 0) {
		err = parked;
		goto out_creds;
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

	if (hides_lower) {
		err = aufsng_mark_diropq(pfs, upper);
		if (err) {
			dput(upper);
			goto out_remove;
		}
	}

	/* a new object is keyed by itself: no lower in its stack */
	oe = aufsng_alloc_entry(0);
	if (!oe) {
		dput(upper);
		err = -ENOMEM;
		goto out_remove;
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
	 * Created, then the operation failed: left behind it turns every
	 * retry into EEXIST, or is a directory missing its marker.
	 */
	aufsng_remove_object(pfs, pupper, &dentry->d_name, is_dir);
out_unpark:
	if (parked)
		aufsng_unpark_whiteout(pfs, pupper, &dentry->d_name, &whtmp,
				    err != 0);
out_creds:
	percpu_up_read(&pfs->dyn_lock);
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
 * Hardlink @old to @new.  AUFS uses pseudo-links when the names would
 * land in different branches; that is out of scope here, so aufs-ng
 * links within the rw branch only, after copying @old up.
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
	const struct cred *old_cred;
	struct dentry *pupper, *upper;
	bool real_removed = false;
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
	 * dyn_lock spans the verdict and the mutation it decides: a
	 * branch spliced in between could provide the name, and the
	 * skipped whiteout would resurrect it.
	 */
	percpu_down_read(&pfs->dyn_lock);
	covered = aufsng_lower_covers(dir, &dentry->d_name);
	if (covered < 0) {
		err = covered;
		goto out_dyn;
	}

	/*
	 * Serialize against a copy-up of this inode: one that has not set
	 * oi->upperdentry yet is invisible here, so the removal would
	 * write only a whiteout while the copy-up gives the name fresh
	 * upper content - both in the branch at once.
	 */
	mutex_lock(&oi->lock);
	pupper = aufsng_upperdentry(dir);
	upper = oi->upperdentry;

	/*
	 * Whiteout FIRST, removal second - AUFS's ordering.  If the
	 * whiteout fails the object is untouched and the delete fails
	 * cleanly; the reverse would destroy the upper copy and then
	 * fail, leaving the stale lower version.  While both exist the
	 * whiteout wins, so a crash between them preserves the delete.
	 */
	if (covered) {
		err = aufsng_create_whiteout(pfs, pupper, &dentry->d_name);
		if (err)
			goto out;
	}

	if (upper && is_dir) {
		char tmpbuf[NAME_MAX + 1];
		struct qstr tmp;
		int werr;

		/*
		 * Rename the victim aside FIRST: the delete commits at this
		 * rename, before a marker inside is touched.  Sweeping in
		 * place is not restartable - a sweep failing halfway has
		 * already destroyed ".wh.<child>" entries, so the surviving
		 * directory resurrects every name they hid.  Parked, it is
		 * invisible whatever happens next; AUFS's own ordering.
		 * The rename is metadata-only and works on a full branch.
		 */
		aufsng_whtmp_name(tmpbuf, &tmp);
		err = aufsng_branch_rename(pfs, pupper, &dentry->d_name,
					pupper, &tmp);
		if (!err) {
			real_removed = true;
			/*
			 * Best effort, as AUFS ignores its whtmp rmdir
			 * failures: the delete is committed and a leftover
			 * is invisible, costing only branch space.
			 */
			werr = aufsng_do_remove_object(pfs, pupper, &tmp, true);
			if (werr)
				pr_warn("aufs (aufs-ng): rmdir left parked dir '%s' in the rw branch (%d)\n",
					tmpbuf, werr);
		}
	} else if (upper) {
		err = aufsng_do_remove_object(pfs, pupper, &dentry->d_name,
					   false);
		if (!err)
			real_removed = true;
		else if (err == -ENOENT)
			/*
			 * No upper under THIS name: the upperdentry belongs
			 * to a copied-up hardlink sibling.  This name is
			 * lower-only, so the whiteout is the whole removal.
			 */
			err = 0;
	}
	if (upper && err) {
		/* roll the pre-created whiteout back */
		if (covered) {
			int wherr = aufsng_remove_whiteout(pfs, pupper,
						&dentry->d_name);

			/* Unrecoverable: live content stays hidden by the marker */
			if (wherr)
				pr_err("aufs (aufs-ng): failed to roll back whiteout '%.*s' (%d), the name stays hidden\n",
				       dentry->d_name.len,
				       dentry->d_name.name, wherr);
		}
		goto out;
	}

	/*
	 * The link count only moves when a real link went away.  A
	 * whiteout-only removal changed no real inode: dropping anyway
	 * would take a shared union inode to nlink 0 while another name
	 * is still linked, then wrap to UINT_MAX when that one goes.
	 */
	if (is_dir) {
		if (real_removed || !upper)
			clear_nlink(inode);
	} else if (real_removed) {
		drop_nlink(inode);
	} else if (!upper) {
		/*
		 * A lower-only name: no real link went away, but if the
		 * lower had only this one, the only way to reach this union
		 * inode just did, so it is as dead as a real removal.
		 *
		 * Ask the lower, not the union inode's mirror: the mirror
		 * only refreshes when drift is noticed, so a link made
		 * directly in the branch would leave it reading 1 while a
		 * sibling name still resolves to it.
		 */
		struct inode *real = aufsng_inode_real(inode);

		if (real && real->i_nlink <= 1)
			clear_nlink(inode);
	}

	/*
	 * The last name is gone: take the inode out of the hash.
	 *
	 * The key is the copy-up origin, re-derived by name on create, so
	 * without this a re-create of the same name gets the dead inode -
	 * and everything the VFS still holds against it.  Most visibly a
	 * running executable: deny_write_access() makes the first write
	 * to the replacement fail with ETXTBSY.  A real filesystem gives
	 * a re-created name a new inode.
	 *
	 * Eviction alone is not enough: it unhashes only on the last
	 * reference, and a deleted-but-open object is exactly the case.
	 */
	if (!inode->i_nlink)
		remove_inode_hash(inode);

	atomic64_inc(&AUFSNG_I(dir)->version);
	aufsng_copyattr(dir);

out:
	mutex_unlock(&oi->lock);
out_dyn:
	percpu_up_read(&pfs->dyn_lock);
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
 * Compensate a committed rename whose follow-up marker failed: drop
 * the new whiteout, then rename back - both metadata-only, so they
 * work on the full branch that brought the caller here.  0 only if
 * fully undone; otherwise the rename stands and is reported as
 * success with a warning.
 */
static int aufsng_rename_undo(struct aufsng_fs *pfs, struct inode *olddir,
			   struct dentry *old, struct dentry *newupperdir,
			   struct dentry *new, bool drop_whiteout)
{
	int err = 0;

	if (drop_whiteout)
		err = aufsng_remove_whiteout(pfs, aufsng_upperdentry(olddir),
					  &old->d_name);
	if (!err)
		err = aufsng_branch_rename(pfs, newupperdir, &new->d_name,
					aufsng_upperdentry(olddir),
					&old->d_name);
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
	 * Rollback safety hinges on whether the upper rename destroyed
	 * something, not on union visibility: a lower-only victim has no
	 * upper entry, so undoing is loss-free.  Decided from the branch
	 * rename's own target lookup under the parent locks, not sampled
	 * at entry: the upperdentry is a per-inode proxy that lies for
	 * hardlink siblings, and a copy-up could race the window.
	 */
	bool had_victim = false;
	bool replace_dir = d_is_positive(new) && d_is_dir(new);
	char whtmpbuf[NAME_MAX + 1];
	struct qstr whtmp;
	int parked = 0;
	int covered, covered_new;
	int err;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	/*
	 * Renaming a still-merged directory would detach it from its
	 * lower content; AUFS has no redirect either, so mv falls back.
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
	 * dyn_lock spans both coverage verdicts and the rename and
	 * markers they decide, so no branch change can make them stale.
	 */
	percpu_down_read(&pfs->dyn_lock);

	/*
	 * Re-derive the -EXDEV verdicts under dyn_lock: the entry checks
	 * ran before the copy-ups, and a branch add in that window can
	 * splice a lower into either stack.  Renaming such a directory
	 * would detach it and then mark it opaque, hiding the new
	 * branch's subtree for good.
	 */
	err = -EXDEV;
	if (d_is_dir(old)) {
		oe = AUFSNG_I_E(d_inode(old));
		if (oe && oe->numlower)
			goto out;
	}
	if (replace_dir) {
		oe = AUFSNG_I_E(d_inode(new));
		if (oe && oe->numlower)
			goto out;
	}

	covered = aufsng_lower_covers(olddir, &old->d_name);
	if (covered < 0) {
		err = covered;
		goto out;
	}
	covered_new = d_is_dir(old) ?
		aufsng_lower_covers(newdir, &new->d_name) : 0;
	if (covered_new < 0) {
		err = covered_new;
		goto out;
	}
	err = 0;

	newupperdir = aufsng_upperdentry(newdir);

	/*
	 * A stale ".wh.<newname>" would keep hiding the name after this
	 * rename gives it fresh content.  Parked, not deleted, so a
	 * failed rename can restore the earlier delete.  A union-empty
	 * target dir still physically holds its own markers, or
	 * vfs_rename() fails -ENOTEMPTY.  (Those cannot be parked; the
	 * residual window is a rename failing after they were cleared.)
	 */
	parked = aufsng_park_whiteout(pfs, newupperdir, &new->d_name,
				   &whtmp, whtmpbuf);
	if (parked < 0) {
		err = parked;
		goto out;
	}
	if (replace_dir && d_inode(new)) {
		struct dentry *new_upper = aufsng_upperdentry(d_inode(new));

		if (new_upper) {
			err = aufsng_clear_whiteouts(pfs, new_upper);
			if (err)
				goto out_unpark;
		}
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
		had_victim = d_is_positive(rd.new_dentry);
		err = vfs_rename(&rd);
		end_renaming(&rd);
	}
	if (err)
		goto out_unpark;

	if (covered) {
		int wherr = aufsng_create_whiteout(pfs, aufsng_upperdentry(olddir),
						&old->d_name);
		/*
		 * Without the whiteout, what the old name shadowed
		 * resurfaces beside the renamed file.  Undo and fail
		 * cleanly - but only if the rename replaced nothing: a
		 * destroyed victim would be lost too.  Otherwise the rename
		 * stands, is reported as success, and is warned about.
		 */
		if (wherr && !had_victim &&
		    !aufsng_rename_undo(pfs, olddir, old, newupperdir, new,
				     false)) {
			err = wherr;
			goto out_unpark;
		}
		if (wherr)
			pr_err("aufs (aufs-ng): failed to cover renamed-away '%.*s' (%d), it may resurface\n",
			       old->d_name.len, old->d_name.name, wherr);
	}

	/*
	 * A directory moved onto a name lowers still provide is marked
	 * opaque, as mkdir over it would be, or the lower directory's
	 * content merges into the renamed one.  A marker failure unwinds
	 * like the whiteout above, one step deeper: drop the new
	 * whiteout, then undo the rename.  If an undo step fails the
	 * rename stands, reported as success with a warning.
	 */
	if (covered_new) {
		int opqerr = aufsng_mark_diropq(pfs,
					aufsng_upperdentry(d_inode(old)));

		if (opqerr && !had_victim &&
		    !aufsng_rename_undo(pfs, olddir, old, newupperdir, new,
				     covered)) {
			err = opqerr;
			goto out_unpark;
		}
		if (opqerr)
			pr_err("aufs (aufs-ng): failed to mark renamed dir '%.*s' opaque (%d), deleted lower content may show through\n",
			       new->d_name.len, new->d_name.name, opqerr);
	}

	/* the copy-up marker is per-name: a rename stops matching it */
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
	percpu_up_read(&pfs->dyn_lock);
	revert_creds(old_cred);
	mnt_drop_write(aufsng_upper_mnt(pfs));
	return err;
}
