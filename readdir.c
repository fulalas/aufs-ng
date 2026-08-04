// SPDX-License-Identifier: GPL-2.0-only
/*
 * Merged directory reading with AUFS whiteout semantics.
 *
 * A listing is the union of branch 0 and the lower stack, highest
 * priority first: the first branch to provide a name wins.  A
 * ".wh.<name>" marker is a tombstone - never shown, and cached under
 * the real name so first-one-wins also hides it in every lower branch.
 * A ".wh..wh.*" name is never shown and hides nothing: not a whiteout,
 * just bookkeeping.
 *
 * The result is cached on the inode against its version, sampled
 * BEFORE the merge: a branch change bumps it without this dir's lock,
 * so a later sample could cover a change the merge never saw.
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/cred.h>
#include <linux/rbtree.h>
#include <linux/refcount.h>
#include <linux/iversion.h>
#include <linux/rcupdate.h>
#include <linux/fs_dirent.h>
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
	/* oi->cache holds a reference while set, so a put needs no mutex */
	refcount_t refcount;
	u64 version;
	/*
	 * Each branch directory's change stamp when the listing was built
	 * (slot 0 = upper, then the lower stack).  Under udba=reval they
	 * catch an out-of-band edit of any branch - the only way the
	 * merged view changes without bumping @version - without
	 * re-reading every branch on each open.  i_version where the
	 * branch fs has it, mtime as the fallback.
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
	 * Where the previous getdents stopped.  Trusted only while
	 * @cache is unchanged and ctx->pos still equals @cursor_pos;
	 * anything else scans from the head.  Without it a full listing
	 * is O(n^2).
	 */
	struct list_head *cursor;
	loff_t cursor_pos;
};

/*
 * Common head for aufsng_dir_drain() actors: the loop needs the
 * per-call count and the sticky error.  Must be the first member.
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
	/* deferred type checks for this layer (see struct aufsng_ino_probe) */
	struct list_head ino_probes;
};

/*
 * An inode-number donation whose type check getdents could not answer:
 * the branch reported DT_UNKNOWN, so whether this lower really is the
 * upper name's copy-up origin needs a lookup in that branch.
 *
 * Deferred until after the drain, since a lookup inside iterate_dir()
 * would re-acquire i_rwsem.  Treating DT_UNKNOWN as "matches anything"
 * would let a type-mismatched lower donate and make d_ino disagree
 * with st_ino; refusing outright would break every genuine origin on
 * such a branch.  Branches reporting real types allocate nothing.
 */
struct aufsng_ino_probe {
	struct list_head node;
	struct aufsng_cache_entry *e;
	u64 ino;		/* donated only if the type checks out */
};

static int aufsng_ino_probe_add(struct list_head *probes,
			     struct aufsng_cache_entry *e, u64 ino)
{
	struct aufsng_ino_probe *pr = kmalloc(sizeof(*pr), GFP_KERNEL);

	if (!pr)
		return -ENOMEM;
	pr->e = e;
	pr->ino = ino;
	list_add_tail(&pr->node, probes);
	return 0;
}

/*
 * Settle (and free) one branch's deferred donations: hand the number
 * over only at the same file type, the rule lookup applies.  @resolve
 * is false when the drain failed and the cache is being discarded; a
 * lookup error leaves the upper's own number, as a mismatch does.
 */
static void aufsng_ino_probes_settle(const struct path *realpath,
				  struct list_head *probes, bool resolve)
{
	struct aufsng_ino_probe *pr, *n;

	list_for_each_entry_safe(pr, n, probes, node) {
		struct qstr q = QSTR_LEN(pr->e->name, pr->e->len);
		struct dentry *this;

		if (resolve) {
			this = lookup_one_positive_unlocked(mnt_idmap(realpath->mnt),
							    &q, realpath->dentry);
			if (!IS_ERR(this)) {
				if (fs_umode_to_dtype(d_inode(this)->i_mode) ==
				    pr->e->d_type)
					pr->e->ino = pr->ino;
				dput(this);
			}
		}
		list_del(&pr->node);
		kfree(pr);
	}
}

/* ".wh..wh." double-prefix: opaque marker + AUFS bookkeeping names */
static bool aufsng_is_wh_bookkeeping(const char *name, int len)
{
	return len >= 2 * AUFSNG_WH_PFX_LEN &&
	       !memcmp(name, AUFSNG_WH_PFX AUFSNG_WH_PFX, 2 * AUFSNG_WH_PFX_LEN);
}

