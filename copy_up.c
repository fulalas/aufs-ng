// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copy-up: give a lower-backed object an upper copy before the first
 * modification.  File data goes into a uniquely named temp and is
 * renamed into place, so a half-copied file is never visible under its
 * real name; everything else is created in place.  Runs with the
 * mounter's credentials, copying mode, ownership, times and xattrs.
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

/*
 * Discard the ".wh..wh.pxu" temp of a failed copy-up.  With no write
 * access left it cannot be removed at all and stays until a
 * clear_whiteouts sweep, so log it.  One policy for both failure paths.
 */
static void aufsng_discard_tmp(struct aufsng_fs *pfs, struct dentry *pupper,
			    const struct qstr *tmp, const char *tmpbuf)
{
	if (!mnt_want_write(aufsng_upper_mnt(pfs))) {
		aufsng_remove_object(pfs, pupper, tmp, false);
		mnt_drop_write(aufsng_upper_mnt(pfs));
	} else {
		pr_warn("aufs (aufs-ng): read-only rw branch, copy-up temp '%s' left behind\n",
			tmpbuf);
	}
}

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
		/* our own bookkeeping is never inherited; the commit sets it */
		if (aufsng_is_private_xattr(name))
			continue;
retry:
		size = vfs_getxattr(old_idmap, oldpath->dentry, name, value,
				    value_size);
		if (size == -ERANGE || (size > 0 && !value)) {
			void *new_value;

			/* The call above was the size probe; only -ERANGE needs a new one */
			if (size == -ERANGE)
				size = vfs_getxattr(old_idmap, oldpath->dentry,
						    name, NULL, 0);
			if (size < 0)
				continue;
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
			continue;

		err = vfs_setxattr(new_idmap, new, name, value, size, 0);
		if (err) {
			if (err == -EOPNOTSUPP || err == -EPERM)
				err = 0;	/* best effort, like a cp -a */
			else
				break;
		}
	}

out:
	kvfree(value);
	kvfree(buf);
	return err;
}

/*
 * Record on @upper that the lower called @name is its copy-up origin.
 * Best effort: a branch that will not hold the xattr just loses a
 * stable st_ino across eviction, which beats failing the copy-up.
 * Warned once, since the condition is per rw branch.
 */
static void aufsng_mark_origin(struct aufsng_fs *pfs, struct dentry *upper,
			    const struct qstr *name)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	int err = vfs_setxattr(idmap, upper, AUFSNG_XATTR_ORIGIN,
			       name->name, name->len, 0);

	if (err)
		pr_warn_once("aufs (aufs-ng): rw branch cannot hold the copy-up marker (%d); copied-up files get a fresh st_ino when their dentry is evicted\n",
			     err);
}

/*
 * Was @upper copied up under @name?  The stored name scopes an
 * inode-wide xattr to one name, so a second link or a renamed name
 * does not match.  A value too big for @buf fails the compare.
 */
bool aufsng_has_origin(struct aufsng_fs *pfs, struct dentry *upper,
		    const struct qstr *name)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	char buf[NAME_MAX];
	ssize_t len;

	len = vfs_getxattr(idmap, upper, AUFSNG_XATTR_ORIGIN, buf, sizeof(buf));
	return len == name->len && !memcmp(buf, name->name, len);
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
	 * There is no lchmod: ATTR_MODE on a symlink fails and would
	 * abort the copy-up.  Set everything else and skip the mode.
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
	 * COPY_FILE_SPLICE lets the VFS handle the cross-sb case, falling
	 * back to an in-kernel splice - no bounce buffer, and the policy
	 * stays with the VFS instead of an errno list here.  No fsync:
	 * the copy-up + rename pair has no crash consistency anyway, and
	 * flushing would serialize every copy-up on the device.
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
 * Phase one of a file copy-up, with NO locks held: create a uniquely
 * named temp in the target's own directory and fill it.  AUFS has no
 * workdir, so the temp lives in the ".wh..wh." namespace and is
 * invisible to lookup and readdir; a crash leftover is swept like any
 * other stale marker.
 *
 * It must run outside oi->lock and mnt_want_write():
 * vfs_copy_file_range() takes the upper sb's write protection, the
 * same sb_writers level, so holding it recurses and deadlocks against
 * a freeze - and sb_writers ranks before oi->lock everywhere else.
 * Nothing is committed here.
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
		aufsng_discard_tmp(pfs, pupper, tmp, tmpbuf);
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
 * lower's metadata and rename it over the real name - the commit.
 */
