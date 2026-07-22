// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng superblock and module setup.  Registers filesystem type
 * "aufs" (see aufs-ng.h) so real AUFS mount/remount invocations work
 * unmodified, and so the kernel's standard "mount -t aufs" module
 * auto-loading (request_module("fs-aufs")) picks up this module
 * instead of requiring any initrd/script changes.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/cred.h>
#include "aufsng.h"

MODULE_AUTHOR("aufs-ng contributors");
MODULE_DESCRIPTION("aufs-ng - standalone AUFS-compatible union filesystem for live Linux distributions");
MODULE_LICENSE("GPL");
MODULE_ALIAS_FS(AUFSNG_NAME);

static struct kmem_cache *aufsng_inode_cachep;

struct aufsng_entry *aufsng_alloc_entry(unsigned int numlower)
{
	struct aufsng_entry *oe;

	oe = kzalloc(struct_size(oe, lowerstack, numlower), GFP_KERNEL);
	if (oe)
		oe->numlower = numlower;
	return oe;
}

static void aufsng_stack_put(struct aufsng_path *stack, unsigned int n)
{
	unsigned int i;

	for (i = 0; stack && i < n; i++)
		dput(stack[i].dentry);
}

void aufsng_free_entry(struct aufsng_entry *oe)
{
	if (!oe)
		return;
	aufsng_stack_put(oe->lowerstack, oe->numlower);
	kfree(oe);
}

/* inode lifecycle */

static struct inode *aufsng_alloc_inode(struct super_block *sb)
{
	struct aufsng_inode *oi = alloc_inode_sb(sb, aufsng_inode_cachep, GFP_KERNEL);

	if (!oi)
		return NULL;

	oi->oe = NULL;
	oi->upperdentry = NULL;
	oi->cache = NULL;
	oi->dyn_parked = NULL;
	atomic64_set(&oi->version, 0);
	mutex_init(&oi->lock);

	return &oi->vfs_inode;
}

static void aufsng_destroy_inode(struct inode *inode)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);

	dput(oi->upperdentry);
	aufsng_dyn_put_parked(oi);
	if (oi->oe)
		aufsng_stack_put(oi->oe->lowerstack, oi->oe->numlower);
	if (S_ISDIR(inode->i_mode))
		aufsng_dir_cache_release(oi);
}

static void aufsng_free_inode(struct inode *inode)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);

	/*
	 * The aufsng_entry struct itself is freed here, an RCU grace
	 * period after eviction: lockless readers of AUFSNG_I_E() may
	 * still be dereferencing it (its dentry references were
	 * already dropped in aufsng_destroy_inode()).
	 */
	aufsng_dyn_free_parked(oi);
	kfree(oi->oe);
	mutex_destroy(&oi->lock);
	kmem_cache_free(aufsng_inode_cachep, oi);
}

/* superblock operations */

static int aufsng_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	struct path path;
	int err;

	/* branch 0 (rw) may be swapped by a runtime add/remove */
	down_read(&pfs->dyn_lock);
	path.mnt = aufsng_upper_mnt(pfs);
	path.dentry = path.mnt->mnt_root;
	err = vfs_statfs(&path, buf);
	up_read(&pfs->dyn_lock);

	if (!err) {
		buf->f_namelen = pfs->namelen;
		buf->f_type = AUFSNG_SUPER_MAGIC;
	}
	return err;
}

static int aufsng_show_options(struct seq_file *m, struct dentry *dentry)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);
	struct aufsng_entry *oe;
	unsigned int i;

	/*
	 * The full branch list in priority order, as real AUFS prints
	 * it without sysfs: branch 0 (rw) first, then the root stack
	 * top-down.  This is the only place branch PRIORITY is visible
	 * to userspace (the branches' own mounts show membership, not
	 * order).  Long lines are safe for every consumer PorteuX
	 * actually runs: boot-time module adds pass explicit options
	 * (no replay), the live system's mount is util-linux, and the
	 * shutdown script only ever umounts - all verified against the
	 * real scripts and BusyBox 1.37 under strace.  (BusyBox's
	 * getmntent does truncate >1K lines on bare-mountpoint option
	 * REPLAY, but the parse ignores a replayed br: on remount, so
	 * even a manual `busybox mount -o remount` in an initrd rescue
	 * shell cannot feed a truncated branch list back in.)
	 */
	down_read(&pfs->dyn_lock);
	seq_puts(m, ",br:");
	seq_escape(m, pfs->config.br_paths[0], " \t\n\\");
	seq_printf(m, "=%s", pfs->config.br_perms[0]);
	oe = AUFSNG_E(dentry);
	for (i = 0; oe && i < oe->numlower; i++) {
		unsigned int idx = oe->lowerstack[i].layer->idx;

		seq_putc(m, ':');
		seq_escape(m, pfs->config.br_paths[idx], " \t\n\\");
		seq_printf(m, "=%s", pfs->config.br_perms[idx]);
	}
	up_read(&pfs->dyn_lock);
	if (pfs->config.xino_path)
		seq_show_option(m, "xino", pfs->config.xino_path);
	switch (pfs->config.udba) {
	case AUFSNG_UDBA_NOTIFY:
		seq_puts(m, ",udba=notify");
		break;
	case AUFSNG_UDBA_REVAL:
		seq_puts(m, ",udba=reval");
		break;
	default:
		seq_puts(m, ",udba=none");
		break;
	}
	return 0;
}