/* the single definition of the cache's key order */
static int aufsng_cache_entry_cmp(const char *name, int len,
			       const struct aufsng_cache_entry *p)
{
	int cmp = strncmp(name, p->name, len);

	return cmp ? cmp : len - p->len;
}

static int aufsng_cache_add(struct aufsng_dir_cache *cache, const char *name,
			 int namelen, u64 ino, unsigned int d_type,
			 bool hidden, unsigned int idx,
			 struct list_head *probes)
{
	struct rb_node **newp = &cache->root.rb_node;
	struct rb_node *parent = NULL;
	struct aufsng_cache_entry *p;

	/*
	 * The first branch to claim a name wins, entry or tombstone -
	 * with two refinements for later occurrences:
	 *
	 * - "foo" and ".wh.foo" in the SAME branch dir: the whiteout
	 *   wins whichever getdents returned first, matching lookup's
	 *   whiteout-first probe.  Hash order makes it arbitrary.
	 *
	 * - an upper entry's d_ino must match stat(): the union inode is
	 *   keyed by the topmost same-type LOWER origin, so the first
	 *   such lower donates its number.  A type mismatch or a
	 *   whiteout settles the number for all deeper branches, and an
	 *   unmarked upper file is settled before any lower is read.
	 *
	 * One rbtree descent serves the find and the insert position:
	 * every entry of every branch passes through here.
	 */
	while (*newp) {
		int cmp;

		p = rb_entry(*newp, struct aufsng_cache_entry, node);
		parent = *newp;
		cmp = aufsng_cache_entry_cmp(name, namelen, p);
		if (cmp > 0) {
			newp = &(*newp)->rb_right;
			continue;
		}
		if (cmp < 0) {
			newp = &(*newp)->rb_left;
			continue;
		}
		if (hidden && !p->hidden && p->idx == idx) {
			p->hidden = true;
			cache->nr_visible--;
		} else if (!p->hidden && !p->ino_fixed && idx != 0) {
			/*
			 * @p came from branch 0 (implied by !ino_fixed), so
			 * this lower is a candidate origin and settles it.
			 */
			p->ino_fixed = true;
			if (hidden)
				;	/* whiteout: ends the origin search */
			else if (d_type == p->d_type)
				p->ino = aufsng_map_ino(ino, idx);
			else if (d_type == DT_UNKNOWN && p->d_type != DT_UNKNOWN)
				/* only the branch itself can tell: defer */
				return aufsng_ino_probe_add(probes, p,
						aufsng_map_ino(ino, idx));
			else if (p->d_type == DT_UNKNOWN)
				/*
				 * The UPPER's type is the unknown one.  Branch
				 * 0 is a local writable fs and does report
				 * types, so accept rather than look up again.
				 */
				p->ino = aufsng_map_ino(ino, idx);
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

	rb_link_node(&p->node, parent, newp);
	rb_insert_color(&p->node, &cache->root);
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
				    rdd->idx, &rdd->ino_probes);
	} else {
		/*
		 * Fold the slot into the number as getattr does, so d_ino
		 * matches st_ino and cannot collide across branches under
		 * the one union st_dev.  Upper names with a lower origin
		 * are corrected when that branch is merged.
		 */
		err = aufsng_cache_add(rdd->cache, name, namelen,
				    aufsng_map_ino(ino, rdd->idx), d_type,
				    false, rdd->idx, &rdd->ino_probes);
	}

	if (err) {
		rdd->dd.err = err;
		return false;
	}
	rdd->dd.count++;
	return true;
}

/*
 * Open @realpath with creator credentials and feed every entry through
 * @dd's actor.  One iterate_dir() need not reach the end, so it runs
 * until a call adds nothing; the actor's sticky error counts only when
 * the call itself succeeded.
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
	int err;

	INIT_LIST_HEAD(&rdd.ino_probes);
	err = aufsng_dir_drain(pfs, realpath, &rdd.dd);

	/* the type checks getdents left open, now that no i_rwsem is held */
	aufsng_ino_probes_settle(realpath, &rdd.ino_probes, !err);
	return err;
}

/*
 * Which of the rw branch's entries may still take a lower's inode
 * number, now that it is drained and no i_rwsem is held.  The rule is
 * lookup's own: an entry it refuses keeps branch 0's number, or d_ino
 * would contradict st_ino.
 *
 * Directories go through it too - an opaque one ends its merged stack
 * and is keyed on the upper.  A failed lookup settles on the upper's
 * number.  Called only with lowers left to read.
 */