static int aufsng_copy_up_commit_regular(struct aufsng_fs *pfs,
				      const struct qstr *name,
				      const struct path *lowerpath,
				      struct dentry *pupper,
				      struct dentry *work)
{
	struct renamedata rd = {};
	struct qstr nameq = QSTR_LEN(name->name, name->len);
	struct kstat stat;
	int err;

	/*
	 * Re-stat now so mode, ownership and times come from the same
	 * instant as the xattrs below.  The prep-era stat only sized the
	 * data copy; the metadata should not be stitched from two
	 * instants on top of that.
	 */
	err = vfs_getattr(lowerpath, &stat,
			  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	if (!err)
		err = aufsng_copy_xattr(pfs, lowerpath, work);
	if (!err)
		err = aufsng_set_attr_from(pfs, work, &stat);
	if (err)
		return err;

	/* before the rename: the name never appears unmarked */
	aufsng_mark_origin(pfs, work, name);

	/*
	 * No whiteout can sit at the target: the name was visible to the
	 * lookup that triggered this copy-up, and one appearing since
	 * means a completed unlink, which the caller re-checks under
	 * oi->lock - the same lock remove holds for its whole sequence.
	 *
	 * vfs_rename() moves the file onto @work and leaves rd.new_dentry
	 * negative; end_renaming() drops the helper's refs.
	 */
	rd.mnt_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	rd.old_parent = pupper;
	rd.new_parent = pupper;
	err = start_renaming_dentry(&rd, 0, work, &nameq);
	if (err)
		return err;
	/*
	 * A POSITIVE target claimed the name while the copy-up ran: a
	 * rename onto it is the one mutation oi->lock cannot see, since
	 * it serializes on the VICTIM's locks.  Renaming the temp over it
	 * would replace that file with stale lower content and report
	 * success.  Abort; the opener retries against the new state.
	 */
	if (d_is_positive(rd.new_dentry)) {
		end_renaming(&rd);
		return -ESTALE;
	}
	err = vfs_rename(&rd);
	end_renaming(&rd);
	return err;
}

/*
 * Create the upper copy of a non-regular object in place.  No whiteout
 * can hold the name, for the same reason as in the file path.
 */
static struct dentry *aufsng_copy_up_inplace(struct aufsng_fs *pfs,
					  const struct qstr *name,
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

	slot = aufsng_create_slot(pupper, name);
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
	/* Directories are keyed by their merged stack; a marker is never read */
	if (!err && !S_ISDIR(stat->mode))
		aufsng_mark_origin(pfs, upper, name);
	if (err) {
		/*
		 * The cleanup the temp+rename scheme gives the file path
		 * for free: a half-attributed object under the real name
		 * would be adopted with wrong metadata and fail every
		 * retry with EEXIST.
		 *
		 * If the removal ALSO fails - likely, since whatever broke
		 * the metadata tends to break this too - the leftover is
		 * live and no sweep removes it.  Report -EIO then, as AUFS
		 * does: "nothing changed" would be a lie.
		 */
		int rerr;

		dput(upper);
		rerr = aufsng_remove_object(pfs, pupper, name,
					 S_ISDIR(stat->mode));
		return ERR_PTR(rerr ? -EIO : err);
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
	struct name_snapshot ns;
	char tmpbuf[32];
	struct qstr tmpq;
	int err;

	/*
	 * Nothing pins the parent's upper between the caller's walk and
	 * this sample: under udba=reval a lookup can shed it.  -ESTALE,
	 * not a WARN - this is reachable from an ordinary open, and the
	 * retry loop copies the parent up again.
	 */
	if (!pupper)
		return -ESTALE;
	if (READ_ONCE(oi->upperdentry))
		return 0;
	/*
	 * Advisory early out for an already dead name; the same checks
	 * re-run under oi->lock, but this spares a whole temp copy.
	 */
	if (d_unhashed(dentry) || !inode->i_nlink)
		return -ENOENT;

	/*
	 * The lower source is sampled without oi->lock: safe to READ (a
	 * superseded stack stays parked until eviction) but not
	 * guaranteed current, since a removal re-points the stack.  The
	 * commit re-reads it under oi->lock and aborts with -ESTALE, so a
	 * copy-up can never publish content from a removed branch.
	 */
	oe = AUFSNG_I_E(inode);
	if (!oe || !oe->numlower)
		return -ENOENT;
	lowerpath.mnt = oe->lowerstack[0].mnt;
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

	/*
	 * From here the name is read through a snapshot: no lock held
	 * keeps d_name stable across the sleeping operations below, and
	 * d_splice_alias() can __d_move() a directory alias, rewriting
	 * d_name or kfree_rcu()ing the external one.  Without one
	 * snapshot the whiteout re-check, the create and the
	 * compensating removal could each act on a different name - and
	 * unlink an unrelated object.
	 */
	take_dentry_name_snapshot(&ns, dentry);

	/* sb_writers ranks before oi->lock, as in every mutation path */
	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		goto out_tmp;

	mutex_lock(&oi->lock);
	if (oi->upperdentry)
		goto out;	/* lost the race: another copy-up committed */

	/*
	 * A removal re-pointed the stack while the temp was filling, so
	 * the data would come from the REMOVED branch.  Abort; the caller
	 * retries against the new stack.
	 */
	if (AUFSNG_I_E(inode) != oe) {
		err = -ESTALE;
		goto out;
	}

	/*
	 * An unlink or rename may have won oi->lock first, leaving the
	 * name dead.  Copying up regardless would recreate it and destroy
	 * the whiteout, undoing a delete that already returned success.
	 */
	if (d_unhashed(dentry) || !inode->i_nlink) {
		err = -ENOENT;
		goto out;
	}
	err = aufsng_check_whiteout(aufsng_upper_mnt(pfs), pupper, &ns.name);
	if (err) {
		err = err < 0 ? err : -ENOENT;
		goto out;
	}

	if (work) {
		err = aufsng_copy_up_commit_regular(pfs, &ns.name, &lowerpath,
						 pupper, work);
		if (err)
			goto out;
		upper = work;
		work = NULL;	/* committed: live under the real name now */
	} else {
		upper = aufsng_copy_up_inplace(pfs, &ns.name, &lowerpath, pupper,
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
		aufsng_discard_tmp(pfs, pupper, &tmpq, tmpbuf);
		dput(work);
	}
	release_dentry_name_snapshot(&ns);
	return err;
}

/*
 * Give @dentry an upper copy, copying up any ancestors that lack one
 * first.  Write access is taken inside the per-object helpers, not
 * across the walk: the data copy must run outside mnt_want_write() or
 * sb_writers nesting deadlocks against a freeze.  The trade is that
 * the walk is not atomic against a rw->ro flip - ancestors copied up
 * before it stay, which is benign, and the walk still fails EROFS.
 */
int aufsng_copy_up(struct dentry *dentry)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	const struct cred *old_cred;
	int retries = 0;
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
		/*
		 * -ESTALE: a removal re-pointed the source stack mid-copy.
		 * The retry copies the survivor; the cap guards a livelock.
		 */
		if (err == -ESTALE && ++retries <= 3)
			err = 0;
		dput(next);
	}

	revert_creds(old_cred);
	return err;
}

/*
 * Copy up @dentry and return its upper WITH A REFERENCE, the form
 * every mutation needs.
 *
 * aufsng_copy_up() alone will not do: it early-outs unlocked the
 * moment an upper exists, so a caller re-reading oi->upperdentry can
 * find the NULL a concurrent shed-upper just published, and nothing
 * the callers hold excludes that heal.
 *
 * So re-read under oi->lock and pin the result.  A shed upper stays
 * parked until eviction, so acting on one just shed is the operation
 * landing before the out-of-band unlink; only a real NULL loops back
 * into copy-up.
 */
struct dentry *aufsng_copy_up_upper(struct dentry *dentry)
{
	struct aufsng_inode *oi = AUFSNG_I(d_inode(dentry));
	int retries = 0;

	for (;;) {
		struct dentry *upper;
		int err = aufsng_copy_up(dentry);

		if (err)
			return ERR_PTR(err);

		mutex_lock(&oi->lock);
		upper = oi->upperdentry;
		if (upper)
			dget(upper);
		mutex_unlock(&oi->lock);
		if (upper)
			return upper;
		if (++retries > 3)
			return ERR_PTR(-ESTALE);
	}
}
