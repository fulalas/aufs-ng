// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime branch add/remove for aufs-ng ("add=N:PATH=MODE" /
 * "del=PATH" on remount), matching AUFS semantics.
 *
 *  - Fixed-capacity layers[] array so struct aufsng_layer addresses stay
 *    stable for the mount's lifetime (aufsng_entry stacks reference them
 *    by pointer); freed slots (mnt == NULL) are reused.
 *
 *  - A new branch is inserted at the front of the root's lowerstack
 *    (matching AUFS's "add=1:" - always immediately below branch 0),
 *    so the most recently added branch has top priority.  Generality
 *    for add=N with N>1 is supported via an insert position, but real
 *    live-distro usage only ever specifies N=1.
 *
 *  - Every add/remove invalidates the dentry cache via the generation
 *    stamp (see dcache.c) rather than attempting a surgical,
 *    cache-preserving update - always correct, at the cost of a cold
 *    cache after each change.  A surgical-add fast path is possible
 *    but deliberately not implemented, to keep this file's size and
 *    risk down.
 *
 *  - Removal never bumps the generation on its own success path:
 *    dcache shrink + evict_inodes() + in-place rebuild of any
 *    surviving in-use directories already keeps the cache consistent,
 *    and a bump would permanently strand pinned dentries (open fds,
 *    process CWDs) that no fresh lookup can ever replace, breaking
 *    fd-based re-resolution (firejail, fusermount) with ESTALE.
 *
 *  - Directory aliasing prevention: a cached directory inode is
 *    re-keyed in the inode hash whenever its top lower changes, so a
 *    fresh lookup finds the pinned inode instead of allocating an
 *    alias.
 *
 *  - Superseded per-directory stacks are parked on the inode (with
 *    the removed branch's mount pinned) until the inode is evicted,
 *    not freed after a single synchronize_rcu(): an in-flight
 *    getattr/permission may still be dereferencing them.
 *
 *  - Removal's busy-scan uses evict_inodes() (one sb-wide pass) after
 *    shrink_dcache_sb(), not a per-inode evict-and-restart loop that
 *    is effectively quadratic in cached inode count.
 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include "aufsng.h"

static inline struct inode *aufsng_root_inode(struct super_block *sb)
{
	return d_inode(sb->s_root);
}

/*
 * Swap the root dentry's aufsng_entry.  Caller must hold the root inode
 * lock and pfs->dyn_lock for writing.  Returns the old entry, which
 * the caller must free with aufsng_free_entry() after all possible
 * readers are done (readers hold pfs->dyn_lock for reading).
 */
static struct aufsng_entry *aufsng_dyn_swap_root(struct super_block *sb,
					   struct aufsng_entry *new_oe,
					   unsigned long gen)
{
	struct inode *inode = aufsng_root_inode(sb);
	struct aufsng_entry *old_oe = AUFSNG_I_E(inode);

	WRITE_ONCE(AUFSNG_I(inode)->oe, new_oe);
	aufsng_dentry_restamp_gen(sb->s_root, gen);
	/* force the merged readdir cache of the root to be rebuilt */
	atomic64_inc(&AUFSNG_I(inode)->version);

	return old_oe;
}

/*
 * With dynamic branches, a directory may be looked up again while an
 * older union inode for it is still cached and pinned: branch changes
 * reorder priorities, so the top lower (the inode hash key) of a
 * fresh lookup can differ from the one the cached inode was created
 * with, temporarily aliasing the directory.  Heal it by adopting the
 * new upper instead of failing the lookup with ESTALE.
 */
bool aufsng_dyn_adopt_upper(struct inode *inode, struct dentry *lowerdentry,
			 struct dentry *upperdentry)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_entry *oe;
	bool ok = false;

	if (!S_ISDIR(inode->i_mode) || !upperdentry || !lowerdentry)
		return false;
	if (!d_is_dir(upperdentry) || !d_is_dir(lowerdentry))
		return false;

	mutex_lock(&oi->lock);
	oe = oi->oe;
	if (oe && oe->numlower && d_inode(oe->lowerstack[0].dentry) ==
				  d_inode(lowerdentry)) {
		if (!oi->upperdentry) {
			oi->upperdentry = dget(upperdentry);
			ok = true;
		} else {
			ok = d_inode(oi->upperdentry) == d_inode(upperdentry);
		}
	}
	mutex_unlock(&oi->lock);

	return ok;
}

/* re-key a cached directory inode in the inode hash on top-lower change */
static void aufsng_dyn_rekey_inode(struct inode *inode, struct aufsng_entry *oe)
{
	struct inode *key = oe->numlower ?
			    d_inode(oe->lowerstack[0].dentry) :
			    d_inode(aufsng_upperdentry(inode));

	if (inode->i_private == key)
		return;
	if (!inode_unhashed(inode))
		remove_inode_hash(inode);
	inode->i_private = key;
	if (key)
		__insert_inode_hash(inode, (unsigned long)key);
}

/* find an active branch whose root is @dentry, or NULL */
static struct aufsng_layer *aufsng_dyn_find_branch(struct aufsng_fs *pfs,
					     struct dentry *dentry)
{
	unsigned int i;

	for (i = 1; i < pfs->numlayer; i++) {
		struct aufsng_layer *l = &pfs->layers[i];

		if (l->mnt && l->mnt->mnt_root == dentry)
			return l;
	}
	return NULL;
}

static unsigned int aufsng_find_free_slot(struct aufsng_fs *pfs)
{
	unsigned int i;

	for (i = 1; i < pfs->numlayer; i++) {
		if (!pfs->layers[i].mnt)
			return i;
	}
	return pfs->numlayer;
}

/* defined later; used by the surgical-add path below */
static bool aufsng_entry_has_layer(struct aufsng_entry *oe,
				const struct aufsng_layer *layer);
static void aufsng_dyn_commit_rebuild(struct aufsng_fs *pfs, struct inode *inode,
				   struct aufsng_entry *new_oe,
				   const struct aufsng_layer *layer,
				   struct aufsng_dyn_parked *parked);

#define AUFSNG_RESOLVE_MAXDEPTH 64

/*
 * Resolve the directory in the just-added branch @mnt that corresponds
 * to a cached union directory @inode, by replaying @inode's path (its
 * dentry name chain up to the root) from the branch root downward.
 * Returns a positive directory dentry (ref held) or NULL if the branch
 * does not provide this path as a visible (non-whited-out) directory.
 */
static struct dentry *aufsng_dyn_resolve_lower(struct super_block *sb,
					    struct inode *inode,
					    struct vfsmount *mnt)
{
	struct dentry *stack[AUFSNG_RESOLVE_MAXDEPTH];
	struct dentry *da, *cur, *par, *base;
	unsigned int n = 0, i;

	da = d_find_alias(inode);
	if (!da)
		return NULL;
	cur = dget(da);
	dput(da);
	while (cur != sb->s_root && !IS_ROOT(cur)) {
		if (n >= AUFSNG_RESOLVE_MAXDEPTH) {
			dput(cur);
			for (i = 0; i < n; i++)
				dput(stack[i]);
			return NULL;	/* too deep: skip (rare) */
		}
		stack[n++] = dget(cur);
		par = dget_parent(cur);
		dput(cur);
		cur = par;
	}
	dput(cur);

	base = dget(mnt->mnt_root);
	for (i = n; i-- > 0; ) {
		struct qstr q = QSTR_LEN(stack[i]->d_name.name,
					 stack[i]->d_name.len);
		struct dentry *child;
		int wh;

		wh = aufsng_check_whiteout(mnt, base, &stack[i]->d_name);
		if (wh) {		/* whited out (or error): path blocked */
			dput(base);
			base = NULL;
			break;
		}
		child = lookup_one_unlocked(mnt_idmap(mnt), &q, base);
		dput(base);
		if (IS_ERR(child)) {
			base = NULL;
			break;
		}
		if (d_is_negative(child) || !d_is_dir(child)) {
			dput(child);
			base = NULL;
			break;
		}
		base = child;
	}
	for (i = 0; i < n; i++)
		dput(stack[i]);
	return base;
}

/*
 * Build the new stack for cached union directory @inode after a branch
 * add: the new branch's directory @nd becomes the top lower (add=1
 * semantics).  Returns the new entry, NULL to skip (nothing to merge),
 * or ERR_PTR on failure.  Caller holds dyn_lock for writing.
 */
static struct aufsng_entry *aufsng_dyn_prep_splice(struct aufsng_fs *pfs,
					     struct inode *inode,
					     struct dentry *nd,
					     struct aufsng_layer *layer)
{
	struct aufsng_entry *cur = AUFSNG_I_E(inode);
	struct dentry *upper = aufsng_upperdentry(inode);
	struct aufsng_entry *neu;
	unsigned int base_n, i;
	int opq;

	if (upper) {
		opq = aufsng_check_diropq(aufsng_upper_mnt(pfs), upper);
		if (opq < 0)
			return ERR_PTR(opq);
		if (opq)
			return NULL;	/* opaque upper hides all lowers */
	}
	if (aufsng_entry_has_layer(cur, layer))
		return NULL;		/* already spliced (legacy replay) */

	opq = aufsng_check_diropq(layer->mnt, nd);
	if (opq < 0)
		return ERR_PTR(opq);

	/* an opaque new dir hides the existing lowers underneath it */
	base_n = (opq || !cur) ? 0 : cur->numlower;
	neu = aufsng_alloc_entry(1 + base_n);
	if (!neu)
		return ERR_PTR(-ENOMEM);
	neu->lowerstack[0].layer = layer;
	neu->lowerstack[0].dentry = dget(nd);
	for (i = 0; i < base_n; i++) {
		neu->lowerstack[i + 1].dentry = dget(cur->lowerstack[i].dentry);
		neu->lowerstack[i + 1].layer = cur->lowerstack[i].layer;
	}
	return neu;
}

/*
 * Drop cached negative children of @inode so names the new branch now
 * provides are re-resolved instead of returning a stale "not found".
 * Negative dentries are never mountpoints, so this touches no mounts.
 */
static void aufsng_dyn_drop_neg_children(struct inode *inode)
{
	struct dentry *parent = d_find_alias(inode);
	struct dentry *child;
	struct hlist_node *tmp;

	if (!parent)
		return;
	spin_lock(&parent->d_lock);
	hlist_for_each_entry_safe(child, tmp, &parent->d_children, d_sib) {
		spin_lock_nested(&child->d_lock, DENTRY_D_LOCK_NESTED);
		if (d_is_negative(child) && !d_unhashed(child))
			__d_drop(child);
		spin_unlock(&child->d_lock);
	}
	spin_unlock(&parent->d_lock);
	dput(parent);
}

/*
 * After the root stack gained @layer, splice that branch into every
 * cached union directory in place, so pinned directories (open fds,
 * cwds) immediately show the new branch's content - without a
 * cache-invalidation that would detach nested mounts.  Caller holds the
 * root inode lock and dyn_lock for writing.
 */
static void aufsng_dyn_splice_cached(struct aufsng_fs *pfs, struct super_block *sb,
				  struct aufsng_layer *layer,
				  struct inode *root_inode)
{
	struct inode **dirs = NULL;
	unsigned int ndirs = 0, cap = 0, i;
	struct inode *inode;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		if (inode == root_inode)
			continue;
		spin_lock(&inode->i_lock);
		if ((inode_state_read(inode) &
		     (I_FREEING | I_WILL_FREE | I_NEW)) ||
		    !S_ISDIR(inode->i_mode)) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		if (ndirs == cap) {
			unsigned int ncap = max(16U, cap * 2);
			struct inode **d;

			d = krealloc_array(dirs, ncap, sizeof(*d), GFP_ATOMIC);
			if (!d) {
				spin_unlock(&inode->i_lock);
				break;	/* best effort: skip the remainder */
			}
			dirs = d;
			cap = ncap;
		}
		atomic_inc(&inode->i_count);
		dirs[ndirs++] = inode;
		spin_unlock(&inode->i_lock);
	}
	spin_unlock(&sb->s_inode_list_lock);

	for (i = 0; i < ndirs; i++) {
		struct dentry *nd = aufsng_dyn_resolve_lower(sb, dirs[i],
							  layer->mnt);
		struct aufsng_entry *neu;
		struct aufsng_dyn_parked *pk;

		if (!nd)
			continue;
		neu = aufsng_dyn_prep_splice(pfs, dirs[i], nd, layer);
		dput(nd);
		if (IS_ERR_OR_NULL(neu))
			continue;
		pk = kmalloc(sizeof(*pk), GFP_KERNEL);
		if (!pk) {
			aufsng_free_entry(neu);
			continue;
		}
		aufsng_dyn_commit_rebuild(pfs, dirs[i], neu, layer, pk);
		aufsng_dyn_drop_neg_children(dirs[i]);
	}

	for (i = 0; i < ndirs; i++)
		iput(dirs[i]);
	kfree(dirs);
}

