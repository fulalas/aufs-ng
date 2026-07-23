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
#include <linux/rcupdate.h>
#include "aufsng.h"

struct aufsng_cache_entry {
	struct rb_node node;
	struct list_head l_node;
	u64 ino;
	unsigned int d_type;
	unsigned short idx;	/* branch slot that provided this entry */
	bool hidden;	/* tombstone: never emitted, blocks lower branches */
	bool ino_fixed;	/* @ino settled; no lower origin can change it */
	int len;
	char name[];
};

/* one branch directory's change signal (see aufsng_dir_cache_fresh) */
struct aufsng_dir_stamp {
	struct timespec64 mtime;
	u64 iversion;
};

struct aufsng_dir_cache {
	long refcount;
	u64 version;
	/*
	 * Every branch directory's change stamp when this listing was
	 * built (slot 0 = upper, then the lower stack in order).  Under
	 * udba=reval they let an out-of-band edit of ANY branch - a
	 * hand-removed whiteout in the rw branch, a file created in a
	 * lower branch through its source mount - invalidate the cache
	 * without re-reading every branch on each open; those edits are
	 * the only way the merged view can change without bumping
	 * @version.  Lookup's revalidation honors the same signals for
	 * positive names (and negatives under an upper-backed parent;
	 * a cached negative whose parent has NO upper deliberately
	 * skips the lower rescan - see aufsng_lookup_negative_valid() -
	 * so readdir may show an out-of-band lower addition before a
	 * cached miss for it expires).
	 * i_version is the tick-independent signal where the branch fs
	 * supports it; mtime is the always-present fallback.
	 */
	unsigned int nr_stamps;
	struct aufsng_dir_stamp *stamps;
	/* non-tombstone entries; the union-emptiness answer for rmdir */
	unsigned int nr_visible;
	struct list_head entries;
	struct rb_root root;
};

struct aufsng_dir_file {
	struct aufsng_dir_cache *cache;
	/*
	 * Where the previous getdents on this fd stopped: the list node
	 * whose entry sits at offset @cursor_pos.  Only trusted while
	 * @cache is unchanged and ctx->pos still equals @cursor_pos;
	 * anything else (llseek, rewind, cache rebuild) falls back to a
	 * scan from the head.  Without it, each getdents call re-skips
	 * everything already returned and a full listing goes O(n^2).
	 */
	struct list_head *cursor;
	loff_t cursor_pos;
};

/*
 * Common head for actors fed through aufsng_dir_drain(): the drain
 * loop needs the per-call progress count and the actor's sticky error.
 * Must be the first member of the embedding struct.
 */
struct aufsng_dir_drain {
	struct dir_context ctx;
	int count;
	int err;
};