static void aufsng_free_fs(struct aufsng_fs *pfs)
{
	unsigned int i;

	for (i = 0; i < pfs->numlayer; i++) {
		if (pfs->layers[i].mnt)
			kern_unmount(pfs->layers[i].mnt);
		if (pfs->config.br_paths)
			kfree(pfs->config.br_paths[i]);
	}
	kfree(pfs->layers);
	kfree(pfs->config.br_paths);
	kfree(pfs->config.br_perms);
	if (pfs->creator_cred)
		put_cred(pfs->creator_cred);
	kfree(pfs->config.xino_path);
	kfree(pfs);
}

static void aufsng_put_super(struct super_block *sb)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);

	if (pfs)
		aufsng_free_fs(pfs);
}

const struct super_operations aufsng_super_operations = {
	.alloc_inode	= aufsng_alloc_inode,
	.destroy_inode	= aufsng_destroy_inode,
	.free_inode	= aufsng_free_inode,
	.put_super	= aufsng_put_super,
	.statfs		= aufsng_statfs,
	.show_options	= aufsng_show_options,
};

/* branch validation, shared with dynlayer.c (no fs_context there) */
int aufsng_check_layer(struct super_block *sb, const struct path *path,
		    const char *name)
{
	if (!d_is_dir(path->dentry)) {
		pr_err("aufs: %s is not a directory\n", name);
		return -ENOTDIR;
	}
	/*
	 * Branches whose dentries need revalidation (remote fs) are not
	 * supported: revalidation of the real dentries is skipped, on
	 * the same grounds as the dynamic-layers overlayfs patch this
	 * design was ported from.
	 */
	if (path->dentry->d_flags & (DCACHE_OP_REVALIDATE |
				     DCACHE_OP_WEAK_REVALIDATE)) {
		pr_err("aufs: %s is on a remote filesystem, not supported\n",
		       name);
		return -EINVAL;
	}
	if (path->mnt->mnt_sb->s_stack_depth >= sb->s_stack_depth) {
		pr_err("aufs: %s exceeds the maximum stacking depth\n", name);
		return -ELOOP;
	}
	return 0;
}

/*
 * The whiteout probe for a name turns it into ".wh.<name>" in the
 * same branch directory (see aufsng_wh_name()), so the advertised name
 * limit must leave room for that prefix, and must be the SMALLEST
 * limit across branches, not the largest - a name a shallower branch
 * cannot even store must not be accepted just because a deeper one
 * could.
 */
static int aufsng_get_namelen(struct aufsng_fs *pfs, const struct path *path)
{
	struct kstatfs statfs;
	long namelen;
	int err = vfs_statfs(path, &statfs);

	if (err)
		return err;
	namelen = statfs.f_namelen - AUFSNG_WH_PFX_LEN;
	pfs->namelen = min(pfs->namelen, max(0L, namelen));
	return 0;
}

