// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng merged directory reading, AUFS-compatible whiteout
 * semantics.
 *
 * A directory listing is the union of branch 0 (rw) and the dentry's
 * lower branch stack, highest priority first: the first branch to
 * provide a name wins.  A name whose branch instead provides a
 * ".wh.<name>" marker is a tombstone: it is never shown, and it also
 * hides that name in every lower branch (inserted into the same
 * cache keyed on the real name, so a later, lower-priority branch's
 * real occurrence of that name is suppressed by the ordinary
 * first-one-wins rule).  Any ".wh..wh.*"-prefixed name (the opaque
 * marker and AUFS bookkeeping names aufs-ng never writes) is never
 * shown and never hides anything - it isn't a whiteout, just noise.
 *
 * The merged result is cached on the inode and validated against the
 * inode version, which every mutation and branch change bumps.  The
 * version is sampled *before* the merge: a branch change bumps it
 * without holding this dir's lock, so a version read after the merge
 * could already account for a change the merge did not see, wrongly
 * marking a stale cache valid.
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/cred.h>
#include <linux/rbtree.h>
#include <linux/iversion.h>
#include "aufsng.h"

struct aufsng_cache_entry {
	struct rb_node node;
	struct list_head l_node;
	u64 ino;
	unsigned int d_type;
	bool hidden;	/* tombstone: never emitted, blocks lower branches */
	int len;
	char name[];
};

struct aufsng_dir_cache {
	long refcount;
	u64 version;
	/*
	 * The upper (rw) directory's change stamp when this listing was
	 * built.  Under udba=reval it lets an out-of-band edit of the rw
	 * branch (e.g. a hand-removed whiteout - the only way the merged
	 * view can change without bumping @version) invalidate the cache
	 * without re-reading every branch on each open.  i_version is the
	 * tick-independent signal where the branch fs supports it; mtime is
	 * the always-present fallback.  {0,0}/0 means no upper.
	 */
	struct timespec64 upper_mtime;
	u64 upper_iversion;
	struct list_head entries;
	struct rb_root root;
};

struct aufsng_dir_file {
	struct aufsng_dir_cache *cache;
};

struct aufsng_readdir_data {
	struct dir_context ctx;
	struct aufsng_dir_cache *cache;
	unsigned int idx;	/* branch slot of the layer being read */
	int count;
	int err;
};

/* ".wh..wh." double-prefix: opaque marker + AUFS bookkeeping names */
static bool aufsng_is_wh_bookkeeping(const char *name, int len)
{
	return len >= 2 * AUFSNG_WH_PFX_LEN &&
	       !memcmp(name, AUFSNG_WH_PFX AUFSNG_WH_PFX, 2 * AUFSNG_WH_PFX_LEN);
}

static struct aufsng_cache_entry *aufsng_cache_entry_find(struct rb_root *root,
						    const char *name, int len)
{
	struct rb_node *node = root->rb_node;
	int cmp;

	while (node) {
		struct aufsng_cache_entry *p =
			rb_entry(node, struct aufsng_cache_entry, node);

		cmp = strncmp(name, p->name, len);
		if (!cmp)
			cmp = len - p->len;
		if (cmp > 0)
			node = node->rb_right;
		else if (cmp < 0)
			node = node->rb_left;
		else
			return p;
	}
	return NULL;
}

static void aufsng_cache_entry_insert(struct rb_root *root,
				   struct aufsng_cache_entry *n)
{
	struct rb_node **newp = &root->rb_node;
	struct rb_node *parent = NULL;
	int cmp;

	while (*newp) {
		struct aufsng_cache_entry *p =
			rb_entry(*newp, struct aufsng_cache_entry, node);

		parent = *newp;
		cmp = strncmp(n->name, p->name, n->len);
		if (!cmp)
			cmp = n->len - p->len;
		if (cmp > 0)
			newp = &(*newp)->rb_right;
		else
			newp = &(*newp)->rb_left;
	}

	rb_link_node(&n->node, parent, newp);
	rb_insert_color(&n->node, root);
}

