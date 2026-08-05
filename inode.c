// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng inode operations: attribute and xattr passthrough to the
 * real object, with copy-up on the first modification.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/cred.h>
#include <linux/xattr.h>
#include <linux/stat.h>
#include <linux/rcupdate.h>
#include "aufsng.h"

static int aufsng_getattr(struct mnt_idmap *idmap, const struct path *path,
		       struct kstat *stat, u32 request_mask,
		       unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	struct path realpath;
	int err;

	aufsng_path_real(inode, &realpath);
	old_cred = override_creds(pfs->creator_cred);
	err = vfs_getattr(&realpath, stat, request_mask, flags);
	revert_creds(old_cred);
	if (err)
		return err;

	stat->dev = inode->i_sb->s_dev;
	stat->ino = inode->i_ino;
	/*
	 * Merged dir link count, as AUFS (au_cpup_attr_nlink): top branch
	 * plus each other branch's, less its own "."/"..".
	 */
	if (S_ISDIR(inode->i_mode)) {
		struct aufsng_entry *oe;
		unsigned int i;

		/* RCU: the root's superseded entry is freed after a grace period */
		rcu_read_lock();
		oe = AUFSNG_I_E(inode);
		i = aufsng_upperdentry(inode) ? 0 : 1;
		for (; oe && i < oe->numlower; i++) {
			unsigned int n =
				d_inode(oe->lowerstack[i].dentry)->i_nlink;

			stat->nlink += n >= 2 ? n - 2 : 0;
		}
		rcu_read_unlock();
	}

	return 0;
}

static int aufsng_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		       struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	struct inode *winode = NULL;
	struct dentry *upper;
	int err;

	/* permission checks against the union inode, caller's creds */
	err = setattr_prepare(idmap, dentry, attr);
	if (err)
		return err;

	/* pinned: a concurrent shed-upper heal must not NULL it under us */
	upper = aufsng_copy_up_upper(dentry);
	if (IS_ERR(upper))
		return PTR_ERR(upper);

	/*
	 * Truncate must respect the write block a direct exec from the
	 * branch holds on the UPPER inode; the caller's VFS only checked
	 * the union's (as ovl_setattr).
	 */
	if (attr->ia_valid & ATTR_SIZE) {
		winode = d_inode(upper);
		err = get_write_access(winode);
		if (err) {
			dput(upper);
			return err;
		}
	}

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err) {
		if (winode)
			put_write_access(winode);
		dput(upper);
		return err;
	}

	/*
	 * notify_change() already turned ATTR_KILL_S*ID into an ATTR_MODE
	 * for the union inode and handed both down, but it BUG()s on that
	 * pair.  Drop the mode and let the upper's own notify_change derive
	 * it from the upper mode, which is the one being changed.  One
	 * corner diverges, and overlayfs shares it: the re-derivation runs
	 * as the mounter, so a chgrp into a group the CALLER left never
	 * clears S_ISGID on a non-group-executable file.
	 */
	if (attr->ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID))
		attr->ia_valid &= ~ATTR_MODE;

	attr->ia_valid &= ~(ATTR_FILE | ATTR_OPEN);

	old_cred = override_creds(pfs->creator_cred);
	inode_lock(d_inode(upper));
	err = notify_change(mnt_idmap(aufsng_upper_mnt(pfs)), upper, attr, NULL);
	inode_unlock(d_inode(upper));
	revert_creds(old_cred);

	if (!err)
		aufsng_copyattr_from(inode, d_inode(upper));

	mnt_drop_write(aufsng_upper_mnt(pfs));
	if (winode)
		put_write_access(winode);
	dput(upper);
	return err;
}

/*
 * Whiteouts are files, not xattrs, so the passthrough below only has to
 * hide aufs-ng's own namespace (AUFSNG_XATTR_PFX) - from listings,
 * reads, writes and cp -a alike.
 */