int aufsng_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct aufsng_fs_context *ctx = fc->fs_private;
	struct aufsng_fs *pfs;
	struct aufsng_entry *oe = NULL;
	struct vfsmount *mnt;
	struct inode *root_inode;
	struct inode *rw_inode;
	struct dentry *root;
	int depth;
	unsigned int i;
	int err;

	if (!ctx->nr) {
		errorfc(fc, "br: with at least one rw branch is required");
		return -EINVAL;
	}
	if (ctx->br[0].perm != AUFSNG_BR_RW) {
		errorfc(fc, "the first branch (index 0) must be 'rw'");
		return -EINVAL;
	}

	err = -ENOMEM;
	pfs = kzalloc(sizeof(*pfs), GFP_KERNEL);
	if (!pfs)
		return err;
	sb->s_fs_info = pfs;
	init_rwsem(&pfs->dyn_lock);
	pfs->config.maxbranch = max_t(size_t, ctx->nr, AUFSNG_MAXBRANCH_DEF);
	pfs->config.xino_path = ctx->config.xino_path;
	ctx->config.xino_path = NULL;
	pfs->config.udba = ctx->config.udba;

	pfs->numlayer_cap = pfs->config.maxbranch + 1;
	pfs->layers = kcalloc(pfs->numlayer_cap, sizeof(*pfs->layers),
			      GFP_KERNEL);
	if (!pfs->layers)
		goto out_free;
	pfs->config.br_paths = kcalloc(pfs->numlayer_cap, sizeof(char *),
				       GFP_KERNEL);
	if (!pfs->config.br_paths)
		goto out_free;
	pfs->config.br_perms = kcalloc(pfs->numlayer_cap, AUFSNG_PERM_LEN,
				       GFP_KERNEL);
	if (!pfs->config.br_perms)
		goto out_free;

	pfs->creator_cred = prepare_creds();
	if (!pfs->creator_cred)
		goto out_free;
	aufsng_backing_ctx_init(pfs);
	pfs->namelen = NAME_MAX;

	depth = 0;
	for (i = 0; i < ctx->nr; i++)
		depth = max(depth, ctx->br[i].path.mnt->mnt_sb->s_stack_depth);
	sb->s_stack_depth = depth + 1;
	if (sb->s_stack_depth > FILESYSTEM_MAX_STACK_DEPTH) {
		errorfc(fc, "maximum fs stacking depth exceeded");
		err = -ELOOP;
		goto out_free;
	}

	for (i = 0; i < ctx->nr; i++) {
		err = aufsng_check_layer(sb, &ctx->br[i].path, ctx->br[i].name);
		if (err)
			goto out_free;
		err = aufsng_get_namelen(pfs, &ctx->br[i].path);
		if (err)
			goto out_free;

		mnt = clone_private_mount(&ctx->br[i].path);
		err = PTR_ERR(mnt);
		if (IS_ERR(mnt)) {
			errorfc(fc, "failed to clone branch '%s'",
				ctx->br[i].name);
			goto out_free;
		}
		/*
		 * Only branch 0 is ever written (see README: "only the
		 * first branch can be" rw); a lower branch declared rw is
		 * accepted for AUFS grammar compatibility but its private
		 * clone is read-only like every other lower.
		 */
		if (i > 0 || ctx->br[i].perm != AUFSNG_BR_RW)
			mnt->mnt_flags |= MNT_READONLY | MNT_NOATIME;

		pfs->layers[i].mnt = mnt;
		pfs->layers[i].idx = i;
		pfs->config.br_paths[i] = ctx->br[i].name;
		ctx->br[i].name = NULL;
		strscpy(pfs->config.br_perms[i], ctx->br[i].permstr,
			AUFSNG_PERM_LEN);
		pfs->numlayer++;
	}

	sb->s_magic = AUFSNG_SUPER_MAGIC;
	sb->s_op = &aufsng_super_operations;
	set_default_d_op(sb, &aufsng_dentry_operations);
	sb->s_xattr = aufsng_xattr_handlers;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_time_gran = ctx->br[0].path.dentry->d_sb->s_time_gran;

	err = -ENOMEM;
	oe = aufsng_alloc_entry(pfs->numlayer - 1);
	if (!oe)
		goto out_free;
	for (i = 1; i < pfs->numlayer; i++) {
		oe->lowerstack[i - 1].layer = &pfs->layers[i];
		oe->lowerstack[i - 1].dentry =
			dget(pfs->layers[i].mnt->mnt_root);
	}

	root_inode = new_inode(sb);
	if (!root_inode)
		goto out_free;

	rw_inode = d_inode(aufsng_upper_mnt(pfs)->mnt_root);
	/* AUFS mounts always have root inode 2; scripts test for it */
	root_inode->i_ino = AUFSNG_ROOT_INO;
	root_inode->i_mode = rw_inode->i_mode;
	root_inode->i_uid = rw_inode->i_uid;
	root_inode->i_gid = rw_inode->i_gid;
	root_inode->i_op = &aufsng_dir_inode_operations;
	root_inode->i_fop = &aufsng_dir_operations;
	set_nlink(root_inode, rw_inode->i_nlink);
	simple_inode_init_ts(root_inode);

	AUFSNG_I(root_inode)->oe = oe;
	AUFSNG_I(root_inode)->upperdentry = dget(aufsng_upper_mnt(pfs)->mnt_root);
	oe = NULL;

	root = d_make_root(root_inode);
	if (!root)
		goto out_free;

	sb->s_root = root;
	return 0;

out_free:
	aufsng_free_entry(oe);
	aufsng_free_fs(pfs);
	sb->s_fs_info = NULL;
	return err;
}

static struct file_system_type aufsng_fs_type = {
	.owner			= THIS_MODULE,
	.name			= AUFSNG_NAME,
	.init_fs_context	= aufsng_init_fs_context,
	.parameters		= aufsng_parameter_spec,
	.kill_sb		= kill_anon_super,
};

static void aufsng_inode_init_once(void *data)
{
	struct aufsng_inode *oi = data;

	inode_init_once(&oi->vfs_inode);
}

static int __init aufsng_init(void)
{
	int err;

	aufsng_inode_cachep = kmem_cache_create("aufsng_inode",
					     sizeof(struct aufsng_inode), 0,
					     SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
					     aufsng_inode_init_once);
	if (!aufsng_inode_cachep)
		return -ENOMEM;

	err = register_filesystem(&aufsng_fs_type);
	if (err) {
		kmem_cache_destroy(aufsng_inode_cachep);
		return err;
	}
	return 0;
}

static void __exit aufsng_exit(void)
{
	unregister_filesystem(&aufsng_fs_type);
	/* wait out RCU-delayed inode frees before destroying the cache */
	rcu_barrier();
	kmem_cache_destroy(aufsng_inode_cachep);
}

module_init(aufsng_init);
module_exit(aufsng_exit);