static void aufsng_cache_fix_unmarked(struct aufsng_fs *pfs,
				   const struct path *upperpath,
				   struct aufsng_dir_cache *cache)
{
	struct aufsng_cache_entry *p;

	list_for_each_entry(p, &cache->entries, l_node) {
		struct qstr q = QSTR_LEN(p->name, p->len);
		struct dentry *this;

		if (p->hidden)
			continue;

		this = lookup_one_positive_unlocked(mnt_idmap(upperpath->mnt),
						    &q, upperpath->dentry);
		if (IS_ERR(this)) {
			p->ino_fixed = true;
			continue;
		}
		if (!aufsng_upper_claims_origin(pfs, this, &q))
			p->ino_fixed = true;
		dput(this);
	}
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
	if (cache && refcount_dec_and_test(&cache->refcount))
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
 * @stop_when_visible (the emptiness probe) it stops at the first
 * branch boundary after a visible entry: the answer is already "not
 * empty".  Stopping mid-branch would be wrong - a later ".wh.<name>"
 * in the same branch may still hide that entry.
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
	refcount_set(&cache->refcount, 1);
	INIT_LIST_HEAD(&cache->entries);
	cache->root = RB_ROOT;

	/* dyn_lock excludes branch changes for the whole merge */
	percpu_down_read(&pfs->dyn_lock);
	version = atomic64_read(&AUFSNG_I(inode)->version);
	old_cred = override_creds(pfs->creator_cred);

	upper = aufsng_upperdentry(inode);
	oe = AUFSNG_I_E(inode);

	/*
	 * Sample every stamp before reading anything: an edit landing
	 * during the read leaves the stored stamp older than the current
	 * one, so the next check rebuilds instead of trusting a listing
	 * that half-caught it.  Only udba=reval reads them; a remount
	 * into it finds nr_stamps == 0 and rebuilds once.
	 */
	if (aufsng_udba_reval(pfs)) {
		cache->nr_stamps = 1 + (oe ? oe->numlower : 0);
		cache->stamps = kcalloc(cache->nr_stamps,
					sizeof(*cache->stamps), GFP_KERNEL);
		if (!cache->stamps) {
			err = -ENOMEM;
			goto out;
		}
		aufsng_stamp_sample(&cache->stamps[0],
				 upper ? d_inode(upper) : NULL);
		for (i = 0; oe && i < oe->numlower; i++)
			aufsng_stamp_sample(&cache->stamps[1 + i],
					 d_inode(oe->lowerstack[i].dentry));
	}

	if (upper) {
		realpath.mnt = aufsng_upper_mnt(pfs);
		realpath.dentry = upper;
		err = aufsng_dir_read_layer(pfs, &realpath, cache, 0);
		/*
		 * Only worth settling when a lower will actually be read;
		 * otherwise the verdict costs a lookup per entry and is
		 * never consulted.
		 */
		if (!err && oe && oe->numlower &&
		    !(stop_when_visible && cache->nr_visible))
			aufsng_cache_fix_unmarked(pfs, &realpath, cache);
	}

	for (i = 0; !err && oe && i < oe->numlower &&
		    !(stop_when_visible && cache->nr_visible); i++) {
		realpath.mnt = oe->lowerstack[i].mnt;
		realpath.dentry = oe->lowerstack[i].dentry;
		err = aufsng_dir_read_layer(pfs, &realpath, cache,
					 aufsng_layer_idx(oe->lowerstack[i].layer));
	}

out:
	revert_creds(old_cred);
	percpu_up_read(&pfs->dyn_lock);

	if (err) {
		aufsng_cache_free(cache);
		return ERR_PTR(err);
	}

	cache->version = version;
	return cache;
}

/*
 * Is @cache still consistent with the branches?  The caller checked
 * @version already; under udba=reval every branch stamp must also be
 * unchanged - the only signal an out-of-band edit leaves.  Under
 * oi->lock, so the branches match the cache being validated.
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
	 * RCU for the walk: oi->lock does not cover the ROOT's entry,
	 * which the root swap replaces under dyn_lock and frees a grace
	 * period later.
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

/*
 * Is @cache still the current listing?  Version-valid AND, under
 * udba=reval, no branch edited out of band - the ONE definition of
 * "reusable", shared by open, rewinddir and the emptiness probe.
 * Caller holds oi->lock.
 */
