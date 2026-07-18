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
		.ia_valid = ATTR_UID | ATTR_GID | ATTR_MODE |
			    ATTR_ATIME | ATTR_MTIME | ATTR_ATIME_SET |
			    ATTR_MTIME_SET | ATTR_FORCE,
		.ia_uid = stat->uid,
		.ia_gid = stat->gid,
		.ia_mode = stat->mode,
		.ia_atime = stat->atime,
		.ia_mtime = stat->mtime,
	};
	int err;

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
	void *buf = NULL;
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

	while (pos_in < len) {
		ssize_t bytes;

		bytes = vfs_copy_file_range(in, pos_in, out, pos_out,
					    len - pos_in, 0);
		if (bytes == -EOPNOTSUPP || bytes == -EXDEV ||
		    bytes == -ENOSYS || bytes == -EINVAL) {
			/* plain read/write fallback, 64k at a time */
			size_t chunk = min_t(loff_t, len - pos_in, SZ_64K);

			if (!buf) {
				buf = kvmalloc(SZ_64K, GFP_KERNEL);
				if (!buf) {
					err = -ENOMEM;
					break;
				}
			}
			bytes = kernel_read(in, buf, chunk, &pos_in);
			if (bytes > 0)
				bytes = kernel_write(out, buf, bytes,
						     &pos_out);
			if (bytes <= 0) {
				err = bytes < 0 ? bytes : -EIO;
				break;
			}
			continue;
		}
		if (bytes <= 0) {
			err = bytes < 0 ? bytes : -EIO;
			break;
		}
		pos_in += bytes;
		pos_out += bytes;
	}

	if (!err)
		err = vfs_fsync(out, 0);

	kvfree(buf);
	fput(out);
	fput(in);
	return err;
}

/*
 * Copy a regular file into a uniquely named hidden file inside the
 * SAME directory as the final target, fill in data and metadata,
 * then rename it over the real name.  AUFS has no separate workdir;
 * the temp name is never ".wh."-prefixed, so it can never be mistaken
 * for whiteout bookkeeping, and it is always cleaned up (renamed away
 * or unlinked) within this single call - no external script ever
 * observes it, since copy-up only runs from live filesystem
 * operations, not from the shutdown-time raw directory scan.
 */
static struct dentry *aufsng_copy_up_regular(struct aufsng_fs *pfs,
					  struct dentry *dentry,
					  const struct path *lowerpath,
					  struct dentry *pupper,
					  const struct kstat *stat)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct renamedata rd = {};
	struct dentry *work;
	struct qstr nameq = QSTR_LEN(dentry->d_name.name, dentry->d_name.len);
	char tmpname[32];
	struct qstr tmpq;
	int err;

	snprintf(tmpname, sizeof(tmpname), "#pxu%u",
		 atomic_inc_return(&aufsng_tmpfile_seq));
	tmpq = QSTR(tmpname);

	work = start_creating_noperm(pupper, &tmpq);
	if (IS_ERR(work))
		return work;
	if (d_is_positive(work)) {
		end_dirop(work);
		return ERR_PTR(-EEXIST);
	}
	err = vfs_create(idmap, work, S_IFREG | 0600, NULL);
	if (err) {
		end_dirop(work);
		return ERR_PTR(err);
	}
	dget(work);
	end_dirop(work);

	err = aufsng_copy_data(pfs, lowerpath, work, stat->size);
	if (!err)
		err = aufsng_copy_xattr(pfs, lowerpath, work);
	if (!err)
		err = aufsng_set_attr_from(pfs, work, stat);
	if (err)
		goto out_cleanup;

	/*
	 * A stale ".wh.<name>" may still sit at the target from an
	 * earlier delete of a name a lower branch also provides; the
	 * rename below only replaces "<name>" itself, so the whiteout
	 * must be cleared first or it would keep hiding the file we are
	 * about to give it real upper content for.
	 */
	err = aufsng_remove_whiteout(pfs, pupper, &dentry->d_name);
	if (err)
		goto out_cleanup;

	rd.mnt_idmap = idmap;
	rd.old_parent = pupper;
	rd.new_parent = pupper;
	err = start_renaming_dentry(&rd, 0, work, &nameq);
	if (err)
		goto out_cleanup;
	err = vfs_rename(&rd);
	end_renaming(&rd);
	if (err)
		goto out_cleanup;

	/*
	 * vfs_rename() moves the file onto @work (the source dentry) via
	 * d_move() and leaves rd.new_dentry negative - end_renaming() drops
	 * the helper's own refs.  @work, now hashed under the final name and
	 * carrying the inode, is the live upper; hand it the ref taken above.
	 */
	return work;

