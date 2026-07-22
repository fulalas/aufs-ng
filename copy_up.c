// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng copy-up: give a lower-backed object an upper copy before
 * the first modification.  Regular file data is copied into a
 * uniquely named file in the workdir and renamed into place, so a
 * half-copied file is never visible under its real name; directories,
 * symlinks and special files are created in place (their "data" is
 * atomic by nature).  Runs with the mounter's credentials; ownership,
 * mode, times and xattrs are copied from the lower original.
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include <linux/xattr.h>
#include <linux/uio.h>
#include <linux/atomic.h>
#include "aufsng.h"

static atomic_t aufsng_tmpfile_seq = ATOMIC_INIT(0);

static int aufsng_copy_xattr(struct aufsng_fs *pfs, const struct path *oldpath,
			  struct dentry *new)
{
	struct mnt_idmap *old_idmap = mnt_idmap(oldpath->mnt);
	struct mnt_idmap *new_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	ssize_t list_size, size, value_size = 0;
	char *buf, *name, *value = NULL;
	int err = 0;

	list_size = vfs_listxattr(oldpath->dentry, NULL, 0);
	if (list_size <= 0)
		return list_size == -EOPNOTSUPP ? 0 : list_size;

	buf = kvmalloc(list_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	list_size = vfs_listxattr(oldpath->dentry, buf, list_size);
	if (list_size < 0) {
		err = list_size;
		goto out;
	}

	for (name = buf; list_size;
	     list_size -= strlen(name) + 1, name += strlen(name) + 1) {
retry:
		size = vfs_getxattr(old_idmap, oldpath->dentry, name, value,
				    value_size);
		if (size == -ERANGE || (size > 0 && !value)) {
			void *new_value;

			size = vfs_getxattr(old_idmap, oldpath->dentry, name,
					    NULL, 0);
			if (size < 0)
				goto next;
			new_value = kvmalloc(size, GFP_KERNEL);
			if (!new_value) {
				err = -ENOMEM;
				break;
			}
			kvfree(value);
			value = new_value;
			value_size = size;
			goto retry;
		}
		if (size < 0)
			goto next;

		err = vfs_setxattr(new_idmap, new, name, value, size, 0);
		if (err) {
			if (err == -EOPNOTSUPP || err == -EPERM)
				err = 0;	/* best effort, like a cp -a */
			else
				break;
		}
next:
		;
	}

out:
	kvfree(value);
	kvfree(buf);
	return err;
}

static int aufsng_set_attr_from(struct aufsng_fs *pfs, struct dentry *upper,
			     const struct kstat *stat)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct iattr attr = {
		.ia_valid = ATTR_UID | ATTR_GID |
			    ATTR_ATIME | ATTR_MTIME | ATTR_ATIME_SET |
			    ATTR_MTIME_SET | ATTR_FORCE,
		.ia_uid = stat->uid,
		.ia_gid = stat->gid,
		.ia_atime = stat->atime,
		.ia_mtime = stat->mtime,
	};
	int err;

	/*
	 * A symlink carries no meaningful mode and there is no lchmod:
	 * notify_change() with ATTR_MODE on one fails (EOPNOTSUPP on the
	 * branch fs), which would abort the whole copy-up.  Set every
	 * other attribute and skip the mode for symlinks, exactly as
	 * overlayfs does on copy-up.
	 */
	if (!S_ISLNK(stat->mode)) {
		attr.ia_valid |= ATTR_MODE;
		attr.ia_mode = stat->mode;
	}

	inode_lock(d_inode(upper));
	err = notify_change(idmap, upper, &attr, NULL);
	inode_unlock(d_inode(upper));

	return err;
}

static int aufsng_copy_data(struct aufsng_fs *pfs, const struct path *lowerpath,
			 struct dentry *work, loff_t len)
{
	struct path workpath = {
		.mnt = aufsng_upper_mnt(pfs),
		.dentry = work,
	};
	struct file *in, *out;
	loff_t pos_in = 0, pos_out = 0;
	int err = 0;

	in = kernel_file_open(lowerpath, O_RDONLY | O_LARGEFILE,
			      pfs->creator_cred);
	if (IS_ERR(in))
		return PTR_ERR(in);
	out = kernel_file_open(&workpath, O_WRONLY | O_LARGEFILE,
			       pfs->creator_cred);
	if (IS_ERR(out)) {
		fput(in);
		return PTR_ERR(out);
	}

