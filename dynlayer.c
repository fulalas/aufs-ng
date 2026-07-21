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
 *  - Both add and remove update every cached directory surgically, in
 *    place, under dyn_lock held for writing: no dentry is ever
 *    invalidated by a branch change, so pinned dentries (open fds,
 *    process CWDs) and nested mounts under the union survive intact -
 *    a d_invalidate-driven refresh would detach the mounts and strand
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
#include <linux/hashtable.h>
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
					   struct aufsng_entry *new_oe)
{
	struct inode *inode = aufsng_root_inode(sb);
	struct aufsng_entry *old_oe = AUFSNG_I_E(inode);

	/*
	 * Release-publish: lockless readers (aufsng_path_real() via
	 * getattr, get_link, copy-up) load the pointer with READ_ONCE
	 * and no dyn_lock, so the entry's freshly written contents must
	 * be ordered before the pointer store.
	 */
	smp_store_release(&AUFSNG_I(inode)->oe, new_oe);
	/* force the merged readdir cache of the root to be rebuilt */
	atomic64_inc(&AUFSNG_I(inode)->version);

	return old_oe;
}

static void aufsng_dyn_drop_neg_children(struct inode *inode);

/*
 * With dynamic branches, an object may be looked up again while an
 * older union inode for it is still cached and pinned: branch changes
 * reorder priorities, so the top lower (the inode hash key) of a
 * fresh lookup can differ from the one the cached inode was created
 * with, temporarily aliasing the object.  Heal it by adopting the new
 * upper instead of failing the lookup with ESTALE.  An object's rw
 * copy can also be replaced (rmdir+mkdir, or write-tmp-and-rename
 * saves, at the same path) so its upper resolves to a different
 * inode; adopt the fresh one, park the superseded upper for a
 * lockless aufsng_path_real() reader (dropped at eviction), bump the
 * version so a directory's merged listing is rebuilt, and drop cached
 * negative children so a name the new upper provides re-resolves.
 * The one adopt-or-park state machine below serves the branch-change
 * path, lookup's ESTALE heal (aufsng_get_inode()) and udba=reval's
 * positive-dentry revalidation (dcache.c) alike.
 */
bool aufsng_dyn_adopt_upper(struct inode *inode, struct dentry *lowerdentry,
			 struct dentry *upperdentry)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);
	bool is_dir = S_ISDIR(inode->i_mode);
	struct aufsng_entry *oe;
	bool replaced = false;
	bool ok = false;

	if (!upperdentry || d_is_negative(upperdentry))
		return false;
	/*
	 * A replacement of a different file type is never adopted: the
	 * cached inode's ops were fixed at its type, so that falls
	 * through to ESTALE at the caller.
	 */
	if ((d_inode(upperdentry)->i_mode ^ inode->i_mode) & S_IFMT)
		return false;
	if (is_dir && (!lowerdentry || !d_is_dir(lowerdentry)))
		return false;

	mutex_lock(&oi->lock);
	oe = oi->oe;
	/*
	 * For directories the top-lower identity must match, keeping
	 * this from grafting an unrelated directory onto the inode.
	 * Non-directories are already identified by the inode hash key
	 * (their lower origin), so no extra match is needed.
	 */
	if (is_dir && !(oe && oe->numlower &&
			d_inode(oe->lowerstack[0].dentry) ==
			d_inode(lowerdentry)))
		goto out;

	if (!oi->upperdentry) {
		WRITE_ONCE(oi->upperdentry, dget(upperdentry));
		ok = true;
	} else if (d_inode(oi->upperdentry) == d_inode(upperdentry)) {
		ok = true;
	} else {
		struct aufsng_dyn_parked *pk =
			kmalloc(struct_size(pk, mnts, 0), GFP_KERNEL);

		if (pk) {
			pk->oe = NULL;
			pk->nr_mnt = 0;
			pk->upper = oi->upperdentry;
			WRITE_ONCE(oi->upperdentry, dget(upperdentry));
			aufsng_copyattr(inode);
			atomic64_inc(&oi->version);
			pk->next = oi->dyn_parked;
			oi->dyn_parked = pk;
			ok = true;
			replaced = true;
		}
	}
out:
	mutex_unlock(&oi->lock);

	if (replaced && is_dir)
		aufsng_dyn_drop_neg_children(inode);

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
				   struct aufsng_dyn_parked *parked);