int aufsng_dyn_add_branch(struct super_block *sb, const char *name,
		       const struct path *path, unsigned int pos,
		       enum aufsng_br_perm perm)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);
	struct inode *root_inode = aufsng_root_inode(sb);
	struct aufsng_entry *old_oe, *new_oe, *cur_oe;
	struct aufsng_path *nstack, *ostack;
	struct aufsng_layer *layer;
	struct vfsmount *mnt;
	char *dup_name;
	unsigned int n, idx;
	unsigned long gen;
	int err;

	idx = aufsng_find_free_slot(pfs);
	if (idx == pfs->numlayer && pfs->numlayer >= pfs->numlayer_cap)
		return -ENOSPC;

	err = aufsng_check_layer(sb, path, name);
	if (err)
		return err;
	if (aufsng_dyn_find_branch(pfs, path->dentry))
		return -EEXIST;	/* legacy remount replay tolerates this */

	dup_name = kstrdup(name, GFP_KERNEL);
	if (!dup_name)
		return -ENOMEM;

	mnt = clone_private_mount(path);
	if (IS_ERR(mnt)) {
		err = PTR_ERR(mnt);
		goto out_name;
	}
	if (perm != AUFSNG_BR_RW)
		mnt->mnt_flags |= MNT_READONLY | MNT_NOATIME;

	cur_oe = AUFSNG_I_E(root_inode);
	n = cur_oe->numlower;
	if (pos > n)
		pos = n;	/* AUFS clamps an out-of-range index too */

	new_oe = aufsng_alloc_entry(n + 1);
	if (!new_oe) {
		err = -ENOMEM;
		goto out_mnt;
	}

	layer = &pfs->layers[idx];
	nstack = new_oe->lowerstack;
	ostack = cur_oe->lowerstack;
	for (n = 0; n < pos; n++) {
		nstack[n].dentry = dget(ostack[n].dentry);
		nstack[n].layer = ostack[n].layer;
	}
	nstack[pos].dentry = dget(path->dentry);
	nstack[pos].layer = layer;
	for (; n < cur_oe->numlower; n++) {
		nstack[n + 1].dentry = dget(ostack[n].dentry);
		nstack[n + 1].layer = ostack[n].layer;
	}

	inode_lock(root_inode);
	down_write(&pfs->dyn_lock);

	layer->mnt = mnt;
	layer->idx = idx;
	pfs->config.br_names[idx] = dup_name;
	if (idx == pfs->numlayer)
		pfs->numlayer++;

	/*
	 * No generation bump: the root stack swap plus the in-place splice
	 * into every cached directory refresh the whole tree without
	 * invalidating any dentry, so nested mounts under the union are
	 * never detached (a d_invalidate-driven refresh would detach them).
	 */
	gen = (unsigned long)atomic_long_read(&pfs->dyn_gen);
	old_oe = aufsng_dyn_swap_root(sb, new_oe, gen);
	aufsng_dyn_drop_neg_children(root_inode);
	aufsng_dyn_splice_cached(pfs, sb, layer, root_inode);

	up_write(&pfs->dyn_lock);
	inode_unlock(root_inode);

	synchronize_rcu();
	aufsng_free_entry(old_oe);

	pr_info("aufs (aufs-ng): branch '%s' added\n", name);
	return 0;

