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

/*
 * Discard the invisible ".wh..wh.pxu" temp of a failed or superseded
 * copy-up.  With no write access (the rw branch went read-only between
 * prep and commit) the temp cannot be removed at all - it then stays
 * until a clear_whiteouts sweep of its directory, so leave a trace for
 * the admin.  The one policy for both the prep-side and commit-side
 * failure paths.
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

			/*
			 * With no buffer yet the call above was already
			 * the size probe; only -ERANGE (the value grew
			 * past the buffer) needs a fresh one.
			 */
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
 *
 * Best effort: a branch fs that will not hold the xattr simply leaves
 * the object unmarked, which costs it a stable st_ino across cache
 * eviction and nothing else - far better than failing the copy-up over
 * bookkeeping.  Unlike the cp -a of a lower's own xattrs above, though,
 * EOPNOTSUPP is not silently fine here: it means every copy-up on this
 * branch loses that stability, which the admin should hear about.  One
 * line for the module either way - the condition is per rw branch, and
 * a mount has exactly one.
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
 * Was @upper copied up under @name?  The stored name is what scopes an
 * inode-wide xattr to one name: a second link to the same inode, or the
 * same inode carrying a name it was renamed to, does not match and so
 * claims no origin.  A value that does not fit @buf comes back as
 * -ERANGE and fails the compare, like any other mismatch.
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
 * lower's metadata and rename it over the real name - the commit
 * point.  On success @work is hashed under the final name and is the
 * live upper.
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

	/* before the rename: the name never appears unmarked */
	aufsng_mark_origin(pfs, work, name);

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
	/*
	 * A POSITIVE target is a foreign object that claimed the name
	 * while the copy-up ran: rename(2) moving another file onto it
	 * is the one mutation oi->lock cannot see coming (the rename
	 * serializes on the VICTIM's locks, not this inode's).  Renaming
	 * the temp over it would replace that freshly renamed file with
	 * this inode's stale lower content - silent data loss reported
	 * as success.  Abort instead; the caller cleans up the temp and
	 * the opener retries against the new state of the name.
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
 * Create the upper copy of a non-regular object directly in the rw
 * parent dir.  No whiteout can occupy the name here for the same
 * reason as in the regular-file path: the name was visible to the
 * lookup that led here, and copy_up_one aborts under oi->lock if a
 * delete slipped in since.
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
	/*
	 * Directories are keyed by their merged stack, not by an origin,
	 * so nothing ever reads a marker on one - leave it off rather
	 * than carry a claim no code path honours.
	 */
	if (!err && !S_ISDIR(stat->mode))
		aufsng_mark_origin(pfs, upper, name);
	if (err) {
		/*
		 * Same cleanup duty the regular-file path gets from its
		 * temp+rename scheme: a half-attributed object left
		 * under the real name would be adopted with wrong
		 * metadata and make every retry fail with EEXIST.
		 *
		 * When the compensating removal ALSO fails - likely, since
		 * whatever broke the metadata (a full or failing branch)
		 * tends to break this too - the leftover is live under the
		 * real name with the mounter's ownership and none of the
		 * lower's xattrs, and no sweep removes it (clear_whiteouts
		 * only knows ".wh." names).  Report -EIO rather than the
		 * original errno then: "ENOSPC, nothing changed" would be a
		 * lie about a branch that now needs the admin.  This is
		 * AUFS's own escalation in au_cpup_single()'s error path.
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
	 * The caller's walk saw the parent's upper, but nothing pins it
	 * between that check and this sample: under udba=reval a lookup of
	 * the parent can shed a just-created upper dir that was removed
	 * out-of-band (aufsng_dyn_shed_upper), which no lock here excludes.
	 * -ESTALE, not a WARN: this is reachable from an ordinary open, and
	 * aufsng_copy_up()'s retry loop re-drives the ancestor walk and
	 * copies the parent up again - the heal, rather than a panic on the
	 * panic_on_warn kernels this ships on.
	 */
	if (!pupper)
		return -ESTALE;
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
	 * The lower source is sampled without oi->lock - safe to READ
	 * (a superseded stack stays parked on the inode until eviction,
	 * its dentries and mounts pinned) but no longer guaranteed
	 * CURRENT: a branch removal re-points even a non-directory's
	 * stack to a surviving branch (aufsng_dyn_prep_repoint).  The
	 * commit below re-reads the stack under oi->lock - which the
	 * re-point's publisher (aufsng_dyn_commit_rebuild) also takes -
	 * and aborts with -ESTALE if this sample went stale, so a
	 * copy-up can never publish content from a branch whose removal
	 * already reported success.
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
	 * From here the name is read through a snapshot, exactly as
	 * aufsng_dyn_prep_repoint() does and for the same reason: neither
	 * oi->lock nor the caller's locks keep @dentry's d_name stable
	 * across the sleeping branch operations below.  d_splice_alias() can
	 * __d_move() a DIRECTORY alias (same-parent __d_unalias takes none
	 * of the locks held here) when a fresh lookup keys the same cached
	 * inode - after an out-of-band rename inside the branch, or after a
	 * failed revalidation unhashed this alias - which rewrites d_name in
	 * place or kfree_rcu()s the external one.  Worse than a torn read:
	 * the whiteout re-check, the create and the compensating removal
	 * would each act on whatever the name happened to be at that
	 * instant, so a failed copy-up could unlink an unrelated upper
	 * object.  One snapshot makes all three agree on one string.
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
	 * A branch removal re-pointed the stack while the temp was being
	 * filled: the data (and the metadata snapshot taken at commit)
	 * would come from the REMOVED branch, moving the file's content
	 * backwards after the removal reported success.  Abort; the
	 * caller retries against the new stack.
	 */
	if (AUFSNG_I_E(inode) != oe) {
		err = -ESTALE;
		goto out;
	}

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
		 * -ESTALE: a branch removal re-pointed the source stack
		 * mid-copy.  The loop re-samples the (new) stack, so a
		 * retry copies the surviving branch's content; branch
		 * changes are rare, the cap only guards a livelock.
		 */
		if (err == -ESTALE && ++retries <= 3)
			err = 0;
		dput(next);
	}

	revert_creds(old_cred);
	return err;
}

/*
 * Copy up @dentry and hand back its upper WITH A REFERENCE - the form
 * every mutation that then operates on the upper needs.
 *
 * aufsng_copy_up() alone is not enough for that: it early-outs the
 * moment an upper exists, taking no lock, so a caller re-reading
 * oi->upperdentry afterwards can find the NULL a concurrent
 * aufsng_dyn_shed_upper() just published (udba=reval, out-of-band
 * unlink of the rw copy) and dereference it.  Nothing the callers hold
 * excludes that heal: it runs from a lookup, under the parent's
 * i_rwsem and only oi->lock.
 *
 * So re-read under oi->lock and pin the result; a shed upper stays
 * parked (and thus valid) until the inode is evicted, so acting on one
 * shed a moment ago is just the operation landing before the
 * out-of-band unlink.  Only a genuine NULL loops back into copy-up,
 * which recreates the upper - the same converge-by-retry protocol
 * aufsng_copy_up() already uses for -ESTALE.
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