struct aufsng_readdir_data {
	struct aufsng_dir_drain dd;
	struct aufsng_dir_cache *cache;
	unsigned int idx;	/* branch slot of the layer being read */
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
			 bool hidden, unsigned int idx)
{
	struct aufsng_cache_entry *p;

	/*
	 * The first (highest priority) branch to claim a name wins,
	 * whether that claim is a real entry or a whiteout tombstone -
	 * with two refinements for later occurrences:
	 *
	 * - "foo" and ".wh.foo" inside the SAME branch directory: the
	 *   whiteout wins no matter which one getdents returned first,
	 *   matching lookup's whiteout-first probe.  (The state occurs
	 *   transiently inside a delete, after a crash mid-delete, or
	 *   via an out-of-band branch edit; hash-ordered getdents makes
	 *   the arrival order arbitrary.)
	 *
	 * - an upper entry's d_ino must match what stat() reports: the
	 *   union inode is keyed by the topmost same-type LOWER origin
	 *   when one exists (see aufsng_get_inode()), so the first
	 *   lower occurrence of an upper-provided name donates its
	 *   inode number.  A type-mismatched lower is shadowed as an
	 *   unrelated object (the upper keeps its own ino), and a
	 *   whiteout ends the origin search - both settle the number
	 *   for all deeper branches.
	 */
	p = aufsng_cache_entry_find(&cache->root, name, namelen);
	if (p) {
		if (hidden && !p->hidden && p->idx == idx) {
			p->hidden = true;
			cache->nr_visible--;
		} else if (!p->hidden && !p->ino_fixed && p->idx == 0 &&
			   idx != 0) {
			if (!hidden &&
			    (p->d_type == d_type || p->d_type == DT_UNKNOWN ||
			     d_type == DT_UNKNOWN))
				p->ino = aufsng_map_ino(ino, idx);
			p->ino_fixed = true;
		}
		return 0;
	}

	p = kmalloc(sizeof(*p) + namelen + 1, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	memcpy(p->name, name, namelen);
	p->name[namelen] = '\0';
	p->len = namelen;
	p->ino = ino;
	p->d_type = d_type;
	p->hidden = hidden;
	p->idx = idx;
	p->ino_fixed = idx != 0;
	if (!hidden)
		cache->nr_visible++;

	aufsng_cache_entry_insert(&cache->root, p);
	list_add_tail(&p->l_node, &cache->entries);
	return 0;
}

static bool aufsng_fill_merge(struct dir_context *ctx, const char *name,
			   int namelen, loff_t offset, u64 ino,
			   unsigned int d_type)
{
	struct aufsng_readdir_data *rdd =
		container_of(ctx, struct aufsng_readdir_data, dd.ctx);
	int err;

	if (name_is_dot_dotdot(name, namelen))
		return true;

	if (aufsng_is_wh_bookkeeping(name, namelen))
		return true;	/* opaque marker or bookkeeping: invisible,
				 * hides nothing */

	if (aufsng_is_wh_name(name, namelen)) {
		const char *real = name + AUFSNG_WH_PFX_LEN;
		int reallen = namelen - AUFSNG_WH_PFX_LEN;

		err = aufsng_cache_add(rdd->cache, real, reallen, 0, 0, true,
				    rdd->idx);
	} else {
		/*
		 * Fold the branch slot into the inode number, exactly as the
		 * stat/getattr path does (aufsng_map_ino), so a file's d_ino
		 * from readdir matches its st_ino and cannot collide with a
		 * same-numbered inode in another branch under the one union
		 * st_dev.  (For upper names with a lower origin the number
		 * is corrected to the origin's when the origin's branch is
		 * merged - see aufsng_cache_add().)
		 */
		err = aufsng_cache_add(rdd->cache, name, namelen,
				    aufsng_map_ino(ino, rdd->idx), d_type,
				    false, rdd->idx);
	}

	if (err) {
		rdd->dd.err = err;
		return false;
	}
	rdd->dd.count++;
	return true;
}

/*
 * Open the real directory @realpath with the creator credentials and
 * feed every entry through @dd's actor.  A single iterate_dir() call
 * is not guaranteed to reach the end of the directory, so it is called
 * again until a call adds nothing (the actor counts its accepted
 * entries in dd->count); the actor's own sticky error (dd->err) is
 * honored only when the call itself succeeded.
 */
static int aufsng_dir_drain(struct aufsng_fs *pfs, const struct path *realpath,
			 struct aufsng_dir_drain *dd)
{
	struct file *realfile;
	int err;

	realfile = dentry_open(realpath, O_RDONLY | O_DIRECTORY | O_LARGEFILE,
			       pfs->creator_cred);
	if (IS_ERR(realfile))
		return PTR_ERR(realfile);

	do {
		dd->count = 0;
		dd->err = 0;
		err = iterate_dir(realfile, &dd->ctx);
		if (!err)
			err = dd->err;
	} while (!err && dd->count);

	fput(realfile);
	return err;
}

static int aufsng_dir_read_layer(struct aufsng_fs *pfs, const struct path *realpath,
			      struct aufsng_dir_cache *cache, unsigned int idx)
{
	struct aufsng_readdir_data rdd = {
		.dd.ctx.actor = aufsng_fill_merge,
		.dd.ctx.count = INT_MAX,
		.cache = cache,
		.idx = idx,
	};

	return aufsng_dir_drain(pfs, realpath, &rdd.dd);
}

static void aufsng_cache_free(struct aufsng_dir_cache *cache)
{
	struct aufsng_cache_entry *p, *n;

	list_for_each_entry_safe(p, n, &cache->entries, l_node)
		kfree(p);
	kfree(cache->stamps);
	kfree(cache);
}

static void aufsng_stamp_sample(struct aufsng_dir_stamp *s, struct inode *dir)
{
	if (!dir) {
		s->mtime = (struct timespec64){0};
		s->iversion = 0;
		return;
	}
	s->mtime = inode_get_mtime(dir);
	s->iversion = inode_query_iversion(dir);
}

static bool aufsng_stamp_match(const struct aufsng_dir_stamp *s,
			    struct inode *dir)
{
	struct aufsng_dir_stamp cur;

	aufsng_stamp_sample(&cur, dir);
	return cur.iversion == s->iversion &&
	       timespec64_equal(&cur.mtime, &s->mtime);
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

/*
 * Merge every branch of @inode into a fresh cache.  With
 * @stop_when_visible (the rmdir/union-emptiness probe) the merge
 * stops at the first branch boundary after a visible entry appears:
 * the answer is already "not empty", and materializing a huge lower
 * directory's full listing just to throw it away would be pure waste.
 * (Stopping mid-branch would be wrong: a later ".wh.<name>" in the
 * same branch may still hide the entry just seen.)
 */
static struct aufsng_dir_cache *aufsng_cache_build(struct inode *inode,
						bool stop_when_visible)
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
	oe = AUFSNG_I_E(inode);

	/*
	 * Sample every branch's stamp before reading anything: if a
	 * concurrent edit lands during the read, the stored stamp stays
	 * older than the now-current one, so the next reuse check
	 * rebuilds rather than trust a listing that might have
	 * half-caught the change.
	 */
	cache->nr_stamps = 1 + (oe ? oe->numlower : 0);
	cache->stamps = kcalloc(cache->nr_stamps, sizeof(*cache->stamps),
				GFP_KERNEL);
	if (!cache->stamps) {
		err = -ENOMEM;
		goto out;
	}
	aufsng_stamp_sample(&cache->stamps[0], upper ? d_inode(upper) : NULL);
	for (i = 0; oe && i < oe->numlower; i++)
		aufsng_stamp_sample(&cache->stamps[1 + i],
				 d_inode(oe->lowerstack[i].dentry));

	if (upper) {
		realpath.mnt = aufsng_upper_mnt(pfs);
		realpath.dentry = upper;
		err = aufsng_dir_read_layer(pfs, &realpath, cache, 0);
	}

	for (i = 0; !err && oe && i < oe->numlower &&
		    !(stop_when_visible && cache->nr_visible); i++) {
		realpath.mnt = oe->lowerstack[i].layer->mnt;
		realpath.dentry = oe->lowerstack[i].dentry;
		err = aufsng_dir_read_layer(pfs, &realpath, cache,
					 aufsng_layer_idx(pfs,
							  oe->lowerstack[i].layer));
	}

out:
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
 * every branch directory's stamp to be unchanged - the only signal an
 * out-of-band branch edit leaves without bumping @version.  Read under
 * oi->lock so the sampled branches match the cache being validated.
 */
static bool aufsng_dir_cache_fresh(struct aufsng_fs *pfs, struct inode *inode,
				   struct aufsng_dir_cache *cache)
{
	struct dentry *upper;
	struct aufsng_entry *oe;
	unsigned int i;
	bool fresh = true;

	if (!aufsng_udba_reval(pfs))
		return true;
	/*
	 * RCU-protect the branch-stack walk: oi->lock (held by the
	 * caller) does not cover the ROOT's entry, which the root swap
	 * replaces under dyn_lock only and frees one grace period later
	 * (non-root stacks are parked on the inode until eviction).
	 */
	rcu_read_lock();
	upper = aufsng_upperdentry(inode);
	oe = AUFSNG_I_E(inode);
	if (cache->nr_stamps != 1 + (oe ? oe->numlower : 0) ||
	    !aufsng_stamp_match(&cache->stamps[0],
			     upper ? d_inode(upper) : NULL)) {
		fresh = false;
		goto out;
	}
	for (i = 0; oe && i < oe->numlower; i++) {
		if (!aufsng_stamp_match(&cache->stamps[1 + i],
				     d_inode(oe->lowerstack[i].dentry))) {
			fresh = false;
			break;
		}
	}
out:
	rcu_read_unlock();
	return fresh;
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
		cache = aufsng_cache_build(inode, false);
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
	od->cursor = NULL;
	return cache;
}

static void aufsng_dir_reset(struct file *file)
{
	struct inode *inode = file_inode(file);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_dir_file *od = file->private_data;
	bool stale;

	if (!od->cache)
		return;
	/*
	 * rewinddir() must reflect the directory's current state, so
	 * the freshness probe runs here exactly as it does on a fresh
	 * open - the version check alone would keep replaying a
	 * listing an out-of-band branch edit already invalidated.
	 */
	mutex_lock(&oi->lock);
	stale = od->cache->version != atomic64_read(&oi->version) ||
		!aufsng_dir_cache_fresh(pfs, inode, od->cache);
	if (stale)
		aufsng_cache_put(od->cache);
	mutex_unlock(&oi->lock);
	if (stale) {
		od->cache = NULL;
		od->cursor = NULL;
	}
}

static int aufsng_iterate(struct file *file, struct dir_context *ctx)
{
	struct aufsng_dir_file *od = file->private_data;
	struct aufsng_dir_cache *cache;
	struct aufsng_cache_entry *p;
	struct list_head *node;
	loff_t off;

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
	 * stay valid across getdents calls on the same cache.  The
	 * cursor resumes where the previous call stopped, so a full
	 * listing costs O(n) in total; a seek anywhere else falls back
	 * to a scan from the head.
	 */
	if (od->cursor && od->cursor_pos == ctx->pos) {
		node = od->cursor;
		off = ctx->pos;
	} else {
		node = cache->entries.next;
		off = 2;
	}
	for (; node != &cache->entries; node = node->next) {
		p = list_entry(node, struct aufsng_cache_entry, l_node);
		if (off++ < ctx->pos)
			continue;
		if (!p->hidden &&
		    !dir_emit(ctx, p->name, p->len, p->ino, p->d_type))
			break;
		ctx->pos = off;
	}
	od->cursor = node;
	od->cursor_pos = ctx->pos;
	return 0;
}

/* is the merged view of @dentry empty (whiteouts aside)? */
int aufsng_check_empty_dir(struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_dir_cache *cache = NULL;
	int err;

	/*
	 * A still-valid cached listing already knows the answer; only
	 * rebuild (aborting at the first visible entry) without one.
	 */
	mutex_lock(&oi->lock);
	if (oi->cache && oi->cache->version == atomic64_read(&oi->version) &&
	    aufsng_dir_cache_fresh(pfs, inode, oi->cache)) {
		cache = oi->cache;
		cache->refcount++;
	}
	mutex_unlock(&oi->lock);

	if (!cache) {
		cache = aufsng_cache_build(inode, true);
		if (IS_ERR(cache))
			return PTR_ERR(cache);
		err = cache->nr_visible ? -ENOTEMPTY : 0;
		aufsng_cache_free(cache);
		return err;
	}

	err = cache->nr_visible ? -ENOTEMPTY : 0;
	mutex_lock(&oi->lock);
	aufsng_cache_put(cache);
	mutex_unlock(&oi->lock);
	return err;
}

struct aufsng_wh_sweep_name {
	struct list_head node;
	int len;
	char name[];
};

struct aufsng_wh_sweep {
	struct aufsng_dir_drain dd;
	struct list_head names;
};

static bool aufsng_wh_sweep_actor(struct dir_context *ctx, const char *name,
			       int namelen, loff_t offset, u64 ino,
			       unsigned int d_type)
{
	struct aufsng_wh_sweep *sw =
		container_of(ctx, struct aufsng_wh_sweep, dd.ctx);
	struct aufsng_wh_sweep_name *p;

	if (!aufsng_is_wh_name(name, namelen))
		return true;

	p = kmalloc(sizeof(*p) + namelen + 1, GFP_KERNEL);
	if (!p) {
		sw->dd.err = -ENOMEM;
		return false;
	}
	memcpy(p->name, name, namelen);
	p->name[namelen] = '\0';
	p->len = namelen;
	list_add_tail(&p->node, &sw->names);
	sw->dd.count++;
	return true;
}

/*
 * Remove every ".wh."-prefixed bookkeeping name physically present in
 * the rw branch's own @upperdir - per-entry whiteouts, the opaque
 * marker, AND any crash-leftover ".wh..wh." temp (a parked whiteout or
 * copy-up temp file) - so that a union-empty merged dir (see
 * aufsng_check_empty_dir()) can actually be vfs_rmdir'ed: rmdir fails
 * on a directory that still physically contains any of them, even
 * though every one is invisible to the merged view.  The on-disk names
 * are swept verbatim (raw scan of @upperdir alone, not the merged
 * cache): deriving them back from the readdir merge's tombstones would
 * miss exactly the ".wh..wh." bookkeeping class, which the merge
 * deliberately never records.
 */
int aufsng_clear_whiteouts(struct aufsng_fs *pfs, struct dentry *upperdir)
{
	struct mnt_idmap *idmap = mnt_idmap(aufsng_upper_mnt(pfs));
	struct aufsng_wh_sweep sw = {
		.dd.ctx.actor = aufsng_wh_sweep_actor,
		.dd.ctx.count = INT_MAX,
	};
	struct aufsng_wh_sweep_name *p, *n;
	struct path realpath = {
		.mnt = aufsng_upper_mnt(pfs),
		.dentry = upperdir,
	};
	int err;

	INIT_LIST_HEAD(&sw.names);

	err = aufsng_dir_drain(pfs, &realpath, &sw.dd);

	if (!err) {
		/*
		 * One lock for the whole sweep.  I_MUTEX_CHILD, not the
		 * dirop helpers' I_MUTEX_PARENT: the rmdir path already
		 * holds the upper PARENT directory's lock at that
		 * subclass while this runs on its child, and taking the
		 * same subclass twice in the same i_rwsem class is a
		 * lockdep splat even though the parent->child order
		 * itself is fine (this mirrors the VFS's own
		 * parent-PARENT/victim-plain convention in vfs_rmdir()).
		 */
		inode_lock_nested(d_inode(upperdir), I_MUTEX_CHILD);
		list_for_each_entry(p, &sw.names, node) {
			struct qstr q = QSTR_LEN(p->name, p->len);
			struct dentry *whd;

			whd = lookup_one(idmap, &q, upperdir);
			if (IS_ERR(whd)) {
				err = PTR_ERR(whd);
				continue;
			}
			if (d_is_positive(whd)) {
				/*
				 * ".wh..wh.plnk"/".wh..wh.orph" style
				 * bookkeeping DIRS (real-AUFS branches,
				 * live-distro boot scripts) at a branch
				 * root can reach here via rename over a
				 * union-empty dir; sweep them too.
				 */
				if (d_is_dir(whd))
					err = vfs_rmdir(idmap,
							d_inode(upperdir),
							whd, NULL);
				else
					err = vfs_unlink(idmap,
							 d_inode(upperdir),
							 whd, NULL);
			}
			dput(whd);
		}
		inode_unlock(d_inode(upperdir));
	}

	list_for_each_entry_safe(p, n, &sw.names, node)
		kfree(p);
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
	struct inode *inode = file_inode(file);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	struct dentry *upper = aufsng_upperdentry(inode);
	struct path realpath;
	struct file *realfile;
	int err;

	/*
	 * Installers fsync a parent directory to make a create/rename
	 * durable before committing a transaction; only the rw branch
	 * can hold such dirty state (the lowers are read-only), and a
	 * directory with no upper has had no union mutations at all.
	 * The union keeps no long-lived real dir file (aufsng_dir_open
	 * only builds the merged cache), so the upper dir is opened for
	 * the sync, as original AUFS's au_do_fsync_dir_no_file does.
	 */
	if (!upper)
		return 0;

	realpath.mnt = aufsng_upper_mnt(pfs);
	realpath.dentry = upper;
	realfile = dentry_open(&realpath, O_RDONLY | O_DIRECTORY | O_LARGEFILE,
			       pfs->creator_cred);
	if (IS_ERR(realfile))
		return PTR_ERR(realfile);
	err = vfs_fsync_range(realfile, start, end, datasync);
	fput(realfile);
	return err;
}

const struct file_operations aufsng_dir_operations = {
	.read		= generic_read_dir,
	.open		= aufsng_dir_open,
	.release	= aufsng_dir_release,
	.llseek		= aufsng_dir_llseek,
	.iterate_shared	= aufsng_iterate,
	.fsync		= aufsng_dir_fsync,
};
