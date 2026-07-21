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
	 * Directories report a merged link count the way AUFS does
	 * (au_cpup_attr_nlink): the top branch's, plus each additional
	 * branch's subdirectory links with its own "."/".." discounted.
	 * find-style tools use nlink-2 as the subdirectory count.
	 */
	if (S_ISDIR(inode->i_mode)) {
		struct aufsng_entry *oe = AUFSNG_I_E(inode);
		unsigned int i = aufsng_upperdentry(inode) ? 0 : 1;

		for (; oe && i < oe->numlower; i++) {
			unsigned int n =
				d_inode(oe->lowerstack[i].dentry)->i_nlink;

			stat->nlink += n >= 2 ? n - 2 : 0;
		}
	}

	return 0;
}

static int aufsng_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		       struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	struct dentry *upper;
	int err;

	/* permission checks against the union inode, caller's creds */
	err = setattr_prepare(idmap, dentry, attr);
	if (err)
		return err;

	err = aufsng_copy_up(dentry);
	if (err)
		return err;

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return err;

	upper = aufsng_upperdentry(inode);
	attr->ia_valid &= ~(ATTR_FILE | ATTR_OPEN);

	old_cred = override_creds(pfs->creator_cred);
	inode_lock(d_inode(upper));
	err = notify_change(mnt_idmap(aufsng_upper_mnt(pfs)), upper, attr, NULL);
	inode_unlock(d_inode(upper));
	revert_creds(old_cred);

	if (!err)
		aufsng_copyattr(inode);

	mnt_drop_write(aufsng_upper_mnt(pfs));
	return err;
}

/*
 * Unlike overlayfs, AUFS reserves no xattr namespace for its own
 * bookkeeping (whiteouts and the opaque marker are plain files, not
 * xattrs - see namei.c/dir.c), so xattr passthrough needs no
 * filtering here.
 */
ssize_t aufsng_listxattr(struct dentry *dentry, char *list, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	struct path realpath;
	ssize_t res;

	aufsng_path_real(inode, &realpath);
	old_cred = override_creds(pfs->creator_cred);
	res = vfs_listxattr(realpath.dentry, list, size);
	revert_creds(old_cred);
	return res;
}

static int aufsng_xattr_get(const struct xattr_handler *handler,
			 struct dentry *dentry, struct inode *inode,
			 const char *name, void *buffer, size_t size)
{
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	struct path realpath;
	int res;

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

	err = aufsng_copy_up(dentry);
	if (err)
		return err;

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err)
		return err;

	upper = aufsng_upperdentry(inode);
	old_cred = override_creds(pfs->creator_cred);
	if (value)
		err = vfs_setxattr(upper_idmap, upper, name, value, size,
				   flags);
	else
		err = vfs_removexattr(upper_idmap, upper, name);
	revert_creds(old_cred);

	if (!err)
		aufsng_copyattr(inode);

	mnt_drop_write(aufsng_upper_mnt(pfs));
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