out_mnt:
	mntput(mnt);
out_name:
	kfree(dup_name);
	return err;
}

struct aufsng_dyn_scan {
	struct inode **dirs;
	unsigned int nr;
	unsigned int cap;
	unsigned int nr_busy;
};

static bool aufsng_entry_has_layer(struct aufsng_entry *oe,
				const struct aufsng_layer *layer)
{
	unsigned int i;

	for (i = 0; oe && i < oe->numlower; i++) {
		if (oe->lowerstack[i].layer == layer)
			return true;
	}
	return false;
}

/*
 * Drop every cache-only reference to @layer and classify what
 * remains: in-use non-directories make the branch busy; in-use
 * directories (ancestors of running binaries, working directories)
 * are collected so their stacks can be rebuilt without the branch.
 * Caller holds the root inode lock and pfs->dyn_lock for writing, so
 * no new reference can appear while we look.
 */
static int aufsng_dyn_scan_branch(struct super_block *sb,
			       const struct aufsng_layer *layer,
			       struct aufsng_dyn_scan *scan,
			       bool force_shrink, bool *dcache_fresh)
{
	struct inode *inode;

	if (force_shrink || !*dcache_fresh) {
		shrink_dcache_sb(sb);
		evict_inodes(sb);
		*dcache_fresh = true;
	}

again:
	scan->nr_busy = 0;
	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		if (inode == aufsng_root_inode(sb))
			continue;
		spin_lock(&inode->i_lock);
		if ((inode_state_read(inode) & (I_FREEING | I_WILL_FREE | I_NEW)) ||
		    !aufsng_entry_has_layer(AUFSNG_I_E(inode), layer)) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		if (atomic_read(&inode->i_count)) {
			if (!S_ISDIR(inode->i_mode)) {
				scan->nr_busy++;
				spin_unlock(&inode->i_lock);
				continue;
			}
			if (scan->nr == scan->cap) {
				unsigned int cap = max(16U, scan->cap * 2);
				struct inode **d;

				d = krealloc_array(scan->dirs, cap,
						   sizeof(*d), GFP_ATOMIC);
				if (!d) {
					spin_unlock(&inode->i_lock);
					spin_unlock(&sb->s_inode_list_lock);
					return -ENOMEM;
				}
				scan->dirs = d;
				scan->cap = cap;
			}
			atomic_inc(&inode->i_count);
			scan->dirs[scan->nr++] = inode;
			spin_unlock(&inode->i_lock);
			continue;
		}
		inode_state_set(inode, I_DONTCACHE);
		atomic_inc(&inode->i_count);
		spin_unlock(&inode->i_lock);
		spin_unlock(&sb->s_inode_list_lock);

		/*
		 * I_DONTCACHE makes this iput evict the inode right
		 * away; evicting before the next pass matters, or it
		 * would be double-counted as busy.  Restart: progress
		 * is guaranteed, the evicted inode cannot be found again.
		 */
		iput(inode);
		goto again;
	}
	spin_unlock(&sb->s_inode_list_lock);

	return 0;
}