	/*
	 * COPY_FILE_SPLICE (kernel-internal) makes the VFS handle the
	 * cross-sb case itself, falling back to an in-kernel splice
	 * that moves pages through a pipe - no bounce buffer, and the
	 * fallback policy stays maintained with the VFS instead of a
	 * hand-picked errno list here (this is what nfsd and overlayfs
	 * do).  No fsync: the union offers no crash consistency across
	 * the copy-up + rename pair anyway, and neither AUFS nor
	 * overlayfs flush here - it only serializes every copy-up
	 * against the branch device.
	 */
	while (pos_in < len) {
		ssize_t bytes = vfs_copy_file_range(in, pos_in, out, pos_out,
						    len - pos_in,
						    COPY_FILE_SPLICE);
		if (bytes <= 0) {
			err = bytes < 0 ? bytes : -EIO;
			break;
		}
		pos_in += bytes;
		pos_out += bytes;
	}

	fput(out);
	fput(in);
	return err;
}

/*
 * Phase one of a regular-file copy-up, run with NO locks held: create
 * a uniquely named temp file inside the SAME directory as the final
 * target and fill in the lower file's data.  AUFS has no separate
 * workdir; the temp lives in AUFS's own ".wh..wh." bookkeeping
 * namespace (".wh..wh.pxu<seq>"), so it is invisible to lookup and
 * readdir for the whole duration of the copy (and a crash leftover is
 * cleaned up like any other stale marker: rmdir's clear_whiteouts
 * sweep removes it with the directory).
 *
 * This phase must run outside both oi->lock and mnt_want_write():
 * vfs_copy_file_range() takes the upper sb's own write protection
 * (file_start_write), which is the same sb_writers level as
 * mnt_want_write() - holding it across the copy recurses and
 * deadlocks against a concurrent freeze of the upper fs - and
 * sb_writers ranks BEFORE oi->lock everywhere else (mutations take
 * mnt_want_write, then dyn_lock, then oi->lock).  Nothing is
 * committed here; publication happens under oi->lock in
 * aufsng_copy_up_one().
 */
static struct dentry *aufsng_copy_up_prep_regular(struct aufsng_fs *pfs,
					       const struct path *lowerpath,
					       struct dentry *pupper,
					       loff_t size,
					       struct qstr *tmp, char *tmpbuf)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct dentry *work;
	int err;

	snprintf(tmpbuf, 32, ".wh..wh.pxu%u",
		 atomic_inc_return(&aufsng_tmpfile_seq));
	*tmp = QSTR(tmpbuf);

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return ERR_PTR(err);
	work = start_creating_noperm(pupper, tmp);
	if (IS_ERR(work))
		goto out_drop;
	if (d_is_positive(work)) {
		end_dirop(work);
		work = ERR_PTR(-EEXIST);
		goto out_drop;
	}
	err = vfs_create(idmap, work, S_IFREG | 0600, NULL);
	if (err) {
		end_dirop(work);
		work = ERR_PTR(err);
		goto out_drop;
	}
	dget(work);
	end_dirop(work);
	mnt_drop_write(aufsng_upper_mnt(pfs));

	err = aufsng_copy_data(pfs, lowerpath, work, size);
	if (err) {
		if (!mnt_want_write(aufsng_upper_mnt(pfs))) {
			aufsng_remove_object(pfs, pupper, tmp, false);
			mnt_drop_write(aufsng_upper_mnt(pfs));
		} else {
			/* same trace the commit-side leak gets (out_tmp) */
			pr_warn("aufs (aufs-ng): read-only rw branch, copy-up temp '%s' left behind\n",
				tmpbuf);
		}
		dput(work);
		return ERR_PTR(err);
	}
	return work;

out_drop:
	mnt_drop_write(aufsng_upper_mnt(pfs));
	return work;
}

/*
 * Phase two, under mnt_want_write() + oi->lock: dress the temp in the
 * lower's metadata and rename it over the real name - the commit
 * point.  On success @work is hashed under the final name and is the
 * live upper.
 */
static int aufsng_copy_up_commit_regular(struct aufsng_fs *pfs,
				      struct dentry *dentry,
				      const struct path *lowerpath,
				      struct dentry *pupper,
				      struct dentry *work)
{
	struct renamedata rd = {};
	struct qstr nameq = QSTR_LEN(dentry->d_name.name, dentry->d_name.len);
	struct kstat stat;
	int err;