#define AUFSNG_RESOLVE_MAXDEPTH 64
#define AUFSNG_MEMO_BITS 9

/*
 * Per-splice-pass memo of "union directory dentry -> its counterpart
 * in the branch being added (NULL = path blocked/absent)".  Cached
 * directories share their ancestor chains, so memoizing each ancestor
 * verdict makes the whole pass O(dirs) branch lookups instead of
 * O(dirs * depth) - the pass runs under dyn_lock held for WRITING,
 * with every union lookup and readdir blocked behind it, so each
 * saved lookup directly shortens a whole-filesystem stall.
 */
struct aufsng_memo_ent {
	struct hlist_node node;
	struct dentry *uniond;	/* key; ref held for the memo's lifetime */
	struct dentry *branchd;	/* counterpart, ref held; NULL = blocked */
};

struct aufsng_splice_memo {
	DECLARE_HASHTABLE(tbl, AUFSNG_MEMO_BITS);
};

static struct aufsng_memo_ent *aufsng_memo_find(struct aufsng_splice_memo *memo,
					   struct dentry *d)
{
	struct aufsng_memo_ent *e;

	hash_for_each_possible(memo->tbl, e, node, (unsigned long)d) {
		if (e->uniond == d)
			return e;
	}
	return NULL;
}

/* best effort: an allocation failure just skips the memoization */
static void aufsng_memo_store(struct aufsng_splice_memo *memo,
			   struct dentry *d, struct dentry *branchd)
{
	struct aufsng_memo_ent *e = kmalloc(sizeof(*e), GFP_KERNEL);

	if (!e)
		return;
	e->uniond = dget(d);
	e->branchd = branchd ? dget(branchd) : NULL;
	hash_add(memo->tbl, &e->node, (unsigned long)d);
}

static void aufsng_memo_free(struct aufsng_splice_memo *memo)
{
	struct aufsng_memo_ent *e;
	struct hlist_node *tmp;
	unsigned int i;

	hash_for_each_safe(memo->tbl, i, tmp, e, node) {
		hash_del(&e->node);
		dput(e->uniond);
		dput(e->branchd);
		kfree(e);
	}
}

/*
 * Advance one path component: given the union parent's inode @punion
 * and the already-resolved branch parent @pbase, resolve the union
 * child dentry @d in the new branch, enforcing the same per-level
 * visibility rules a fresh lookup would.  Returns the branch child
 * (ref held) or NULL when the path is blocked: also on transient
 * errors - skipping is safe, the directory just keeps its pre-add
 * view until evicted, same as one deeper than the replay limit.
 */
static struct dentry *aufsng_dyn_resolve_step(struct aufsng_fs *pfs,
					   struct vfsmount *mnt,
					   struct inode *punion,
					   struct dentry *pbase,
					   struct dentry *d)
{
	struct dentry *pupper = aufsng_upperdentry(punion);
	struct dentry *child = NULL;
	struct name_snapshot ns;

	/*
	 * Upper-branch masking, exactly as aufsng_lookup() enforces it
	 * level by level: an opaque upper ancestor hides every lower
	 * branch at and below it - including the one being added (it
	 * splices in as the TOP lower, so only the upper can mask it) -
	 * and an upper whiteout of this component kills the path the
	 * same way.  Without these checks a pinned directory would show
	 * module content that a fresh lookup of the same path would
	 * never merge, which then silently vanishes on dcache eviction.
	 */
	if (pupper &&
	    aufsng_check_diropq(aufsng_upper_mnt(pfs), pupper))
		return NULL;

	/*
	 * The name is read via a snapshot: union renames are excluded
	 * by dyn_lock (aufsng_rename() holds it for reading across
	 * vfs_rename()), so this is defense in depth against reading a
	 * d_name that a future unlocked d_move could swap and free
	 * mid-walk.
	 */
	take_dentry_name_snapshot(&ns, d);
	if (pupper &&
	    aufsng_check_whiteout(aufsng_upper_mnt(pfs), pupper, &ns.name))
		goto out;
	/* the new branch's own whiteout blocks the path too */
	if (aufsng_check_whiteout(mnt, pbase, &ns.name))
		goto out;

	child = lookup_one_unlocked(mnt_idmap(mnt), &ns.name, pbase);
	if (IS_ERR(child)) {
		child = NULL;
	} else if (d_is_negative(child) || !d_is_dir(child)) {
		dput(child);
		child = NULL;
	}
out:
	release_dentry_name_snapshot(&ns);
	return child;
}