static int aufsng_dyn_copy_up_dirs(struct aufsng_dyn_scan *scan)
{
	unsigned int i;
	int err = 0;

	for (i = 0; !err && i < scan->nr; i++) {
		struct inode *inode = scan->dirs[i];
		struct dentry *alias;

		if (aufsng_upperdentry(inode))
			continue;
		alias = d_find_alias(inode);
		if (!alias)
			continue;
		err = aufsng_copy_up(alias);
		dput(alias);
	}
	return err;
}

static struct aufsng_entry *aufsng_dyn_prep_rebuild(struct inode *inode,
					      const struct aufsng_layer *layer)
{
	struct aufsng_entry *old_oe = AUFSNG_I_E(inode), *new_oe;
	struct aufsng_path *nstack, *ostack;
	unsigned int i, j, n, hits = 0;

	n = old_oe->numlower;
	ostack = old_oe->lowerstack;
	for (i = 0; i < n; i++) {
		if (ostack[i].layer == layer)
			hits++;
	}
	if (!hits)
		return NULL;

	new_oe = aufsng_alloc_entry(n - hits);
	if (!new_oe)
		return ERR_PTR(-ENOMEM);

	nstack = new_oe->lowerstack;
	for (i = 0, j = 0; i < n; i++) {
		if (ostack[i].layer == layer)
			continue;
		nstack[j].dentry = dget(ostack[i].dentry);
		nstack[j].layer = ostack[i].layer;
		j++;
	}
	return new_oe;
}