	/*
	 * Re-stat the lower NOW so ownership/mode/times land from the
	 * same point in time as the xattrs read below - one coherent
	 * commit-era metadata snapshot.  The prep-era stat served only
	 * to size the data copy; if the lower was mutated out-of-band
	 * mid-copy-up the data is stale either way (a window as old as
	 * the unlocked data copy itself), but the metadata should not
	 * additionally be stitched from a third instant.
	 */
	err = vfs_getattr(lowerpath, &stat,
			  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	if (!err)
		err = aufsng_copy_xattr(pfs, lowerpath, work);
	if (!err)
		err = aufsng_set_attr_from(pfs, work, &stat);
	if (err)
		return err;

	/*
	 * No whiteout can sit at the target: the name was visible to
	 * the lookup that triggered this copy-up (a whiteout would
	 * have hidden it), and one appearing since means a completed
	 * unlink - aufsng_copy_up_one() re-checks for that under
	 * oi->lock before calling here, and aufsng_do_remove() holds
	 * the same lock for its whole whiteout + unlink sequence.
	 *
	 * vfs_rename() moves the file onto @work (the source dentry)
	 * via d_move() and leaves rd.new_dentry negative -
	 * end_renaming() drops the helper's own refs.
	 */
	rd.mnt_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	rd.old_parent = pupper;
	rd.new_parent = pupper;
	err = start_renaming_dentry(&rd, 0, work, &nameq);
	if (err)
		return err;
	err = vfs_rename(&rd);
	end_renaming(&rd);
	return err;
}

/*
 * Create the upper copy of a non-regular object directly in the rw
 * parent dir.  No whiteout can occupy the name here for the same
 * reason as in the regular-file path: the name was visible to the
 * lookup that led here, and copy_up_one aborts under oi->lock if a
 * delete slipped in since.
 */
static struct dentry *aufsng_copy_up_inplace(struct aufsng_fs *pfs,
					  struct dentry *dentry,
					  const struct path *lowerpath,
					  struct dentry *pupper,
					  const struct kstat *stat)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct dentry *slot, *upper = NULL;
	DEFINE_DELAYED_CALL(done);
	const char *link = NULL;
	int err;

	if (S_ISLNK(stat->mode)) {
		link = vfs_get_link(lowerpath->dentry, &done);
		if (IS_ERR(link))
			return ERR_CAST(link);
	}

	slot = aufsng_create_slot(pupper, &dentry->d_name);
	if (IS_ERR(slot)) {
		do_delayed_call(&done);
		return slot;
	}

	switch (stat->mode & S_IFMT) {
	case S_IFDIR:
		upper = vfs_mkdir(idmap, d_inode(pupper), slot,
				  stat->mode, NULL);
		if (IS_ERR(upper)) {
			/* vfs_mkdir consumed the dentry and the lock */
			do_delayed_call(&done);
			return upper;
		}
		slot = upper;
		break;
	case S_IFLNK:
		err = vfs_symlink(idmap, d_inode(pupper), slot, link, NULL);
		if (err)
			goto out_end;
		break;
	default:
		err = vfs_mknod(idmap, d_inode(pupper), slot, stat->mode,
				stat->rdev, NULL);
		if (err)
			goto out_end;
		break;
	}

	upper = dget(slot);
	end_dirop(slot);
	do_delayed_call(&done);

	err = aufsng_copy_xattr(pfs, lowerpath, upper);
	if (!err)
		err = aufsng_set_attr_from(pfs, upper, stat);
	if (err) {
		/*
		 * Same cleanup duty the regular-file path gets from its
		 * temp+rename scheme: a half-attributed object left
		 * under the real name would be adopted with wrong
		 * metadata and make every retry fail with EEXIST.
		 */
		dput(upper);
		aufsng_remove_object(pfs, pupper, &dentry->d_name,
				  S_ISDIR(stat->mode));
		return ERR_PTR(err);
	}
	return upper;

out_end:
	end_dirop(slot);
	do_delayed_call(&done);
	return ERR_PTR(err);
}