out_cleanup:
	/* best effort removal of the temporary file */
	{
		struct dentry *w = start_removing_noperm(pupper, &tmpq);

		if (!IS_ERR(w)) {
			vfs_unlink(idmap, d_inode(pupper), w, NULL);
			end_dirop(w);
		}
	}
	dput(work);
	return ERR_PTR(err);
}

/*
 * Create the upper copy of a non-regular object directly in the rw
 * parent dir.  A stale whiteout for this name (from an earlier
 * delete of a name a lower branch also provides) is a SEPARATE
 * ".wh.<name>" sibling under AUFS semantics, not a property of the
 * name's own (negative) dentry, so it is removed up front rather
 * than detected via the create slot itself.
 */
static struct dentry *aufsng_copy_up_inplace(struct aufsng_fs *pfs,
					  struct dentry *dentry,
					  const struct path *lowerpath,
					  struct dentry *pupper,
					  const struct kstat *stat)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct dentry *slot, *upper = NULL;
	struct qstr nameq = QSTR_LEN(dentry->d_name.name, dentry->d_name.len);
	DEFINE_DELAYED_CALL(done);
	const char *link = NULL;
	int err;

	if (S_ISLNK(stat->mode)) {
		link = vfs_get_link(lowerpath->dentry, &done);
		if (IS_ERR(link))
			return ERR_CAST(link);
	}

	err = aufsng_remove_whiteout(pfs, pupper, &dentry->d_name);
	if (err) {
		do_delayed_call(&done);
		return ERR_PTR(err);
	}

	slot = start_creating_noperm(pupper, &nameq);
	if (IS_ERR(slot)) {
		do_delayed_call(&done);
		return slot;
	}
	if (d_is_positive(slot)) {
		err = -EEXIST;
		goto out_end;
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
		dput(upper);
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
	int err = 0;

	if (WARN_ON(!pupper))
		return -ENOENT;

	mutex_lock(&oi->lock);
	if (oi->upperdentry)
		goto out;

	oe = AUFSNG_I_E(inode);
	if (!oe || !oe->numlower) {
		err = -ENOENT;
		goto out;
	}
	lowerpath.mnt = oe->lowerstack[0].layer->mnt;
	lowerpath.dentry = oe->lowerstack[0].dentry;

	err = vfs_getattr(&lowerpath, &stat,
			  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	if (err)
		goto out;

	if (S_ISREG(stat.mode))
		upper = aufsng_copy_up_regular(pfs, dentry, &lowerpath, pupper,
					    &stat);
	else
		upper = aufsng_copy_up_inplace(pfs, dentry, &lowerpath, pupper,
					    &stat);
	if (IS_ERR(upper)) {
		err = PTR_ERR(upper);
		goto out;
	}

	WRITE_ONCE(oi->upperdentry, upper);
	aufsng_copyattr(inode);
out:
	mutex_unlock(&oi->lock);
	return err;
}

/*
 * Make sure @dentry has an upper copy, copying up any ancestors that
 * lack one first (top-down).  Takes write access on the rw branch
 * itself: callers that go on to perform their own mutation there
 * (create, rename, setattr, ...) additionally bracket that with their
 * own mnt_want_write(), which nests safely on top of this one.
 */
int aufsng_copy_up(struct dentry *dentry)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	const struct cred *old_cred;
	int err;

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return err;

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
	mnt_drop_write(aufsng_upper_mnt(pfs));
	return err;
}