static void aufsng_dyn_commit_rebuild(struct aufsng_fs *pfs, struct inode *inode,
				  struct aufsng_entry *new_oe,
				  const struct aufsng_layer *layer,
				  struct aufsng_dyn_parked *parked)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_entry *old_oe;
	unsigned long gen;

	/*
	 * No inode_lock() here: pfs->dyn_lock (write, held by caller)
	 * alone excludes every other oe reader/writer in this design
	 * (aufsng_lookup(), readdir's cache build, etc. all serialize on
	 * dyn_lock, not this directory's inode lock).  Taking
	 * inode_lock() too would deadlock: a concurrent lookup holds
	 * the parent's i_rwsem (shared) before calling ->lookup(),
	 * which blocks on down_read(&pfs->dyn_lock) - ABBA with
	 * inode_lock(inode)+dyn_lock(write) here.
	 */
	old_oe = oi->oe;
	WRITE_ONCE(oi->oe, new_oe);
	atomic64_inc(&oi->version);
	aufsng_dyn_rekey_inode(inode, new_oe);

	parked->oe = old_oe;
	parked->mnt = mntget(layer->mnt);
	parked->next = oi->dyn_parked;
	oi->dyn_parked = parked;

	gen = (unsigned long)atomic_long_read(&pfs->dyn_gen);
	{
		struct dentry *alias;

		spin_lock(&inode->i_lock);
		hlist_for_each_entry(alias, &inode->i_dentry, d_alias)
			aufsng_dentry_restamp_gen(alias, gen);
		spin_unlock(&inode->i_lock);
	}
}

