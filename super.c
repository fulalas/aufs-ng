// SPDX-License-Identifier: GPL-2.0-only
/*
 * Superblock and module setup.  Registers filesystem type "aufs", so
 * real AUFS mount commands and "mount -t aufs" auto-loading both find
 * this module unchanged.
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

/*
 * Source of the "si=" id, one per mount.  A counter, not AUFS's masked
 * sbinfo address: same uniqueness, no kernel address on show.
 */
static atomic_long_t aufsng_si_last = ATOMIC_LONG_INIT(0);

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

/*
 * Wrap a copy-up origin into the 0-or-1-entry stack that keys a
 * non-directory inode.  ALWAYS consumes *@origin - the reference moves
 * into the entry or is dropped - so the handoff has one owner.
 */
struct aufsng_entry *aufsng_entry_from_origin(int found,
					struct aufsng_path *origin)
{
	struct aufsng_entry *oe = aufsng_alloc_entry(found ? 1 : 0);

	if (!oe) {
		dput(origin->dentry);
		origin->dentry = NULL;
		return ERR_PTR(-ENOMEM);
	}
	if (found) {
		oe->lowerstack[0] = *origin;
		origin->dentry = NULL;
	}
	return oe;
}

/*
 * Put @dentry (in @layer, via @mnt) on top of @cur's first @base_n
 * entries, one reference per copied dentry.  @mnt is passed in because
 * the add path builds the stack before publishing the slot.  No mntget:
 * a live stack's mounts are pinned by the branch itself.
 */
struct aufsng_entry *aufsng_entry_prepend(struct aufsng_entry *cur,
				     unsigned int base_n,
				     struct aufsng_layer *layer,
				     struct dentry *dentry,
				     struct vfsmount *mnt)
{
	struct aufsng_entry *neu = aufsng_alloc_entry(1 + base_n);
	unsigned int i;

	if (!neu)
		return NULL;
	neu->lowerstack[0].layer = layer;
	neu->lowerstack[0].dentry = dget(dentry);
	neu->lowerstack[0].mnt = mnt;
	for (i = 0; i < base_n; i++) {
		neu->lowerstack[i + 1] = cur->lowerstack[i];
		dget(neu->lowerstack[i + 1].dentry);
	}
	return neu;
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
	/*
	 * Stack dentries drop BEFORE the parked mount pins: a
	 * deleted-but-open object's stack is pinned only by the parked
	 * node, so the other order tears the branch down under it.
	 */
	if (oi->oe)
		aufsng_stack_put(oi->oe->lowerstack, oi->oe->numlower);
	aufsng_dyn_put_parked(oi);
	if (S_ISDIR(inode->i_mode))
		aufsng_dir_cache_release(oi);
}

static void aufsng_free_inode(struct inode *inode)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);

	/* A grace period after eviction: AUFSNG_I_E() readers are lockless */
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

	/* No lock: branch 0 is written once, at mount; adds touch slots >= 1 */
	path.mnt = aufsng_upper_mnt(pfs);
	path.dentry = path.mnt->mnt_root;
	err = vfs_statfs(&path, buf);

	if (!err) {
		buf->f_namelen = pfs->namelen;
		buf->f_type = AUFSNG_SUPER_MAGIC;
	}
	return err;
}