static int aufsng_cache_add(struct aufsng_dir_cache *cache, const char *name,
			 int namelen, u64 ino, unsigned int d_type,
			 bool hidden)
{
	struct aufsng_cache_entry *p;

	/* the first (highest priority) branch to claim a name wins,
	 * whether that claim is a real entry or a whiteout tombstone
	 */
	if (aufsng_cache_entry_find(&cache->root, name, namelen))
		return 0;

	p = kmalloc(sizeof(*p) + namelen + 1, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	memcpy(p->name, name, namelen);
	p->name[namelen] = '\0';
	p->len = namelen;
	p->ino = ino;
	p->d_type = d_type;
	p->hidden = hidden;

	aufsng_cache_entry_insert(&cache->root, p);
	list_add_tail(&p->l_node, &cache->entries);
	return 0;
}

static bool aufsng_fill_merge(struct dir_context *ctx, const char *name,
			   int namelen, loff_t offset, u64 ino,
			   unsigned int d_type)
{
	struct aufsng_readdir_data *rdd =
		container_of(ctx, struct aufsng_readdir_data, ctx);
	int err;

	if (name[0] == '.' && (namelen == 1 ||
			       (namelen == 2 && name[1] == '.')))
		return true;

	if (aufsng_is_wh_bookkeeping(name, namelen))
		return true;	/* opaque marker or bookkeeping: invisible,
				 * hides nothing */

	if (aufsng_is_wh_name(name, namelen)) {
		const char *real = name + AUFSNG_WH_PFX_LEN;
		int reallen = namelen - AUFSNG_WH_PFX_LEN;

		err = aufsng_cache_add(rdd->cache, real, reallen, 0, 0, true);
	} else {
		/*
		 * Fold the branch slot into the inode number, exactly as the
		 * stat/getattr path does (aufsng_map_ino), so a file's d_ino
		 * from readdir matches its st_ino and cannot collide with a
		 * same-numbered inode in another branch under the one union
		 * st_dev.
		 */
		err = aufsng_cache_add(rdd->cache, name, namelen,
				    aufsng_map_ino(ino, rdd->idx), d_type,
				    false);
	}

	if (err) {
		rdd->err = err;
		return false;
	}
	rdd->count++;
	return true;
}

static int aufsng_dir_read_layer(struct aufsng_fs *pfs, const struct path *realpath,
			      struct aufsng_dir_cache *cache, unsigned int idx)
{
	struct aufsng_readdir_data rdd = {
		.ctx.actor = aufsng_fill_merge,
		.ctx.count = INT_MAX,
		.cache = cache,
		.idx = idx,
	};
	struct file *realfile;
	int err;

	realfile = dentry_open(realpath, O_RDONLY | O_DIRECTORY | O_LARGEFILE,
			       pfs->creator_cred);
	if (IS_ERR(realfile))
		return PTR_ERR(realfile);

	/*
	 * A single iterate_dir() call is not guaranteed to reach the
	 * end of the directory; keep calling until a call adds nothing.
	 */
	do {
		rdd.count = 0;
		rdd.err = 0;
		err = iterate_dir(realfile, &rdd.ctx);
		if (!err)
			err = rdd.err;
	} while (!err && rdd.count);

	fput(realfile);
	return err;
}

static void aufsng_cache_free(struct aufsng_dir_cache *cache)
{
	struct aufsng_cache_entry *p, *n;

	list_for_each_entry_safe(p, n, &cache->entries, l_node)
		kfree(p);
	kfree(cache);
}

static void aufsng_cache_put(struct aufsng_dir_cache *cache)
{
	if (cache && !--cache->refcount)
		aufsng_cache_free(cache);
}

/* drop the inode's cache attachment; called from inode teardown too */
void aufsng_dir_cache_release(struct aufsng_inode *oi)
{
	aufsng_cache_put(oi->cache);
	oi->cache = NULL;
}

static struct aufsng_dir_cache *aufsng_cache_build(struct inode *inode)
{
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	struct aufsng_entry *oe;
	struct aufsng_dir_cache *cache;
	const struct cred *old_cred;
	struct path realpath;
	struct dentry *upper;
	unsigned int i;
	u64 version;
	int err = 0;

	cache = kzalloc(sizeof(*cache), GFP_KERNEL);
	if (!cache)
		return ERR_PTR(-ENOMEM);
	cache->refcount = 1;
	INIT_LIST_HEAD(&cache->entries);
	cache->root = RB_ROOT;

	/*
	 * dyn_lock excludes branch changes for the whole merge, so no
	 * stack this loop touches can be swapped or freed while it
	 * runs, and the pre-sampled version cannot go stale unseen.
	 */
	down_read(&pfs->dyn_lock);
	version = atomic64_read(&AUFSNG_I(inode)->version);
	old_cred = override_creds(pfs->creator_cred);

	upper = aufsng_upperdentry(inode);
	/*
	 * Sample the upper mtime before reading it: if a concurrent edit
	 * lands during the read, the stored stamp stays older than the now-
	 * current mtime, so the next reuse check rebuilds rather than trust
	 * a listing that might have half-caught the change.
	 */
	cache->upper_mtime = upper ? inode_get_mtime(d_inode(upper)) :
				     (struct timespec64){0};
	cache->upper_iversion = upper ? inode_query_iversion(d_inode(upper)) : 0;
	if (upper) {
		realpath.mnt = aufsng_upper_mnt(pfs);
		realpath.dentry = upper;
		err = aufsng_dir_read_layer(pfs, &realpath, cache, 0);
	}

	oe = AUFSNG_I_E(inode);
	for (i = 0; !err && oe && i < oe->numlower; i++) {
		realpath.mnt = oe->lowerstack[i].layer->mnt;
		realpath.dentry = oe->lowerstack[i].dentry;
		err = aufsng_dir_read_layer(pfs, &realpath, cache,
					 oe->lowerstack[i].layer->idx);
	}

	revert_creds(old_cred);
	up_read(&pfs->dyn_lock);

	if (err) {
		aufsng_cache_free(cache);
		return ERR_PTR(err);
	}

	cache->version = version;
	return cache;
}

/*
 * Is @cache still consistent with the branches?  The caller has already
 * checked @version (in-union mutations).  Under udba=reval also require
 * the upper directory's mtime to be unchanged - the only signal an out-
 * of-band rw-branch edit leaves without bumping @version.  Read under
 * oi->lock so the sampled upper matches the cache being validated.
 */
static bool aufsng_dir_cache_fresh(struct aufsng_fs *pfs, struct inode *inode,
				   struct aufsng_dir_cache *cache)
{
	struct dentry *upper;
	struct timespec64 cur_mtime;
	u64 cur_iversion;

	if (!aufsng_udba_reval(pfs))
		return true;
	upper = aufsng_upperdentry(inode);
	cur_mtime = upper ? inode_get_mtime(d_inode(upper)) :
			    (struct timespec64){0};
	cur_iversion = upper ? inode_query_iversion(d_inode(upper)) : 0;
	return cur_iversion == cache->upper_iversion &&
	       timespec64_equal(&cur_mtime, &cache->upper_mtime);
}

static struct aufsng_dir_cache *aufsng_cache_get(struct file *file)
{
	struct inode *inode = file_inode(file);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_dir_file *od = file->private_data;
	struct aufsng_dir_cache *cache;

	if (od->cache)
		return od->cache;

	mutex_lock(&oi->lock);
	cache = oi->cache;
	/*
	 * Reuse the cached listing while it is version-valid (no in-union
	 * mutation since it was built) and, under udba=reval, the rw branch
	 * has not been edited out-of-band.  This makes the common "nothing
	 * changed" open O(1) instead of re-reading every branch each time.
	 */
	if (cache && cache->version == atomic64_read(&oi->version) &&
	    aufsng_dir_cache_fresh(pfs, inode, cache)) {
		cache->refcount++;
	} else {
		mutex_unlock(&oi->lock);
		cache = aufsng_cache_build(inode);
		if (IS_ERR(cache))
			return cache;
		mutex_lock(&oi->lock);
		/* attach as the inode's cache, superseding any old one */
		aufsng_dir_cache_release(oi);
		oi->cache = cache;
		cache->refcount++;	/* the inode's reference */
	}
	mutex_unlock(&oi->lock);

	od->cache = cache;
	return cache;
}

static void aufsng_dir_reset(struct file *file)
{
	struct inode *inode = file_inode(file);
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_dir_file *od = file->private_data;

	if (od->cache && od->cache->version != atomic64_read(&oi->version)) {
		mutex_lock(&oi->lock);
		aufsng_cache_put(od->cache);
		mutex_unlock(&oi->lock);
		od->cache = NULL;
	}
}

static int aufsng_iterate(struct file *file, struct dir_context *ctx)
{
	struct aufsng_dir_cache *cache;
	struct aufsng_cache_entry *p;
	loff_t off = 2;

	if (!ctx->pos)
		aufsng_dir_reset(file);

	if (!dir_emit_dots(file, ctx))
		return 0;

	cache = aufsng_cache_get(file);
	if (IS_ERR(cache))
		return PTR_ERR(cache);

	/*
	 * Offsets 2.. index the (stable, immutable once built) merged
	 * list; hidden tombstone entries consume an offset so cookies
	 * stay valid across getdents calls on the same cache.
	 */
	list_for_each_entry(p, &cache->entries, l_node) {
		if (off++ < ctx->pos)
			continue;
		if (!p->hidden &&
		    !dir_emit(ctx, p->name, p->len, p->ino, p->d_type))
			break;
		ctx->pos = off;
	}
	return 0;
}

/* is the merged view of @dentry empty (whiteouts aside)? */
int aufsng_check_empty_dir(struct dentry *dentry)
{
	struct aufsng_dir_cache *cache;
	struct aufsng_cache_entry *p;
	int err = 0;

	cache = aufsng_cache_build(d_inode(dentry));
	if (IS_ERR(cache))
		return PTR_ERR(cache);

	list_for_each_entry(p, &cache->entries, l_node) {
		if (!p->hidden) {
			err = -ENOTEMPTY;
			break;
		}
	}

	aufsng_cache_free(cache);
	return err;
}

/*
 * Remove every ".wh."-prefixed bookkeeping name (per-entry whiteouts
 * and the opaque marker) physically present in the rw branch's own
 * @upperdir, so that a union-empty merged dir (see
 * aufsng_check_empty_dir()) can actually be vfs_rmdir'ed - rmdir fails
 * on a directory that still physically contains these marker files,
 * even though they are invisible to the merged view.  This is a raw
 * scan of @upperdir alone, not the merged cache.
 */
int aufsng_clear_whiteouts(struct aufsng_fs *pfs, struct dentry *upperdir)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct aufsng_dir_cache *cache;
	struct aufsng_cache_entry *p, *n;
	struct path realpath = {
		.mnt = aufsng_upper_mnt(pfs),
		.dentry = upperdir,
	};
	int err;

	cache = kzalloc(sizeof(*cache), GFP_KERNEL);
	if (!cache)
		return -ENOMEM;
	INIT_LIST_HEAD(&cache->entries);
	cache->root = RB_ROOT;

	/*
	 * aufsng_dir_read_layer()/aufsng_fill_merge() classify entries into
	 * real names and whiteout tombstones (storing the tombstone's
	 * derived real name, not its on-disk ".wh."+name form); that
	 * classification is exactly what we need here too.  Re-derive
	 * each tombstone's on-disk name to unlink it.
	 * Only tombstone names are inspected below, so the mapped inode
	 * numbers are irrelevant here - any idx is fine.
	 */
	err = aufsng_dir_read_layer(pfs, &realpath, cache, 0);
	if (err) {
		aufsng_cache_free(cache);
		return err;
	}

	list_for_each_entry_safe(p, n, &cache->entries, l_node) {
		char buf[NAME_MAX + AUFSNG_WH_PFX_LEN + 1];
		struct qstr wh;
		struct dentry *slot;

		if (!p->hidden)
			continue;

		memcpy(buf, AUFSNG_WH_PFX, AUFSNG_WH_PFX_LEN);
		memcpy(buf + AUFSNG_WH_PFX_LEN, p->name, p->len);
		wh = QSTR_LEN(buf, AUFSNG_WH_PFX_LEN + p->len);

		slot = start_removing_noperm(upperdir, &wh);
		if (IS_ERR(slot)) {
			err = PTR_ERR(slot);
			continue;
		}
		if (d_is_positive(slot))
			err = vfs_unlink(idmap, d_inode(upperdir), slot, NULL);
		end_dirop(slot);
	}

	/* the opaque marker, if this directory carries one */
	{
		struct qstr opq = QSTR(AUFSNG_WH_DIROPQ);
		struct dentry *slot;

		slot = start_removing_noperm(upperdir, &opq);
		if (!IS_ERR(slot)) {
			if (d_is_positive(slot))
				vfs_unlink(idmap, d_inode(upperdir), slot,
					   NULL);
			end_dirop(slot);
		}
	}

	aufsng_cache_free(cache);
	return err;
}