void aufsng_dyn_put_parked(struct aufsng_inode *oi)
{
	struct aufsng_dyn_parked *p;

	for (p = oi->dyn_parked; p; p = p->next) {
		unsigned int i;

		for (i = 0; i < p->oe->numlower; i++)
			dput(p->oe->lowerstack[i].dentry);
		mntput(p->mnt);
	}
}

void aufsng_dyn_free_parked(struct aufsng_inode *oi)
{
	struct aufsng_dyn_parked *p, *next;

	for (p = oi->dyn_parked; p; p = next) {
		next = p->next;
		kfree(p->oe);
		kfree(p);
	}
	oi->dyn_parked = NULL;
}

static void aufsng_dyn_release_branch(struct aufsng_fs *pfs, struct aufsng_layer *layer)
{
	kfree(pfs->config.br_names[layer->idx]);
	pfs->config.br_names[layer->idx] = NULL;
	mntput(layer->mnt);
	layer->mnt = NULL;
}

int aufsng_dyn_del_branch(struct super_block *sb, const struct path *path,
		       bool *dcache_fresh)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);
	struct inode *root_inode = aufsng_root_inode(sb);
	struct aufsng_entry *new_oe, *cur_oe, *old_root_oe;
	struct aufsng_entry **new_oes = NULL;
	struct aufsng_dyn_parked **parked = NULL;
	struct aufsng_path *nstack, *ostack;
	struct aufsng_layer *layer;
	struct aufsng_dyn_scan scan = {};
	unsigned int i, n, j, tries;
	unsigned long gen;
	int err;

	layer = aufsng_dyn_find_branch(pfs, path->dentry);
	if (!layer)
		return -ENOENT;

	cur_oe = AUFSNG_I_E(root_inode);
	n = cur_oe->numlower;
	if (n < 1)
		return -EINVAL;	/* never remove the last lower branch */

	new_oe = aufsng_alloc_entry(n - 1);
	if (!new_oe)
		return -ENOMEM;

	for (tries = 0; ; tries++) {
		inode_lock(root_inode);
		down_write(&pfs->dyn_lock);

		err = aufsng_dyn_scan_branch(sb, layer, &scan, tries > 0,
					  dcache_fresh);
		if (err)
			goto out_unlock;

		if (scan.nr_busy) {
			pr_info("aufs (aufs-ng): cannot remove branch '%s': %u file(s) in use\n",
				pfs->config.br_names[layer->idx],
				scan.nr_busy);
			err = -EBUSY;
			goto out_unlock;
		}

		for (i = 0; i < scan.nr; i++) {
			if (!aufsng_upperdentry(scan.dirs[i]))
				break;
		}
		if (i == scan.nr)
			break;

		up_write(&pfs->dyn_lock);
		inode_unlock(root_inode);

		err = -EBUSY;
		if (tries >= 4)
			goto out_scan;
		err = aufsng_dyn_copy_up_dirs(&scan);
		if (err)
			goto out_scan;
		for (i = 0; i < scan.nr; i++)
			iput(scan.dirs[i]);
		scan.nr = 0;
	}

	err = -ENOMEM;
	new_oes = kcalloc(scan.nr, sizeof(*new_oes), GFP_KERNEL);
	if (!new_oes)
		goto out_unlock;
	parked = kcalloc(scan.nr, sizeof(*parked), GFP_KERNEL);
	if (!parked)
		goto out_unlock;
	for (i = 0; i < scan.nr; i++) {
		new_oes[i] = aufsng_dyn_prep_rebuild(scan.dirs[i], layer);
		if (IS_ERR(new_oes[i])) {
			new_oes[i] = NULL;
			goto out_unlock;
		}
		if (!new_oes[i])
			continue;
		parked[i] = kmalloc(sizeof(*parked[i]), GFP_KERNEL);
		if (!parked[i])
			goto out_unlock;
	}

	err = 0;
	nstack = new_oe->lowerstack;
	ostack = cur_oe->lowerstack;
	for (i = 0, j = 0; i < n; i++) {
		if (ostack[i].layer == layer)
			continue;
		if (WARN_ON(j >= n - 1)) {
			err = -EIO;
			goto out_unlock;
		}
		nstack[j].dentry = dget(ostack[i].dentry);
		nstack[j].layer = ostack[i].layer;
		j++;
	}

	/*
	 * Removal never bumps the generation on this, the success
	 * path: shrink+evict+in-place rebuild (below, still under
	 * dyn_lock write) already keeps every surviving cached dentry
	 * consistent, and a bump would strand pinned dentries (open
	 * fds, CWDs) that no fresh lookup can replace, breaking
	 * fd-based re-resolution with ESTALE.  Restamp the root to its
	 * current generation instead of advancing it.
	 */
	gen = (unsigned long)atomic_long_read(&pfs->dyn_gen);
	old_root_oe = aufsng_dyn_swap_root(sb, new_oe, gen);

	/*
	 * Swap in the prepared stacks of the collected in-use
	 * directories before releasing dyn_lock: a lookup through one
	 * of these directories takes dyn_lock for reading, so as long
	 * as we still hold it for writing here, no lookup can resolve
	 * a child through a stack that still references the removed
	 * branch.
	 */
	for (i = 0; i < scan.nr; i++) {
		if (new_oes[i])
			aufsng_dyn_commit_rebuild(pfs, scan.dirs[i], new_oes[i],
					      layer, parked[i]);
		iput(scan.dirs[i]);
	}
	kfree(scan.dirs);
	kfree(new_oes);
	kfree(parked);

	pr_info("aufs (aufs-ng): branch '%s' removed\n",
		pfs->config.br_names[layer->idx]);
	aufsng_dyn_release_branch(pfs, layer);

	up_write(&pfs->dyn_lock);
	inode_unlock(root_inode);

	synchronize_rcu();
	aufsng_free_entry(old_root_oe);

	return 0;