static bool aufsng_cache_usable(struct aufsng_fs *pfs, struct inode *inode,
			     struct aufsng_dir_cache *cache)
{
	return cache &&
	       cache->version == atomic64_read(&AUFSNG_I(inode)->version) &&
	       aufsng_dir_cache_fresh(pfs, inode, cache);
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
	/* Reuse while usable: the "nothing changed" open stays O(1) */
	if (aufsng_cache_usable(pfs, inode, cache)) {
		refcount_inc(&cache->refcount);
	} else {
		mutex_unlock(&oi->lock);
		cache = aufsng_cache_build(inode, false);
		if (IS_ERR(cache))
			return cache;
		mutex_lock(&oi->lock);
		/* attach as the inode's cache, superseding any old one */
		aufsng_dir_cache_release(oi);
		oi->cache = cache;
		refcount_inc(&cache->refcount);	/* the inode's reference */
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
	 * rewinddir() must show the current state, so the freshness probe
	 * runs as on a fresh open: the version check alone would replay a
	 * listing an out-of-band edit already invalidated.
	 */
	mutex_lock(&oi->lock);
	stale = !aufsng_cache_usable(pfs, inode, od->cache);
	mutex_unlock(&oi->lock);
	if (stale) {
		aufsng_cache_put(od->cache);
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
	 * Offsets 2.. index the merged list, immutable once built;
	 * tombstones consume an offset so cookies stay valid across
	 * calls.  The cursor resumes where the last call stopped, making
	 * a full listing O(n); any other seek scans from the head.
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

	/* A valid cache knows the answer; only rebuild without one */
	mutex_lock(&oi->lock);
	if (aufsng_cache_usable(pfs, inode, oi->cache)) {
		cache = oi->cache;
		refcount_inc(&cache->refcount);
	}
	mutex_unlock(&oi->lock);

	if (!cache) {
		cache = aufsng_cache_build(inode, true);
		if (IS_ERR(cache))
			return PTR_ERR(cache);
	}

	/*
	 * One tail for both cases: a fresh probe cache comes back at
	 * refcount 1, so the put frees it while a shared one survives.
	 */
	err = cache->nr_visible ? -ENOTEMPTY : 0;
	aufsng_cache_put(cache);
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
 * Remove every ".wh." name physically present in @upperdir - per-entry
 * whiteouts, the opaque marker and any ".wh..wh." leftover - so a
 * union-empty directory can actually be vfs_rmdir'ed: rmdir fails
 * while any remains, invisible to the merged view though they are.
 * Swept by a raw scan, not from the merge's tombstones, which
 * deliberately never record the ".wh..wh." class.
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
		 * One lock for the sweep, at I_MUTEX_CHILD: rmdir already
		 * holds the parent at I_MUTEX_PARENT, and the same
		 * subclass twice is a lockdep splat even though the
		 * parent->child order is fine.
		 */
		int sweep_err = 0;

		inode_lock_nested(d_inode(upperdir), I_MUTEX_CHILD);
		list_for_each_entry(p, &sw.names, node) {
			struct qstr q = QSTR_LEN(p->name, p->len);
			struct dentry *whd;
			int e;

			whd = lookup_one(idmap, &q, upperdir);
			if (IS_ERR(whd)) {
				if (!sweep_err)
					sweep_err = PTR_ERR(whd);
				continue;
			}
			e = 0;
			if (d_is_positive(whd)) {
				/*
				 * ".wh..wh.plnk"/".wh..wh.orph" style
				 * bookkeeping DIRS at a branch root can
				 * reach here via rename over a union-empty
				 * dir; sweep them too.
				 */
				if (d_is_dir(whd))
					e = vfs_rmdir(idmap,
						      d_inode(upperdir),
						      whd, NULL);
				else
					e = vfs_unlink(idmap,
						       d_inode(upperdir),
						       whd, NULL);
			}
			dput(whd);
			/*
			 * The FIRST failure is the result: a later success
			 * must not hide it, or the caller sees 0 with
			 * markers still on disk.  The sweep continues -
			 * each one removed is one less blocking the retry.
			 */
			if (e && !sweep_err)
				sweep_err = e;
		}
		inode_unlock(d_inode(upperdir));
		err = sweep_err;
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

	aufsng_cache_put(od->cache);
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
	 * Only the rw branch can hold dirty directory state, and a dir
	 * with no upper has seen no union mutation at all.  The union
	 * keeps no long-lived real dir file, so the upper is opened for
	 * the sync, as AUFS's au_do_fsync_dir_no_file does.
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