/* copy up one object whose parent already has an upper dir */
static int aufsng_copy_up_one(struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	struct dentry *pupper = aufsng_upperdentry(d_inode(dentry->d_parent));
	struct aufsng_entry *oe;
	struct path lowerpath;
	struct kstat stat;
	struct dentry *upper;
	struct dentry *work = NULL;
	char tmpbuf[32];
	struct qstr tmpq;
	int err;

	if (WARN_ON(!pupper))
		return -ENOENT;
	if (READ_ONCE(oi->upperdentry))
		return 0;
	/*
	 * Best-effort early out for a name already dead (concurrent
	 * unlink/rename won): purely advisory - the same checks re-run
	 * authoritatively under oi->lock below - but it spares building
	 * and copying a full temp that the commit would only discard.
	 */
	if (d_unhashed(dentry) || !inode->i_nlink)
		return -ENOENT;

	/*
	 * The lower source is stable without oi->lock: a non-directory's
	 * stack never changes after creation, and a directory's
	 * superseded stacks stay parked on the inode until eviction.
	 */
	oe = AUFSNG_I_E(inode);
	if (!oe || !oe->numlower)
		return -ENOENT;
	lowerpath.mnt = oe->lowerstack[0].layer->mnt;
	lowerpath.dentry = oe->lowerstack[0].dentry;

	err = vfs_getattr(&lowerpath, &stat,
			  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	if (err)
		return err;

	/* the data-filled temp is built before any lock (see prep_regular) */
	if (S_ISREG(stat.mode)) {
		work = aufsng_copy_up_prep_regular(pfs, &lowerpath, pupper,
						stat.size, &tmpq, tmpbuf);
		if (IS_ERR(work))
			return PTR_ERR(work);
	}

	/* sb_writers ranks before oi->lock, as in every mutation path */
	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		goto out_tmp;

	mutex_lock(&oi->lock);
	if (oi->upperdentry)
		goto out;	/* lost the race: another copy-up committed */

	/*
	 * The lookup that led here saw the name alive, but an unlink or
	 * rename may have won oi->lock first: the whiteout it left (and
	 * the dropped link count / unhashed dentry) mark the name dead.
	 * Copying up regardless would recreate the name with full upper
	 * content - and destroy the whiteout - silently undoing a
	 * delete that already returned success to userspace.
	 */
	if (d_unhashed(dentry) || !inode->i_nlink) {
		err = -ENOENT;
		goto out;
	}
	err = aufsng_check_whiteout(aufsng_upper_mnt(pfs), pupper,
				 &dentry->d_name);
	if (err) {
		err = err < 0 ? err : -ENOENT;
		goto out;
	}

	if (work) {
		err = aufsng_copy_up_commit_regular(pfs, dentry, &lowerpath,
						 pupper, work);
		if (err)
			goto out;
		upper = work;
		work = NULL;	/* committed: live under the real name now */
	} else {
		upper = aufsng_copy_up_inplace(pfs, dentry, &lowerpath, pupper,
					    &stat);
		if (IS_ERR(upper)) {
			err = PTR_ERR(upper);
			goto out;
		}
	}

	WRITE_ONCE(oi->upperdentry, upper);
	aufsng_copyattr(inode);
out:
	mutex_unlock(&oi->lock);
	/* a temp that was not committed (failure or lost race) goes away */
	if (work) {
		aufsng_remove_object(pfs, pupper, &tmpq, false);
		dput(work);
		work = NULL;
	}
	mnt_drop_write(aufsng_upper_mnt(pfs));
out_tmp:
	if (work) {
		/*
		 * No write access (the rw branch went read-only between
		 * prep and commit).  One retry covers a flapping fs; if
		 * the branch is persistently ro no removal is possible -
		 * the invisible temp then stays until a clear_whiteouts
		 * sweep of its directory, so leave a trace for the admin.
		 */
		if (!mnt_want_write(aufsng_upper_mnt(pfs))) {
			aufsng_remove_object(pfs, pupper, &tmpq, false);
			mnt_drop_write(aufsng_upper_mnt(pfs));
		} else {
			pr_warn("aufs (aufs-ng): read-only rw branch, copy-up temp '%s' left behind\n",
				tmpbuf);
		}
		dput(work);
	}
	return err;
}

/*
 * Make sure @dentry has an upper copy, copying up any ancestors that
 * lack one first (top-down).  Write access on the rw branch is taken
 * inside the per-object helpers, NOT here for the whole walk: the
 * regular-file data copy must run outside mnt_want_write(), because
 * vfs_copy_file_range() takes the upper sb's own write protection and
 * same-level sb_writers nesting deadlocks against an upper-fs freeze.
 * The trade is that the walk is no longer atomic against a rw->ro
 * flip: ancestors copied up before the flip stay - benign, since an
 * upper dir mirroring its lower is the same state any sibling's
 * copy-up produces - and the walk still fails cleanly with EROFS.
 * Callers performing their own mutation afterwards take their own
 * mnt_want_write(), sequentially, as before.
 */
int aufsng_copy_up(struct dentry *dentry)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	const struct cred *old_cred;
	int err = 0;

	old_cred = override_creds(pfs->creator_cred);

	while (!err && !aufsng_upperdentry(d_inode(dentry))) {
		struct dentry *next = dget(dentry);

		/* find the topmost ancestor still lacking an upper */
		while (!IS_ROOT(next) &&
		       !aufsng_upperdentry(d_inode(next->d_parent))) {
			struct dentry *parent = dget(next->d_parent);

			dput(next);
			next = parent;
		}

		err = aufsng_copy_up_one(next);
		dput(next);
	}

	revert_creds(old_cred);
	return err;
}