static loff_t aufsng_dir_llseek(struct file *file, loff_t offset, int whence)
{
	return generic_file_llseek_size(file, offset, whence, LLONG_MAX,
					LLONG_MAX);
}

static int aufsng_dir_open(struct inode *inode, struct file *file)
{
	struct aufsng_dir_file *od;

	od = kzalloc(sizeof(*od), GFP_KERNEL);
	if (!od)
		return -ENOMEM;

	file->private_data = od;
	return 0;
}

static int aufsng_dir_release(struct inode *inode, struct file *file)
{
	struct aufsng_dir_file *od = file->private_data;
	struct aufsng_inode *oi = AUFSNG_I(inode);

	if (od->cache) {
		mutex_lock(&oi->lock);
		aufsng_cache_put(od->cache);
		mutex_unlock(&oi->lock);
	}
	kfree(od);
	return 0;
}

static int aufsng_dir_fsync(struct file *file, loff_t start, loff_t end,
			 int datasync)
{
	return 0;
}

const struct file_operations aufsng_dir_operations = {
	.read		= generic_read_dir,
	.open		= aufsng_dir_open,
	.release	= aufsng_dir_release,
	.llseek		= aufsng_dir_llseek,
	.iterate_shared	= aufsng_iterate,
	.fsync		= aufsng_dir_fsync,
};