/*
 * Resolve the directory in the just-added branch @mnt that corresponds
 * to a cached union directory @inode, by replaying @inode's ancestor
 * chain from the branch root downward through the memo.  Returns a
 * positive directory dentry (ref held) or NULL if the branch does not
 * provide this path as a visible directory.
 */
static struct dentry *aufsng_dyn_resolve_lower(struct aufsng_fs *pfs,
					    struct super_block *sb,
					    struct inode *inode,
					    struct vfsmount *mnt,
					    struct aufsng_splice_memo *memo)
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
		struct dentry *pu = (i == n - 1) ? sb->s_root : stack[i + 1];
		struct aufsng_memo_ent *hit = aufsng_memo_find(memo, stack[i]);
		struct dentry *child;

		if (hit) {
			child = hit->branchd ? dget(hit->branchd) : NULL;
		} else {
			child = aufsng_dyn_resolve_step(pfs, mnt, d_inode(pu),
						     base, stack[i]);
			aufsng_memo_store(memo, stack[i], child);
		}
		dput(base);
		base = child;
		if (!base)
			break;
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

	struct aufsng_splice_memo *memo;

	memo = kzalloc(sizeof(*memo), GFP_KERNEL);
	if (!memo)
		goto out_iput;	/* best effort, same as a failed dirs[] grow */
	hash_init(memo->tbl);

	for (i = 0; i < ndirs; i++) {
		struct dentry *nd = aufsng_dyn_resolve_lower(pfs, sb, dirs[i],
							  layer->mnt, memo);
		struct aufsng_entry *cur, *neu;
		struct aufsng_dyn_parked *pk;

		if (!nd)
			continue;
		neu = aufsng_dyn_prep_splice(pfs, dirs[i], nd, layer);
		dput(nd);
		if (IS_ERR_OR_NULL(neu))
			continue;
		cur = AUFSNG_I_E(dirs[i]);
		pk = kmalloc(struct_size(pk, mnts, cur ? cur->numlower : 0),
			     GFP_KERNEL);
		if (!pk) {
			aufsng_free_entry(neu);
			continue;
		}
		aufsng_dyn_commit_rebuild(pfs, dirs[i], neu, pk);
		aufsng_dyn_drop_neg_children(dirs[i]);
	}

	aufsng_memo_free(memo);
	kfree(memo);
out_iput:
	for (i = 0; i < ndirs; i++)
		iput(dirs[i]);
	kfree(dirs);
}

int aufsng_dyn_add_branch(struct super_block *sb, const char *name,
		       const struct path *path, unsigned int pos,
		       enum aufsng_br_perm perm, const char *permstr)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);
	struct inode *root_inode = aufsng_root_inode(sb);
	struct aufsng_entry *old_oe, *new_oe, *cur_oe;
	struct aufsng_path *nstack, *ostack;
	struct aufsng_layer *layer;
	struct vfsmount *mnt;
	char *dup_name;
	unsigned int n, idx;
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
	pfs->config.br_paths[idx] = dup_name;
	strscpy(pfs->config.br_perms[idx], permstr, AUFSNG_PERM_LEN);
	if (idx == pfs->numlayer)
		pfs->numlayer++;

	/*
	 * The root stack swap plus the in-place splice into every
	 * cached directory refresh the whole tree without invalidating
	 * any dentry, so nested mounts under the union are never
	 * detached (a d_invalidate-driven refresh would detach them).
	 */
	old_oe = aufsng_dyn_swap_root(sb, new_oe);
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
	unsigned int i;

	if (force_shrink || !*dcache_fresh) {
		shrink_dcache_sb(sb);
		evict_inodes(sb);
		*dcache_fresh = true;
	}