static int aufsng_show_options(struct seq_file *m, struct dentry *dentry)
{
	struct aufsng_fs *pfs = AUFSNG_FS(dentry->d_sb);

	/*
	 * The mount id, and no branch list - as AUFS, which prints "br:"
	 * only with its sysfs tree off.  Hundreds of branches would push
	 * the line past 4 KB and break readers with fixed line buffers.
	 */
	seq_printf(m, ",si=%lx", pfs->si_id);
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

/*
 * Make slot @idx exist and return its branch, growing the slot table if
 * needed.  Table and branches live until umount, so a slot freed by a
 * removal - or left behind by a failed add - is reused, not leaked.
 * Callers hold sb->s_umount exclusively.
 */
struct aufsng_layer *aufsng_layer_reserve(struct aufsng_fs *pfs,
					  unsigned int idx)
{
	struct aufsng_layer **layers, *layer;
	unsigned int cap;

	if (idx >= AUFSNG_MAXBRANCH) {
		pr_err("aufs: too many branches, limit is %d\n",
		       AUFSNG_MAXBRANCH);
		return ERR_PTR(-ENOSPC);
	}

	if (idx >= pfs->numlayer_cap) {
		cap = min_t(unsigned int, max(2 * pfs->numlayer_cap, idx + 1),
			    AUFSNG_MAXBRANCH);
		/* kvcalloc: a table at the ceiling is past kmalloc's comfort */
		layers = kvcalloc(cap, sizeof(*layers), GFP_KERNEL);
		if (!layers)
			return ERR_PTR(-ENOMEM);
		if (pfs->layers)
			memcpy(layers, pfs->layers,
			       pfs->numlayer_cap * sizeof(*layers));
		kvfree(pfs->layers);
		pfs->layers = layers;
		pfs->numlayer_cap = cap;
	}

	if (!pfs->layers[idx]) {
		layer = kzalloc(sizeof(*layer), GFP_KERNEL);
		if (!layer)
			return ERR_PTR(-ENOMEM);
		layer->idx = idx;
		pfs->layers[idx] = layer;
	}
	return pfs->layers[idx];
}

static void aufsng_free_fs(struct aufsng_fs *pfs)
{
	struct vfsmount **mnts;
	unsigned int i, nr = 0;

	/*
	 * All branches behind ONE grace period: kern_unmount() waits for
	 * one per call, which is seconds with hundreds of branches.  The
	 * vector is optional - without it the slower path still works.
	 */
	mnts = kvmalloc_array(pfs->numlayer_cap, sizeof(*mnts), GFP_KERNEL);
	for (i = 0; i < pfs->numlayer_cap; i++) {
		struct aufsng_layer *layer = pfs->layers[i];

		if (!layer)
			continue;
		if (layer->mnt) {
			if (mnts)
				mnts[nr++] = layer->mnt;
			else
				kern_unmount(layer->mnt);
		}
		kfree(layer->path);
		kfree(layer);
	}
	if (nr)
		kern_unmount_array(mnts, nr);
	kvfree(mnts);
	kvfree(pfs->layers);
	/* safe on a never-initialized (kzalloc'd) lock, by design */
	percpu_free_rwsem(&pfs->dyn_lock);
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
	/* Branches needing dentry revalidation (remote fs) are unsupported */
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
 * No branch may equal or nest inside another (AUFS's test_overlap):
 * nested, a whiteout in one branch is a live marker in the other, so
 * one deletion hides an unrelated name.  is_subdir() answers ancestry
 * within one sb; different filesystems cannot overlap.
 */
int aufsng_check_overlap(struct aufsng_fs *pfs, struct dentry *dentry,
		      const char *name)
{
	unsigned int i;

	for (i = 0; i < pfs->numlayer; i++) {
		struct vfsmount *mnt = pfs->layers[i]->mnt;

		if (mnt && (is_subdir(dentry, mnt->mnt_root) ||
			    is_subdir(mnt->mnt_root, dentry))) {
			pr_err("aufs: %s overlaps an existing branch\n", name);
			return -EINVAL;
		}
	}
	return 0;
}

/*
 * Room for @need elements of @elemsize in *@arr, doubling from a floor
 * of 16.  One growth policy for every caller; they differ only in
 * allocation context, which @gfp carries.
 */
int aufsng_grow_array(void **arr, size_t *cap, size_t need, size_t elemsize,
		   gfp_t gfp)
{
	size_t nr;
	void *p;

	if (need <= *cap)
		return 0;
	nr = max_t(size_t, 16, *cap * 2);
	if (nr < need)
		nr = need;
	p = krealloc_array(*arr, nr, elemsize, gfp);
	if (!p)
		return -ENOMEM;
	*arr = p;
	*cap = nr;
	return 0;
}

/*
 * A whiteout adds ".wh." to the name, so the advertised limit must
 * leave room for the prefix - and must be the SMALLEST across
 * branches, or a name no shallow branch can store gets accepted.
 */
int aufsng_probe_namelen(const struct path *path, long *namelen)
{
	struct kstatfs statfs;
	int err = vfs_statfs(path, &statfs);

	if (err)
		return err;
	*namelen = max(0L, (long)statfs.f_namelen - AUFSNG_WH_PFX_LEN);
	return 0;
}

int aufsng_get_namelen(struct aufsng_fs *pfs, const struct path *path)
{
	long namelen;
	int err = aufsng_probe_namelen(path, &namelen);

	if (err)
		return err;
	pfs->namelen = min(pfs->namelen, namelen);
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
	/* Declaring branch 0 rw is not enough: EROFS later would be worse */
	if (sb_rdonly(ctx->br[0].path.mnt->mnt_sb) ||
	    (ctx->br[0].path.mnt->mnt_flags & MNT_READONLY)) {
		errorfc(fc, "the rw branch '%s' is on a read-only filesystem or mount",
			ctx->br[0].name);
		return -EROFS;
	}

	err = -ENOMEM;
	pfs = kzalloc(sizeof(*pfs), GFP_KERNEL);
	if (!pfs)
		return err;
	sb->s_fs_info = pfs;
	/* per-cpu, so unlike init_rwsem() this one allocates and can fail */
	err = percpu_init_rwsem(&pfs->dyn_lock);
	if (err)
		goto out_free;
	err = -ENOMEM;
	atomic_long_set(&pfs->branch_gen, 0);
	pfs->si_id = atomic_long_inc_return(&aufsng_si_last);
	pfs->config.xino_path = ctx->config.xino_path;
	ctx->config.xino_path = NULL;
	pfs->config.udba = ctx->config.udba;

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
		struct aufsng_layer *layer;

		err = aufsng_check_layer(sb, &ctx->br[i].path, ctx->br[i].name);
		if (err)
			goto out_free;
		/* against the branches already cloned into layers[0..i-1] */
		err = aufsng_check_overlap(pfs, ctx->br[i].path.dentry,
					ctx->br[i].name);
		if (err)
			goto out_free;
		err = aufsng_get_namelen(pfs, &ctx->br[i].path);
		if (err)
			goto out_free;

		layer = aufsng_layer_reserve(pfs, i);
		if (IS_ERR(layer)) {
			err = PTR_ERR(layer);
			goto out_free;
		}

		mnt = clone_private_mount(&ctx->br[i].path);
		err = PTR_ERR(mnt);
		if (IS_ERR(mnt)) {
			errorfc(fc, "failed to clone branch '%s'",
				ctx->br[i].name);
			goto out_free;
		}
		/*
		 * Only branch 0 is ever written; a lower "=rw" is accepted
		 * for AUFS grammar but cloned read-only anyway.
		 */
		if (i > 0 || ctx->br[i].perm != AUFSNG_BR_RW)
			mnt->mnt_flags |= MNT_READONLY | MNT_NOATIME;

		layer->mnt = mnt;
		layer->path = ctx->br[i].name;
		ctx->br[i].name = NULL;
		/* The only notice: no branch mode is echoed to userspace. */
		if (i > 0 && ctx->br[i].perm == AUFSNG_BR_RW)
			pr_warn("aufs (aufs-ng): branch '%s' declared rw but only the first branch is writable; using ro\n",
				layer->path);
		pfs->numlayer++;
	}
	pfs->upper = pfs->layers[0];

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
		struct aufsng_layer *layer = pfs->layers[i];

		oe->lowerstack[i - 1].layer = layer;
		oe->lowerstack[i - 1].dentry = dget(layer->mnt->mnt_root);
		oe->lowerstack[i - 1].mnt = layer->mnt;
	}

	root_inode = new_inode(sb);
	if (!root_inode)
		goto out_free;

	rw_inode = d_inode(aufsng_upper_mnt(pfs)->mnt_root);
	/* AUFS mounts always have root inode 2; scripts test for it */
	root_inode->i_ino = AUFSNG_ROOT_INO;
	/*
	 * The shared mirror helper, as every other inode uses: an
	 * open-coded copy here left the root reporting mount-time
	 * timestamps.  The merged dir link count is computed in getattr.
	 */
	aufsng_copyattr_from(root_inode, rw_inode);
	root_inode->i_op = &aufsng_dir_inode_operations;
	root_inode->i_fop = &aufsng_dir_operations;

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