out_unlock:
	up_write(&pfs->dyn_lock);
	inode_unlock(root_inode);
out_scan:
	for (i = 0; i < scan.nr; i++)
		iput(scan.dirs[i]);
	kfree(scan.dirs);
	if (new_oes) {
		for (i = 0; i < scan.nr; i++) {
			if (new_oes[i])
				aufsng_free_entry(new_oes[i]);
		}
		kfree(new_oes);
	}
	if (parked) {
		for (i = 0; i < scan.nr; i++)
			kfree(parked[i]);
		kfree(parked);
	}
	aufsng_free_entry(new_oe);
	return err;
}

/*
 * Called from aufsng_reconfigure() (params.c) with sb->s_umount held for
 * writing.  Applies the "add=N:PATH=MODE" additions and "del=PATH"
 * removals collected in @ctx.
 */
int aufsng_dyn_reconfigure(struct fs_context *fc)
{
	struct super_block *sb = fc->root->d_sb;
	struct aufsng_fs_context *ctx = fc->fs_private;
	bool dcache_fresh = false;
	size_t i;
	int err = 0;

	for (i = 0; !err && i < ctx->nr_dyn_add; i++) {
		struct aufsng_ctx_branch *b = &ctx->dyn_add[i];
		unsigned int pos = b->pos ? b->pos - 1 : 0;

		err = aufsng_dyn_add_branch(sb, b->name, &b->path, pos, b->perm);
		/*
		 * Tolerate re-adding a present branch: legacy remounts
		 * may replay the current mount options.
		 */
		if (err == -EEXIST)
			err = 0;
	}

	/*
	 * dcache_fresh is shared across the whole batch of removals
	 * below: removing one branch doesn't change another branch's
	 * reference counts, so only the first removal's first attempt
	 * needs to shrink/evict the cache.
	 */
	for (i = 0; !err && i < ctx->nr_dyn_del; i++)
		err = aufsng_dyn_del_branch(sb, &ctx->dyn_del[i], &dcache_fresh);

	return err;
}