static ssize_t aufsng_listxattr(struct dentry *dentry, char *list, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	struct path realpath;
	char *from, *to, *end;
	ssize_t res;

	aufsng_path_real(inode, &realpath);
	old_cred = override_creds(pfs->creator_cred);
	res = vfs_listxattr(realpath.dentry, list, size);
	revert_creds(old_cred);
	/* A size probe may over-report; the VFS expects an upper bound */
	if (res <= 0 || !list)
		return res;

	for (from = list, to = list, end = list + res; from < end;) {
		size_t len = strlen(from) + 1;

		if (!aufsng_is_private_xattr(from)) {
			if (to != from)
				memmove(to, from, len);
			to += len;
		}
		from += len;
	}
	return to - list;
}

static int aufsng_xattr_get(const struct xattr_handler *handler,
			 struct dentry *dentry, struct inode *inode,
			 const char *name, void *buffer, size_t size)
{
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	struct path realpath;
	int res;

	if (aufsng_is_private_xattr(name))
		return -ENODATA;

	aufsng_path_real(inode, &realpath);
	old_cred = override_creds(pfs->creator_cred);
	res = vfs_getxattr(mnt_idmap(realpath.mnt), realpath.dentry, name,
			   buffer, size);
	revert_creds(old_cred);
	return res;
}

static int aufsng_xattr_set(const struct xattr_handler *handler,
			 struct mnt_idmap *idmap, struct dentry *dentry,
			 struct inode *inode, const char *name,
			 const void *value, size_t size, int flags)
{
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	struct mnt_idmap *upper_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	const struct cred *old_cred;
	struct dentry *upper;
	int err;

	/* forging it would let a file claim another's identity */
	if (aufsng_is_private_xattr(name))
		return -EPERM;

	/* pinned: a concurrent shed-upper heal must not NULL it under us */
	upper = aufsng_copy_up_upper(dentry);
	if (IS_ERR(upper))
		return PTR_ERR(upper);

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err) {
		dput(upper);
		return err;
	}

	old_cred = override_creds(pfs->creator_cred);
	if (value)
		err = vfs_setxattr(upper_idmap, upper, name, value, size,
				   flags);
	else
		err = vfs_removexattr(upper_idmap, upper, name);
	revert_creds(old_cred);

	if (!err)
		aufsng_copyattr_from(inode, d_inode(upper));

	mnt_drop_write(aufsng_upper_mnt(pfs));
	dput(upper);
	return err;
}

static const struct xattr_handler aufsng_all_xattr_handler = {
	.prefix	= "",
	.get	= aufsng_xattr_get,
	.set	= aufsng_xattr_set,
};

const struct xattr_handler * const aufsng_xattr_handlers[] = {
	&aufsng_all_xattr_handler,
	NULL,
};

const struct inode_operations aufsng_dir_inode_operations = {
	.lookup		= aufsng_lookup,
	.create		= aufsng_create,
	.mkdir		= aufsng_mkdir,
	.mknod		= aufsng_mknod,
	.symlink	= aufsng_symlink,
	.link		= aufsng_link,
	.unlink		= aufsng_unlink,
	.rmdir		= aufsng_rmdir,
	.rename		= aufsng_rename,
	.getattr	= aufsng_getattr,
	.setattr	= aufsng_setattr,
	.listxattr	= aufsng_listxattr,
};

const struct inode_operations aufsng_file_inode_operations = {
	.getattr	= aufsng_getattr,
	.setattr	= aufsng_setattr,
	.listxattr	= aufsng_listxattr,
};

const struct inode_operations aufsng_symlink_inode_operations = {
	.get_link	= aufsng_get_link,
	.getattr	= aufsng_getattr,
	.setattr	= aufsng_setattr,
	.listxattr	= aufsng_listxattr,
};

const struct inode_operations aufsng_special_inode_operations = {
	.getattr	= aufsng_getattr,
	.setattr	= aufsng_setattr,
	.listxattr	= aufsng_listxattr,
};