again:
	/*
	 * A restart rewalks the whole list, so the previous pass's
	 * collection must be dropped first: every in-use directory would
	 * otherwise be collected once more per restart, growing
	 * scan->dirs (and the allocations later sized by scan->nr) by
	 * O(in-use dirs) for each cache-only inode evicted below.
	 */
	for (i = 0; i < scan->nr; i++)
		iput(scan->dirs[i]);
	scan->nr = 0;
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
				  struct aufsng_dyn_parked *parked)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_entry *old_oe;
	unsigned int i;

	/*
	 * No inode_lock() here: pfs->dyn_lock (write, held by caller)
	 * alone excludes every other oe reader/writer in this design
	 * (aufsng_lookup(), readdir's cache build, etc. all serialize on
	 * dyn_lock, not this directory's inode lock).  Taking
	 * inode_lock() too would deadlock: a concurrent lookup holds
	 * the parent's i_rwsem (shared) before calling ->lookup(),
	 * which blocks on down_read(&pfs->dyn_lock) - ABBA with
	 * inode_lock(inode)+dyn_lock(write) here.
	 *
	 * @parked must have room for old_oe->numlower mounts (see the
	 * struct aufsng_dyn_parked comment: each parked stack pins
	 * every branch mount its dentries point into).
	 */
	old_oe = oi->oe;
	/* release-publish for lockless READ_ONCE readers, as in swap_root */
	smp_store_release(&oi->oe, new_oe);
	atomic64_inc(&oi->version);
	aufsng_dyn_rekey_inode(inode, new_oe);

	parked->oe = old_oe;
	parked->upper = NULL;
	parked->nr_mnt = old_oe ? old_oe->numlower : 0;
	for (i = 0; i < parked->nr_mnt; i++)
		parked->mnts[i] = mntget(old_oe->lowerstack[i].layer->mnt);
	parked->next = oi->dyn_parked;
	oi->dyn_parked = parked;
}

void aufsng_dyn_put_parked(struct aufsng_inode *oi)
{
	struct aufsng_dyn_parked *p;

	for (p = oi->dyn_parked; p; p = p->next) {
		unsigned int i;

		if (p->oe)
			for (i = 0; i < p->oe->numlower; i++)
				dput(p->oe->lowerstack[i].dentry);
		dput(p->upper);
		/* only after every dentry into those branches is gone */
		for (i = 0; i < p->nr_mnt; i++)
			mntput(p->mnts[i]);
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
	kfree(pfs->config.br_paths[layer->idx]);
	pfs->config.br_paths[layer->idx] = NULL;
	pfs->config.br_perms[layer->idx][0] = '\0';
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
			pr_info("aufs (aufs-ng): cannot remove branch '%pd': %u file(s) in use\n",
				path->dentry, scan.nr_busy);
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
		struct aufsng_entry *cur;

		new_oes[i] = aufsng_dyn_prep_rebuild(scan.dirs[i], layer);
		if (IS_ERR(new_oes[i])) {
			new_oes[i] = NULL;
			goto out_unlock;
		}
		if (!new_oes[i])
			continue;
		/* sized to pin every mount the superseded stack references */
		cur = AUFSNG_I_E(scan.dirs[i]);
		parked[i] = kmalloc(struct_size(parked[i], mnts,
						cur ? cur->numlower : 0),
				    GFP_KERNEL);
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
	 * Removal invalidates no dentries: shrink+evict+in-place
	 * rebuild (below, still under dyn_lock write) already keeps
	 * every surviving cached dentry consistent, and invalidating
	 * would strand pinned dentries (open fds, CWDs) that no fresh
	 * lookup can replace, breaking fd-based re-resolution with
	 * ESTALE.
	 */
	old_root_oe = aufsng_dyn_swap_root(sb, new_oe);

	/*
	 * Swap in the prepared stacks of the collected in-use
	 * directories before releasing dyn_lock: a lookup through one
	 * of these directories takes dyn_lock for reading, so as long
	 * as we still hold it for writing here, no lookup can resolve
	 * a child through a stack that still references the removed
	 * branch.
	 */
	for (i = 0; i < scan.nr; i++) {
		if (new_oes[i]) {
			aufsng_dyn_commit_rebuild(pfs, scan.dirs[i], new_oes[i],
					      parked[i]);
			/*
			 * Removing a branch that carried a whiteout reveals the
			 * name it was hiding in a lower-priority branch, so drop
			 * cached negative children whose "absent" verdict the
			 * removal may have overturned - the same refresh the add
			 * path does after splicing.
			 */
			aufsng_dyn_drop_neg_children(scan.dirs[i]);
		}
		iput(scan.dirs[i]);
	}
	kfree(scan.dirs);
	kfree(new_oes);
	kfree(parked);

	/* the busy-scan skips the root; refresh its negatives too */
	aufsng_dyn_drop_neg_children(root_inode);

	pr_info("aufs (aufs-ng): branch '%pd' removed\n", path->dentry);
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

		err = aufsng_dyn_add_branch(sb, b->name, &b->path, pos, b->perm,
					 b->permstr);
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
