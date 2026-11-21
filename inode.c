// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aufs-ng inode operations: attribute and xattr passthrough to the
 * real object, with copy-up on the first modification.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/cred.h>
#include <linux/xattr.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/stat.h>
#include <linux/rcupdate.h>
#include "aufsng.h"

static int aufsng_getattr(AUFSNG_IDMAP *idmap, const struct path *path,
		       struct kstat *stat, u32 request_mask,
		       unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	struct path realpath;
	int err;

	aufsng_path_real_get(inode, &realpath);
	old_cred = override_creds(pfs->creator_cred);
	err = vfs_getattr(&realpath, stat, request_mask, flags);
	revert_creds(old_cred);
	aufsng_path_real_put(inode, &realpath);
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

		/* RCU: a directory's superseded entry is freed after a grace period */
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

static int aufsng_setattr(AUFSNG_IDMAP *idmap, struct dentry *dentry,
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

	aufsng_path_real_get(inode, &realpath);
	old_cred = override_creds(pfs->creator_cred);
	res = vfs_listxattr(realpath.dentry, list, size);
	revert_creds(old_cred);
	aufsng_path_real_put(inode, &realpath);
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

	aufsng_path_real_get(inode, &realpath);
	old_cred = override_creds(pfs->creator_cred);
	res = vfs_getxattr(mnt_idmap(realpath.mnt), realpath.dentry, name,
			   buffer, size);
	revert_creds(old_cred);
	aufsng_path_real_put(inode, &realpath);
	return res;
}

static int aufsng_xattr_set(const struct xattr_handler *handler,
			 AUFSNG_IDMAP *idmap, struct dentry *dentry,
			 struct inode *inode, const char *name,
			 const void *value, size_t size, int flags)
{
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	AUFSNG_IDMAP *upper_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
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

#ifdef CONFIG_FS_POSIX_ACL
static void aufsng_idmap_acl(struct inode *realinode, AUFSNG_IDMAP *idmap,
			     struct posix_acl *acl)
{
	struct user_namespace *fs_userns = i_user_ns(realinode);
	unsigned int i;

	for (i = 0; i < acl->a_count; i++) {
		struct posix_acl_entry *e = &acl->a_entries[i];

		if (e->e_tag == ACL_USER)
			e->e_uid = vfsuid_into_kuid(make_vfsuid(idmap, fs_userns,
								e->e_uid));
		else if (e->e_tag == ACL_GROUP)
			e->e_gid = vfsgid_into_kgid(make_vfsgid(idmap, fs_userns,
								e->e_gid));
	}
}

struct posix_acl *aufsng_get_acl_path(const struct path *path, int type,
				      bool noperm)
{
	struct inode *realinode = d_inode(path->dentry);
	struct posix_acl *acl, *clone;

	if (noperm)
		acl = get_inode_acl(realinode, type);
	else
		acl = vfs_get_acl(mnt_idmap(path->mnt), path->dentry,
				  posix_acl_xattr_name(type));
	if (acl == ERR_PTR(-ENODATA) || acl == ERR_PTR(-EOPNOTSUPP))
		return NULL;
	if (IS_ERR_OR_NULL(acl) || !is_idmapped_mnt(path->mnt))
		return acl;

	clone = posix_acl_clone(acl, GFP_KERNEL);
	posix_acl_release(acl);
	if (!clone)
		return ERR_PTR(-ENOMEM);
	aufsng_idmap_acl(realinode, mnt_idmap(path->mnt), clone);
	return clone;
}

static struct posix_acl *aufsng_do_get_acl(struct inode *inode, int type,
					   bool noperm)
{
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	struct posix_acl *acl = NULL;
	struct path realpath;

	aufsng_path_real_get(inode, &realpath);
	if (realpath.dentry) {
		old_cred = override_creds(pfs->creator_cred);
		acl = aufsng_get_acl_path(&realpath, type, noperm);
		revert_creds(old_cred);
	}
	aufsng_path_real_put(inode, &realpath);
	return acl;
}

static struct posix_acl *aufsng_get_inode_acl(struct inode *inode, int type,
					      bool rcu)
{
	struct inode *realinode;
	struct path realpath;

	if (!rcu)
		return aufsng_do_get_acl(inode, type, true);

	aufsng_path_real(inode, &realpath);
	realinode = realpath.dentry ? d_inode_rcu(realpath.dentry) : NULL;
	if (!realinode)
		return ERR_PTR(-ECHILD);
	if (!IS_POSIXACL(realinode))
		return NULL;
	if (is_idmapped_mnt(realpath.mnt))
		return ERR_PTR(-ECHILD);
	return get_cached_acl_rcu(realinode, type);
}

static struct posix_acl *aufsng_get_acl(AUFSNG_IDMAP *idmap,
					struct dentry *dentry, int type)
{
	return aufsng_do_get_acl(d_inode(dentry), type, false);
}

static int aufsng_set_acl(AUFSNG_IDMAP *idmap, struct dentry *dentry,
			  struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	AUFSNG_IDMAP *upper_idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	const char *name = posix_acl_xattr_name(type);
	const struct cred *old_cred;
	struct dentry *upper;
	bool kill_sgid;
	int err;

	if (!IS_POSIXACL(d_inode(aufsng_upper_mnt(pfs)->mnt_root)))
		return -EOPNOTSUPP;

	if (!acl && !aufsng_upperdentry(inode)) {
		struct posix_acl *real = aufsng_do_get_acl(inode, type, false);

		if (IS_ERR(real))
			return PTR_ERR(real);
		if (!real)
			return -ENODATA;
		posix_acl_release(real);
	}

	kill_sgid = type == ACL_TYPE_ACCESS && (inode->i_mode & S_ISGID) &&
		    !in_group_or_capable(idmap, inode,
					 i_gid_into_vfsgid(idmap, inode));

	upper = aufsng_copy_up_upper(dentry);
	if (IS_ERR(upper))
		return PTR_ERR(upper);

	err = mnt_want_write(aufsng_upper_mnt(pfs));
	if (err) {
		dput(upper);
		return err;
	}

	old_cred = override_creds(pfs->creator_cred);
	if (acl)
		err = vfs_set_acl(upper_idmap, upper, name, acl);
	else
		err = vfs_remove_acl(upper_idmap, upper, name);
	revert_creds(old_cred);

	if (!err)
		aufsng_copyattr_from(inode, d_inode(upper));

	mnt_drop_write(aufsng_upper_mnt(pfs));
	dput(upper);
	if (!err && kill_sgid) {
		struct iattr attr = { .ia_valid = ATTR_KILL_SGID };

		err = aufsng_setattr(&nop_mnt_idmap, dentry, &attr);
	}
	return err;
}
#else
#define aufsng_get_inode_acl	NULL
#define aufsng_get_acl		NULL
#define aufsng_set_acl		NULL
#endif

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
	.get_inode_acl	= aufsng_get_inode_acl,
	.get_acl	= aufsng_get_acl,
	.set_acl	= aufsng_set_acl,
};

const struct inode_operations aufsng_file_inode_operations = {
	.getattr	= aufsng_getattr,
	.setattr	= aufsng_setattr,
	.listxattr	= aufsng_listxattr,
	.get_inode_acl	= aufsng_get_inode_acl,
	.get_acl	= aufsng_get_acl,
	.set_acl	= aufsng_set_acl,
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
	.get_inode_acl	= aufsng_get_inode_acl,
	.get_acl	= aufsng_get_acl,
	.set_acl	= aufsng_set_acl,
};
