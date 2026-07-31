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
 *    so the most recently added branch has top priority.  Only N=1 is
 *    supported - the in-place splice below and its blocked/unhash
 *    verdicts assume the added branch is the new TOP lower - and the
 *    parser rejects any other index (real live-distro usage only ever
 *    specifies N=1).
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
#include <linux/cred.h>
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
 * Fill @pk and link it onto @oi's parked list: @oe is the superseded
 * stack the node OWNS (freed at eviction; NULL for a pin-only node),
 * @upper the superseded upper it owns, and @mnt_src the stack whose
 * branch mounts it must PIN - every mount a parked stack's dentries
 * point into has to outlive the branch's kern_unmount(), or the branch
 * is torn down under them ("Dentry still in use" on umount).
 *
 * Owned stack and pinned stack are separate parameters on purpose: a
 * pin-only node leaves the inode's LIVE stack in place and only pins its
 * mounts, so passing that stack as @oe would have eviction dput and free
 * a stack still in use.  @pk must have room for @mnt_src's lowers.
 * Caller holds oi->lock.
 */
static void aufsng_dyn_park_fill(struct aufsng_inode *oi,
			      struct aufsng_dyn_parked *pk,
			      struct aufsng_entry *oe,
			      struct dentry *upper,
			      struct aufsng_entry *mnt_src)
{
	unsigned int i;

	pk->oe = oe;
	pk->upper = upper;
	pk->nr_mnts = mnt_src ? mnt_src->numlower : 0;
	for (i = 0; i < pk->nr_mnts; i++)
		pk->mnts[i] = mntget(mnt_src->lowerstack[i].mnt);
	pk->next = oi->dyn_parked;
	oi->dyn_parked = pk;
}

/*
 * Park the inode's current upper on the dyn_parked list - a lockless
 * aufsng_path_real() reader may still hold a pointer to it until the
 * inode is evicted - and publish @new_upper (NULL sheds the upper so
 * the top lower resurfaces), refreshing the mirrored attributes and
 * the merged-readdir version to follow the new top real object.  A
 * pin-only node (no mounts): a bare upper points into branch 0, which
 * outlives every inode.  Returns false on allocation failure, with
 * nothing changed.  Caller holds oi->lock.
 */
static bool aufsng_dyn_park_upper(struct aufsng_inode *oi, struct inode *inode,
			       struct dentry *new_upper)
{
	struct aufsng_dyn_parked *pk;

	pk = kmalloc(struct_size(pk, mnts, 0), GFP_KERNEL);
	if (!pk)
		return false;
	aufsng_dyn_park_fill(oi, pk, NULL, oi->upperdentry, NULL);
	WRITE_ONCE(oi->upperdentry, new_upper ? dget(new_upper) : NULL);
	aufsng_copyattr(inode);
	atomic64_inc(&oi->version);
	return true;
}

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
	if (!aufsng_origin_type_ok(upperdentry, inode->i_mode))
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
		/* the upper is the authoritative real object now */
		aufsng_copyattr(inode);
		ok = true;
	} else if (d_inode(oi->upperdentry) == d_inode(upperdentry)) {
		ok = true;
	} else if (aufsng_dyn_park_upper(oi, inode, upperdentry)) {
		ok = true;
		replaced = true;
	}
out:
	mutex_unlock(&oi->lock);

	if (replaced && is_dir)
		aufsng_dyn_drop_neg_children(inode);

	return ok;
}

/*
 * Shed a DEAD upper (unhashed or turned negative by an out-of-band
 * unlink/rename in the rw branch) from a lower-backed inode, so the
 * lower resurfaces exactly as udba=reval promises.  Keeping it would
 * serve the deleted upper's content forever and route write opens into
 * the unlinked inode (they skip copy-up while upperdentry is set).
 * The upper is parked, not dropped: a lockless aufsng_path_real()
 * reader may still hold a pointer to it until the inode is evicted.
 * Returns false if the upper turned out to be alive after all (the
 * caller's unlocked read raced a re-adopt) or on allocation failure -
 * the caller then falls back to its no-heal path.
 */
bool aufsng_dyn_shed_upper(struct inode *inode)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_entry *oe;
	bool shed = false;
	bool ok = false;

	mutex_lock(&oi->lock);
	if (!oi->upperdentry) {
		ok = true;	/* raced: already gone */
		goto out;
	}
	oe = oi->oe;
	if (!oe || !oe->numlower)
		goto out;	/* nothing to resurface: keep the upper */
	if (aufsng_dentry_alive(oi->upperdentry)) {
		ok = true;	/* alive after all: cached state wins */
		goto out;
	}

	if (!aufsng_dyn_park_upper(oi, inode, NULL))
		goto out;
	ok = true;
	shed = true;
out:
	mutex_unlock(&oi->lock);
	/*
	 * Losing the upper can resurface lower names that a whiteout or
	 * opaque marker in the just-shed upper directory was hiding.  Cached
	 * negative children were primed against the has-upper state, and
	 * aufsng_lookup_negative_valid()'s !pupper shortcut would now keep
	 * them valid forever - drop them so those names re-resolve, exactly
	 * as the adopt path does on the reverse transition.
	 */
	if (shed && S_ISDIR(inode->i_mode))
		aufsng_dyn_drop_neg_children(inode);
	return ok;
}

/*
 * Re-key a cached inode in the inode hash when its top real object
 * changes, recomputing i_ino exactly as a fresh lookup would
 * (aufsng_get_inode): the number folds in the providing branch's slot,
 * so a re-pointed object must not keep a number minted from the
 * REMOVED branch's slot - readdir derives d_ino live from the current
 * stack (d_ino must match st_ino), and a later add= reusing the freed
 * slot could otherwise mint a different file with the identical
 * (st_dev, st_ino), which archivers treat as a hardlink.
 */
static void aufsng_dyn_rekey_inode(struct aufsng_fs *pfs, struct inode *inode,
				struct aufsng_entry *oe)
{
	unsigned int key_idx;
	struct inode *key = aufsng_hash_key(pfs, oe,
					    aufsng_upperdentry(inode),
					    &key_idx);

	if (inode->i_private == key)
		return;
	if (!inode_unhashed(inode))
		remove_inode_hash(inode);
	inode->i_private = key;
	if (key) {
		inode->i_ino = aufsng_map_ino(key->i_ino, key_idx);
		__insert_inode_hash(inode, (unsigned long)key);
	}
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
	struct dentry *branchd;	/* counterpart, ref held; NULL = absent */
	/*
	 * The new branch actively HIDES this union object (a whiteout or
	 * same-named non-directory in the now-top branch, with no upper
	 * to win over it) - stronger than "absent": the union name is
	 * gone, and every cached directory at or below it must be
	 * unhashed rather than kept on its pre-add stack.
	 */
	bool blocked;
};

/*
 * The upper diropq verdict is per PARENT, not per child: without its
 * own memo, k sibling directories under one memo-missed parent would
 * each probe the same upper directory's ".wh..wh..opq" once, inside
 * the whole-filesystem stall dyn_lock(write) imposes.
 */
struct aufsng_opq_ent {
	struct hlist_node node;
	struct dentry *pupper;	/* key; ref held for the memo's lifetime */
	bool opq;
};

#define AUFSNG_OPQ_MEMO_BITS 6

struct aufsng_splice_memo {
	DECLARE_HASHTABLE(tbl, AUFSNG_MEMO_BITS);
	DECLARE_HASHTABLE(opq_tbl, AUFSNG_OPQ_MEMO_BITS);
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
			   struct dentry *d, struct dentry *branchd,
			   bool blocked)
{
	struct aufsng_memo_ent *e = kmalloc(sizeof(*e), GFP_KERNEL);

	if (!e)
		return;
	e->uniond = dget(d);
	e->branchd = branchd ? dget(branchd) : NULL;
	e->blocked = blocked;
	hash_add(memo->tbl, &e->node, (unsigned long)d);
}

/* memoized "is this upper directory opaque?"; probes on the first miss */
static int aufsng_memo_diropq(struct aufsng_fs *pfs,
			   struct aufsng_splice_memo *memo,
			   struct dentry *pupper)
{
	struct aufsng_opq_ent *e;
	int opq;

	hash_for_each_possible(memo->opq_tbl, e, node, (unsigned long)pupper) {
		if (e->pupper == pupper)
			return e->opq;
	}
	opq = aufsng_check_diropq(aufsng_upper_mnt(pfs), pupper);
	if (opq < 0)
		return opq;	/* transient: let the next child re-probe */
	/* best effort: an allocation failure just skips the memoization */
	e = kmalloc(sizeof(*e), GFP_KERNEL);
	if (e) {
		e->pupper = dget(pupper);
		e->opq = opq;
		hash_add(memo->opq_tbl, &e->node, (unsigned long)pupper);
	}
	return opq;
}

static void aufsng_memo_free(struct aufsng_splice_memo *memo)
{
	struct aufsng_memo_ent *e;
	struct aufsng_opq_ent *oq;
	struct hlist_node *tmp;
	unsigned int i;

	hash_for_each_safe(memo->tbl, i, tmp, e, node) {
		hash_del(&e->node);
		dput(e->uniond);
		dput(e->branchd);
		kfree(e);
	}
	hash_for_each_safe(memo->opq_tbl, i, tmp, oq, node) {
		hash_del(&oq->node);
		dput(oq->pupper);
		kfree(oq);
	}
}

/*
 * Advance one path component: given the union parent's inode @punion
 * and the already-resolved branch parent @pbase, resolve the union
 * child dentry @d in the new branch, enforcing the same per-level
 * visibility rules a fresh lookup would.  Returns the branch child
 * (ref held) or NULL when the path is not provided: also on transient
 * errors - skipping is safe, the directory just keeps its pre-add
 * view until evicted, same as one deeper than the replay limit.
 *
 * @blocked is set when the new branch actively HIDES the union object
 * at this level: it carries a whiteout for the name, or provides a
 * same-named non-directory, and the union object has no upper to win
 * over the new top lower.  A fresh lookup of that path would no longer
 * find the cached directory at all, so the caller must unhash it
 * instead of keeping its pre-add view.
 */
static struct dentry *aufsng_dyn_resolve_step(struct aufsng_fs *pfs,
					   struct vfsmount *mnt,
					   struct inode *punion,
					   struct dentry *pbase,
					   struct dentry *d,
					   struct aufsng_splice_memo *memo,
					   bool *blocked)
{
	struct dentry *pupper = aufsng_upperdentry(punion);
	struct dentry *child = NULL;
	struct name_snapshot ns;
	int wh;

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
	if (pupper && aufsng_memo_diropq(pfs, memo, pupper))
		return NULL;

	/*
	 * The name is read via a snapshot.  Cross-directory union
	 * renames - the ones that re-parent, i.e. would tear the
	 * ancestor chain this walk replays - are excluded outright: the
	 * splice pass holds the union sb's s_vfs_rename_mutex, which
	 * lock_rename() holds across ->rename() AND the union-level
	 * d_move().  (dyn_lock alone would not do: aufsng_rename()
	 * releases it before the VFS runs d_move.)  Same-directory
	 * renames take no rename mutex, so a concurrent one can still
	 * swap a d_name mid-walk; the snapshot makes that read safe,
	 * and at worst the walk resolves the pre-rename name and this
	 * directory keeps its pre-add view until evicted - the same
	 * fallback as any transient probe error below.
	 */
	take_dentry_name_snapshot(&ns, d);
	if (pupper &&
	    aufsng_check_whiteout(aufsng_upper_mnt(pfs), pupper, &ns.name))
		goto out;
	/*
	 * The new branch's own whiteout: as the new TOP lower it hides
	 * the name in every branch below - if no upper provides the
	 * union object, the object itself is gone from the union.  A
	 * probe error skips instead (transient: keep the pre-add view).
	 */
	wh = aufsng_check_whiteout(mnt, pbase, &ns.name);
	if (wh) {
		if (wh == 1)
			*blocked = !aufsng_upperdentry(d_inode(d));
		goto out;
	}

	child = lookup_one_positive_unlocked(mnt_idmap(mnt), &ns.name, pbase);
	if (IS_ERR(child)) {
		child = NULL;
	} else if (!d_is_dir(child)) {
		/*
		 * A same-named non-directory in the new top lower ends
		 * the merge at itself: a lower-only union directory is
		 * shadowed by it, exactly as if deleted and replaced.
		 */
		*blocked = !aufsng_upperdentry(d_inode(d));
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
 * provide this path as a visible directory.  *@blocked is set when the
 * new branch actively hides this directory or one of its ancestors
 * (see aufsng_dyn_resolve_step): the path no longer exists in the
 * union at all.
 */
static struct dentry *aufsng_dyn_resolve_lower(struct aufsng_fs *pfs,
					    struct super_block *sb,
					    struct inode *inode,
					    struct vfsmount *mnt,
					    struct aufsng_splice_memo *memo,
					    bool *blocked)
{
	struct dentry *stack[AUFSNG_RESOLVE_MAXDEPTH];
	struct dentry *cur, *par, *base;
	unsigned int n = 0, i;

	cur = d_find_alias(inode);
	if (!cur)
		return NULL;
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
		bool blk = false;

		if (hit) {
			child = hit->branchd ? dget(hit->branchd) : NULL;
			blk = hit->blocked;
		} else {
			child = aufsng_dyn_resolve_step(pfs, mnt, d_inode(pu),
						     base, stack[i], memo,
						     &blk);
			aufsng_memo_store(memo, stack[i], child, blk);
		}
		dput(base);
		base = child;
		if (blk) {
			/* an ancestor gone hides the whole subtree */
			*blocked = true;
			break;
		}
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
	unsigned int base_n;
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
	neu = aufsng_entry_prepend(cur, base_n, layer, nd, layer->mnt);
	return neu ? neu : ERR_PTR(-ENOMEM);
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
	unsigned int ndirs = 0, i;
	size_t cap = 0;
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
		if (aufsng_grow_array((void **)&dirs, &cap, ndirs + 1,
				   sizeof(*dirs), GFP_ATOMIC)) {
			spin_unlock(&inode->i_lock);
			break;	/* best effort: skip the remainder */
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
	hash_init(memo->opq_tbl);

	for (i = 0; i < ndirs; i++) {
		struct aufsng_entry *cur, *neu;
		struct aufsng_dyn_parked *pk;
		bool blocked = false;
		struct dentry *nd = aufsng_dyn_resolve_lower(pfs, sb, dirs[i],
							  layer->mnt, memo,
							  &blocked);

		if (blocked) {
			/*
			 * The new branch hides this directory (a whiteout
			 * or same-named non-directory in the now-top
			 * branch, no upper to win over it): the name is
			 * gone from the union, exactly as if deleted.
			 * Unhash the alias so fresh lookups re-resolve
			 * (to the shadowing file, or to ENOENT); pinned
			 * users keep the old view until they let go, as
			 * with any deleted-while-open directory.
			 */
			struct dentry *alias = d_find_alias(dirs[i]);

			if (alias) {
				d_drop(alias);
				dput(alias);
			}
			atomic64_inc(&AUFSNG_I(dirs[i])->version);
			continue;
		}
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
		       const struct path *path, const char *permstr)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);
	struct inode *root_inode = aufsng_root_inode(sb);
	struct aufsng_entry *old_oe, *new_oe, *cur_oe;
	struct aufsng_layer *layer;
	struct vfsmount *mnt;
	char *dup_name;
	unsigned int idx;
	long namelen;
	int err;

	idx = aufsng_find_free_slot(pfs);
	if (idx == pfs->numlayer && pfs->numlayer >= pfs->numlayer_cap)
		return -ENOSPC;

	err = aufsng_check_layer(sb, path, name);
	if (err)
		return err;
	if (aufsng_dyn_find_branch(pfs, path->dentry))
		return -EEXIST;	/* legacy remount replay tolerates this */
	err = aufsng_check_overlap(pfs, path->dentry, name);
	if (err)
		return err;
	/*
	 * The new branch's name-length limit, folded in as mount time does
	 * for every initial branch: the advertised limit must fit the
	 * SHALLOWEST branch (see aufsng_get_namelen()).  Only PROBED here -
	 * the clamp is applied under the locks below, once nothing can fail
	 * anymore: a shrunken limit is never widened back (it is a
	 * mount-lifetime promise to userspace), so applying it before a
	 * kstrdup or clone_private_mount failure would leave statfs
	 * under-reporting f_namelen forever for a branch that was never
	 * added.
	 */
	err = aufsng_probe_namelen(path, &namelen);
	if (err)
		return err;

	dup_name = kstrdup(name, GFP_KERNEL);
	if (!dup_name)
		return -ENOMEM;

	mnt = clone_private_mount(path);
	if (IS_ERR(mnt)) {
		err = PTR_ERR(mnt);
		goto out_name;
	}
	/*
	 * An added branch is always a lower (never branch 0), and only
	 * branch 0 is ever written: even a "=rw" add gets a read-only
	 * private clone, as in aufsng_fill_super().
	 */
	mnt->mnt_flags |= MNT_READONLY | MNT_NOATIME;

	cur_oe = AUFSNG_I_E(root_inode);

	/*
	 * The new branch becomes the top lower (add=1, the only mode).  @mnt
	 * is passed explicitly because layer->mnt is only published below,
	 * once the locks are held.
	 */
	layer = &pfs->layers[idx];
	new_oe = aufsng_entry_prepend(cur_oe, cur_oe->numlower, layer,
				  path->dentry, mnt);
	if (!new_oe) {
		err = -ENOMEM;
		goto out_mnt;
	}

	/*
	 * s_vfs_rename_mutex excludes cross-directory union renames -
	 * including their VFS-level d_move(), which runs after
	 * ->rename() returns and re-parents the very ancestor chains
	 * the splice pass replays (see aufsng_dyn_resolve_step()).
	 * Taken before the root's i_rwsem, matching lock_rename()'s own
	 * ordering.
	 */
	mutex_lock(&sb->s_vfs_rename_mutex);
	inode_lock(root_inode);
	percpu_down_write(&pfs->dyn_lock);

	layer->mnt = mnt;
	pfs->namelen = min(pfs->namelen, namelen);
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
	/* one re-check of every cached lower-only dentry (dcache.c) */
	atomic_long_inc(&pfs->branch_gen);

	percpu_up_write(&pfs->dyn_lock);
	inode_unlock(root_inode);
	mutex_unlock(&sb->s_vfs_rename_mutex);

	/*
	 * The grace period only orders freeing the superseded ROOT entry
	 * against its RCU readers (show_options, getattr's dir-nlink
	 * walk).  Expedited: a plain synchronize_rcu() stalls every
	 * module load ~10-40ms for a stack nothing hot depends on -
	 * precedent in kern_unmount_array().  (Parking the root entry
	 * instead would mntget every pre-existing lower until umount,
	 * keeping removed branches' loop devices attached - worse.)
	 */
	synchronize_rcu_expedited();
	aufsng_free_entry(old_oe);

	pr_info("aufs (aufs-ng): branch '%s' added\n", name);
	return 0;

out_mnt:
	/*
	 * clone_private_mount() clones are "longterm" mounts
	 * (mnt_ns == MNT_NS_INTERNAL): plain mntput() never tears one
	 * down - only kern_unmount() does, via mnt_make_shortterm().
	 */
	kern_unmount(mnt);
out_name:
	kfree(dup_name);
	return err;
}

struct aufsng_dyn_scan {
	struct inode **pinned;
	unsigned int nr;
	size_t cap;
	unsigned int nr_busy;
	u64 busy_ino;
};

static bool aufsng_entry_has_layer(struct aufsng_entry *oe,
				const struct aufsng_layer *layer)
{
	unsigned int i;

	for (i = 0; oe && i < oe->numlower; i++) {
		/*
		 * Match the mount too, not the slot pointer alone: a
		 * deleted-but-open inode keeps a stack referencing a
		 * REMOVED branch (aufsng_dyn_pin_stack), and a later add
		 * may reuse that layer slot for a different branch - the
		 * stale entry must not claim the new branch.
		 */
		if (oe->lowerstack[i].layer == layer &&
		    oe->lowerstack[i].mnt == layer->mnt)
			return true;
	}
	return false;
}

/*
 * A directory removed through the union but still pinned by a cwd or an
 * open fd: rmdir cleared its link count, so no fresh lookup can ever
 * reach it again.  Exactly like a deleted-but-open FILE it must not
 * block a branch removal - there is nothing to re-point and nothing to
 * re-point to - so it takes the same pin-only path
 * (aufsng_dyn_pin_stack): the stack stays, its mounts pinned, until the
 * last user lets go.  Without this it wedged the removal instead, with
 * -EBUSY once the whtmp'd upper made aufsng_copy_up() a no-op, or
 * -ENOENT straight from copy-up's dead-name early-out.
 */
static bool aufsng_dyn_dir_gone(struct inode *inode)
{
	return S_ISDIR(inode->i_mode) && !inode->i_nlink;
}

/*
 * True when something other than @layer still backs @inode: a live
 * upper, or a lower entry from another branch.  A dead upper -
 * unhashed by an out-of-band unlink in the rw branch under udba=reval,
 * pending the shed-upper heal (aufsng_get_inode) - is not a survivor:
 * treating it as one would strip the stack the heal needs and serve
 * the deleted upper forever.
 */
static bool aufsng_dyn_has_survivor(struct inode *inode,
				 const struct aufsng_layer *layer)
{
	struct aufsng_entry *oe = AUFSNG_I_E(inode);
	struct dentry *upper = aufsng_upperdentry(inode);
	unsigned int i;

	if (upper && aufsng_dentry_alive(upper))
		return true;
	for (i = 0; oe && i < oe->numlower; i++)
		if (oe->lowerstack[i].layer != layer)
			return true;
	return false;
}

/*
 * Is @inode's real object on @layer the target of a live memory
 * mapping?  Checked on the BACKING inode: aufsng_mmap maps through
 * backing_file_mmap, which links the vma into the real inode's
 * address_space - the union inode's own i_mapping never sees a
 * mapping.  Non-sleeping; called under i_lock.
 */
static bool aufsng_dyn_mapped_on_layer(struct inode *inode,
				    const struct aufsng_layer *layer)
{
	struct aufsng_entry *oe = AUFSNG_I_E(inode);
	unsigned int i;

	for (i = 0; oe && i < oe->numlower; i++)
		if (oe->lowerstack[i].layer == layer &&
		    oe->lowerstack[i].mnt == layer->mnt &&
		    mapping_mapped(d_inode(oe->lowerstack[i].dentry)->i_mapping))
			return true;
	return false;
}

/*
 * Rebuild a pinned non-directory's stack for the removal of @layer.
 * A non-directory stack records only the topmost provider (namei.c),
 * so the survivor cannot be read off the old stack: re-resolve the
 * name against the surviving branches (aufsng_find_origin_ex with the
 * removed layer skipped and the type check applied), exactly as the
 * first fresh lookup after the removal will, so the rekeyed inode
 * keeps the identity (hash key, st_ino) a fresh lookup computes.
 *
 * The alias's name is read through a snapshot and its parent through
 * its own reference: dyn_lock does NOT keep either stable - a union
 * rename releases it before the VFS runs d_move (see the matching
 * comment in aufsng_dyn_resolve_step()), and a same-directory rename
 * takes no rename mutex at all, so a concurrent one can swap (or
 * kfree_rcu) the d_name this walk would otherwise dereference across
 * sleeping branch lookups.  Cross-directory renames - the ones that
 * would re-parent the alias mid-walk - are excluded by the caller's
 * s_vfs_rename_mutex, exactly as on the add path.
 *
 * When no name resolves to a survivor and no live upper carries the
 * object, it is invisible to fresh lookups already (deleted through
 * the union while held open, or every alias evicted): *@pin_only is
 * set and NULL returned - the caller keeps the stack and pins its
 * mounts until eviction (aufsng_dyn_pin_stack()) instead of failing
 * the whole removal with EBUSY for an object the union no longer
 * shows.  (With several hardlink aliases sharing this inode the
 * arbitrary d_find_alias() pick decides which name is re-resolved;
 * per-name divergence for the un-chosen sibling is inherent to the
 * one-inode-per-lower-origin design.)  Returns the rebuilt entry, or
 * ERR_PTR on error.  Caller holds the root inode lock,
 * s_vfs_rename_mutex and pfs->dyn_lock for writing.
 */
static struct aufsng_entry *aufsng_dyn_prep_repoint(struct aufsng_fs *pfs,
					      struct inode *inode,
					      const struct aufsng_layer *layer,
					      bool *pin_only)
{
	struct aufsng_path origin = { NULL, NULL };
	struct dentry *alias, *upper;
	bool upper_alive;
	int found = 0;

	*pin_only = false;
	upper = aufsng_upperdentry(inode);
	upper_alive = upper && aufsng_dentry_alive(upper);

	alias = d_find_alias(inode);
	if (alias) {
		struct dentry *parent = dget_parent(alias);
		const struct cred *old_cred;
		struct name_snapshot ns;

		take_dentry_name_snapshot(&ns, alias);
		old_cred = override_creds(pfs->creator_cred);
		/*
		 * The gate a fresh lookup applies, so the two agree.  A
		 * DEAD upper is passed as none: it carries no readable
		 * marker (it has no inode at all), and the object is about
		 * to be served by a lower again anyway - which is exactly
		 * what re-resolving finds.
		 */
		if (aufsng_upper_claims_origin(pfs,
					    upper_alive ? upper : NULL,
					    &ns.name))
			found = aufsng_find_origin_ex(AUFSNG_E(parent),
						   &ns.name, layer,
						   inode->i_mode, &origin);
		revert_creds(old_cred);
		release_dentry_name_snapshot(&ns);
		dput(parent);
		dput(alias);
		if (found < 0)
			return ERR_PTR(found);
	}

	if (!found && !upper_alive) {
		*pin_only = true;
		return NULL;
	}

	return aufsng_entry_from_origin(found, &origin);
}

/*
 * Pin every branch mount @oe references on @oi's parked list WITHOUT
 * swapping the stack: a deleted-but-open (or alias-less) object whose
 * serving branch is being removed keeps working through its open fds
 * exactly like any deleted-while-open file, but the stack's mounts
 * must outlive the branch's kern_unmount().  @pk has room for
 * oe->numlower mounts.  Caller holds pfs->dyn_lock for writing.
 */
static void aufsng_dyn_pin_stack(struct aufsng_inode *oi,
			      struct aufsng_entry *oe,
			      struct aufsng_dyn_parked *pk)
{
	mutex_lock(&oi->lock);
	/* pin the KEPT stack's mounts; the stack itself stays live */
	aufsng_dyn_park_fill(oi, pk, NULL, NULL, oe);
	mutex_unlock(&oi->lock);
}

/*
 * Drop every cache-only reference to @layer and classify what
 * remains: a non-directory whose @layer object is memory-mapped makes
 * the branch busy (a live mapping cannot have its backing pulled);
 * every other in-use inode - directories (ancestors of running
 * binaries, working directories) and re-pointable files alike - is
 * collected so its stack can be rebuilt without the branch.
 * Caller holds the root inode lock and pfs->dyn_lock for writing, so
 * no new reference can appear while we look.
 */
static int aufsng_dyn_scan_branch(struct super_block *sb,
			       const struct aufsng_layer *layer,
			       struct aufsng_dyn_scan *scan)
{
	struct inode *inode, **dispose = NULL;
	unsigned int i, nd = 0;
	size_t dcap = 0;
	int err = 0;

	/*
	 * Shrink INSIDE the removal locks, every pass.  i_count is the only
	 * thing separating "a process is using this" from "the dcache merely
	 * remembers it", so a lookup landing between an unlocked shrink and
	 * this walk leaves a cache-only inode looking exactly like an open
	 * fd - and a sole-backed directory misclassified that way gets
	 * physically copied up into the rw branch, or fails the removal
	 * outright if that copy-up errors.  Holding dyn_lock for writing
	 * here keeps lookups out for the whole shrink-then-classify window.
	 */
	shrink_dcache_sb(sb);
	evict_inodes(sb);

	/* a fresh pass (retry loop): drop the previous one's collection */
	for (i = 0; i < scan->nr; i++)
		iput(scan->pinned[i]);
	scan->nr = 0;
	scan->nr_busy = 0;
	scan->busy_ino = 0;
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
			if (!S_ISDIR(inode->i_mode) &&
			    aufsng_dyn_mapped_on_layer(inode, layer)) {
				if (!scan->nr_busy)
					scan->busy_ino = inode->i_ino;
				scan->nr_busy++;
				spin_unlock(&inode->i_lock);
				continue;
			}
			err = aufsng_grow_array((void **)&scan->pinned,
					     &scan->cap, scan->nr + 1,
					     sizeof(*scan->pinned),
					     GFP_ATOMIC);
			if (err) {
				spin_unlock(&inode->i_lock);
				break;
			}
			atomic_inc(&inode->i_count);
			scan->pinned[scan->nr++] = inode;
			spin_unlock(&inode->i_lock);
			continue;
		}
		/*
		 * Cache-only: mark I_DONTCACHE and take a reference so
		 * the deferred iput below evicts it - AFTER the walk,
		 * not here.  An in-place iput would have to drop the
		 * list lock and restart the walk, re-scanning the whole
		 * list (and re-collecting every pinned inode) once per
		 * evicted inode - O(n * evicted) under dyn_lock held
		 * for writing.  A single forward pass never revisits an
		 * inode, so nothing is double-counted.
		 */
		err = aufsng_grow_array((void **)&dispose, &dcap, nd + 1,
				     sizeof(*dispose), GFP_ATOMIC);
		if (err) {
			spin_unlock(&inode->i_lock);
			break;
		}
		inode_state_set(inode, I_DONTCACHE);
		atomic_inc(&inode->i_count);
		dispose[nd++] = inode;
		spin_unlock(&inode->i_lock);
	}
	spin_unlock(&sb->s_inode_list_lock);

	for (i = 0; i < nd; i++)
		iput(dispose[i]);
	kfree(dispose);

	return err;
}

/*
 * Copy up the collected directories that would lose their only
 * backing with @layer; survivor-backed ones keep being served from a
 * surviving branch and need nothing.
 */
static int aufsng_dyn_copy_up_dirs(struct aufsng_dyn_scan *scan,
				const struct aufsng_layer *layer)
{
	unsigned int i;
	int err = 0;

	for (i = 0; !err && i < scan->nr; i++) {
		struct inode *inode = scan->pinned[i];
		struct dentry *alias;

		if (!S_ISDIR(inode->i_mode) ||
		    aufsng_dyn_dir_gone(inode) ||
		    aufsng_dyn_has_survivor(inode, layer))
			continue;
		alias = d_find_alias(inode);
		if (!alias)
			continue;
		err = aufsng_copy_up(alias);
		dput(alias);
	}
	return err;
}

/*
 * Filter @layer out of a cached directory's stack.  When the removed
 * entry was the BOTTOM of the stack and carried an opaque marker, the
 * merge that built this stack stopped AT the removed branch: branches
 * below it in the parent's stack were never probed, though a fresh
 * lookup after the removal will now merge them (the marker leaves the
 * union with its branch).  Re-run that tail of the merge here, by the
 * same rules as aufsng_lookup (stop at a whiteout, a non-directory, or
 * an opaque directory), so the pinned directory and a fresh lookup of
 * the same path keep answering identically - a filter alone can only
 * shorten the stack and would leave the pinned view permanently
 * missing everything the removed opaque branch was hiding.
 * @tail_merge is false for the ROOT, whose stack is the branch list
 * itself, not the product of a merge.  Caller holds the root inode
 * lock, s_vfs_rename_mutex (keeping the alias name/parent walk safe,
 * as in aufsng_dyn_prep_repoint) and pfs->dyn_lock for writing.
 */
static struct aufsng_entry *aufsng_dyn_prep_rebuild(struct aufsng_fs *pfs,
					      struct inode *inode,
					      const struct aufsng_layer *layer,
					      bool tail_merge)
{
	struct aufsng_entry *old_oe = AUFSNG_I_E(inode), *new_oe;
	struct aufsng_path *nstack, *ostack;
	struct dentry *alias = NULL, *parent = NULL;
	struct aufsng_entry *poe = NULL;
	unsigned int i, j, n, hits = 0, tail_from = 0, tail_room = 0;
	int err = 0;

	n = old_oe->numlower;
	ostack = old_oe->lowerstack;
	for (i = 0; i < n; i++) {
		if (ostack[i].layer == layer)
			hits++;
	}
	if (!hits)
		return NULL;

	if (tail_merge && ostack[n - 1].layer == layer) {
		const struct cred *old_cred = override_creds(pfs->creator_cred);
		int opq = aufsng_check_diropq(ostack[n - 1].mnt,
					   ostack[n - 1].dentry);

		revert_creds(old_cred);
		if (opq < 0)
			return ERR_PTR(opq);
		if (opq) {
			alias = d_find_alias(inode);
			if (alias)
				parent = dget_parent(alias);
			poe = parent ? AUFSNG_E(parent) : NULL;
			/* the merge resumes below the removed layer's position */
			for (tail_from = 0; poe && tail_from < poe->numlower;
			     tail_from++) {
				if (poe->lowerstack[tail_from].layer == layer) {
					tail_from++;
					tail_room = poe->numlower - tail_from;
					break;
				}
			}
		}
	}

	new_oe = aufsng_alloc_entry(n - hits + tail_room);
	if (!new_oe) {
		new_oe = ERR_PTR(-ENOMEM);
		goto out;
	}

	nstack = new_oe->lowerstack;
	for (i = 0, j = 0; i < n; i++) {
		if (ostack[i].layer == layer)
			continue;
		nstack[j] = ostack[i];
		dget(nstack[j].dentry);
		j++;
	}
	if (tail_room) {
		const struct cred *old_cred = override_creds(pfs->creator_cred);
		/* append through the shared merge rule (see aufsng_merge_dirs) */
		struct aufsng_merge m = { .oe = new_oe, .n = j };
		struct name_snapshot ns;

		take_dentry_name_snapshot(&ns, alias);
		err = aufsng_merge_dirs(&m, poe, tail_from, &ns.name, layer);
		j = m.n;
		release_dentry_name_snapshot(&ns);
		revert_creds(old_cred);
	}
	new_oe->numlower = j;
	if (err) {
		aufsng_free_entry(new_oe);
		new_oe = ERR_PTR(err);
	}
out:
	dput(parent);
	dput(alias);
	return new_oe;
}

static void aufsng_dyn_commit_rebuild(struct aufsng_fs *pfs, struct inode *inode,
				  struct aufsng_entry *new_oe,
				  struct aufsng_dyn_parked *parked)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_entry *old_oe;

	/*
	 * No inode_lock() here: pfs->dyn_lock (write, held by caller)
	 * alone excludes every other oe reader/writer in this design
	 * (aufsng_lookup(), readdir's cache build, etc. all serialize on
	 * dyn_lock, not this directory's inode lock).  Taking
	 * inode_lock() too would deadlock: a concurrent lookup holds
	 * the parent's i_rwsem (shared) before calling ->lookup(),
	 * which blocks on percpu_down_read(&pfs->dyn_lock) - ABBA with
	 * inode_lock(inode)+dyn_lock(write) here.
	 *
	 * oi->lock IS taken: copy-up never takes dyn_lock (its data
	 * copy runs lock-free by design), so this swap is the one
	 * branch-change publication a mid-flight copy-up could
	 * interleave with.  Under oi->lock the two serialize: copy-up
	 * re-reads the stack under the same lock before committing and
	 * aborts with -ESTALE if this swap won - otherwise its upper
	 * published first and this is a plain copy-up-then-branch-change
	 * sequence.  Lock order dyn_lock(write) -> oi->lock matches the
	 * documented mnt_want_write -> dyn_lock -> oi->lock ordering.
	 *
	 * @parked must have room for old_oe->numlower mounts (see the
	 * struct aufsng_dyn_parked comment: each parked stack pins
	 * every branch mount its dentries point into).
	 */
	mutex_lock(&oi->lock);
	old_oe = oi->oe;
	/* release-publish for lockless READ_ONCE readers, as in swap_root */
	smp_store_release(&oi->oe, new_oe);
	atomic64_inc(&oi->version);
	aufsng_dyn_rekey_inode(pfs, inode, new_oe);
	/*
	 * The top real object may have changed (a spliced-in branch's
	 * directory, or the surviving top after a removal); the union
	 * inode's attributes - directory permissions above all - must
	 * follow it, exactly as a fresh lookup of the same path would.
	 */
	aufsng_copyattr(inode);

	aufsng_dyn_park_fill(oi, parked, old_oe, NULL, old_oe);
	mutex_unlock(&oi->lock);
}

void aufsng_dyn_put_parked(struct aufsng_inode *oi)
{
	struct aufsng_dyn_parked *p;

	for (p = oi->dyn_parked; p; p = p->next) {
		unsigned int n = p->oe ? p->oe->numlower : 0;
		unsigned int i;

		for (i = 0; i < n; i++)
			dput(p->oe->lowerstack[i].dentry);
		dput(p->upper);
		/* only after every dentry into those branches is gone */
		for (i = 0; i < p->nr_mnts; i++)
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

/*
 * A rekey re-hashes @inode under the key its rebuilt stack computes,
 * and __insert_inode_hash() performs no duplicate check - a second
 * inode under one key would shadow the first for every fresh lookup,
 * splitting one lower hardlink family across two union identities
 * (distinct st_ino for siblings, independent copy-ups of one file).
 * Reachable exactly through hardlinks: two pinned union inodes for
 * sibling names, one keyed on the removed branch's inode, re-pointing
 * onto the surviving branch's inode that already keys the other (which
 * the scan never visits - its stack holds no removed layer).  Refuse
 * the removal instead.  @new_oes[0..@i] are this batch's prepared
 * stacks: two collected inodes may also be re-pointing onto the SAME
 * new key, which the hash (neither is rekeyed yet) cannot show.
 */
static int aufsng_dyn_check_rekey(struct super_block *sb,
			       struct aufsng_dyn_scan *scan,
			       struct aufsng_entry **new_oes, unsigned int i,
			       const char *brname)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);
	struct inode *inode = scan->pinned[i];
	struct inode *key, *dup = NULL;
	unsigned int k;

	key = aufsng_hash_key(pfs, new_oes[i], aufsng_upperdentry(inode),
			      NULL);
	if (!key || key == inode->i_private)
		return 0;

	dup = ilookup5(sb, (unsigned long)key, aufsng_inode_test, key);
	if (dup) {
		iput(dup);
		goto busy;
	}
	for (k = 0; k < i; k++) {
		if (!new_oes[k])
			continue;
		if (aufsng_hash_key(pfs, new_oes[k],
				    aufsng_upperdentry(scan->pinned[k]),
				    NULL) == key)
			goto busy;
	}
	return 0;

busy:
	pr_info("aufs (aufs-ng): cannot remove branch '%s': re-pointing in-use inode %llu would alias another cached inode (hardlink siblings)\n",
		brname, (u64)inode->i_ino);
	return -EBUSY;
}

static void aufsng_dyn_release_branch(struct aufsng_fs *pfs, struct aufsng_layer *layer)
{
	unsigned int idx = aufsng_layer_idx(pfs, layer);

	kfree(pfs->config.br_paths[idx]);
	pfs->config.br_paths[idx] = NULL;
	pfs->config.br_perms[idx][0] = '\0';
	/*
	 * clone_private_mount() clones are "longterm" mounts
	 * (mnt_ns == MNT_NS_INTERNAL): mntput()'s fast path only
	 * decrements the count while mnt_ns is set and never schedules
	 * cleanup, so a plain mntput() here would leak the mount, its
	 * root dentry and the branch superblock's active count forever
	 * (a deactivated squashfs would keep its loop device attached).
	 * kern_unmount() makes the mount shortterm first, exactly as
	 * put_super does for the surviving branches.  Its one-element
	 * _array form is that same teardown behind an EXPEDITED grace
	 * period instead of a plain one, which is what the add path already
	 * uses for its own publication - a module deactivation should not
	 * stall the remount ~10-40ms per branch.
	 */
	kern_unmount_array(&layer->mnt, 1);
	layer->mnt = NULL;
}

int aufsng_dyn_del_branch(struct super_block *sb, const struct path *path)
{
	struct aufsng_fs *pfs = AUFSNG_FS(sb);
	struct inode *root_inode = aufsng_root_inode(sb);
	struct aufsng_entry *new_oe = NULL, *old_root_oe;
	struct aufsng_entry **new_oes = NULL;
	struct aufsng_dyn_parked **parked = NULL;
	struct aufsng_layer *layer;
	struct aufsng_dyn_scan scan = {};
	u64 blocker_ino;
	const char *brname;
	unsigned int i, tries;
	int err;

	layer = aufsng_dyn_find_branch(pfs, path->dentry);
	if (!layer)
		return -ENOENT;
	/*
	 * The branch root dentry is the layer filesystem's own root, so
	 * '%pd' on it prints "/"; the config string is the path the user
	 * knows the branch by.  Freed only in aufsng_dyn_release_branch(),
	 * after the last message below.
	 */
	brname = pfs->config.br_paths[aufsng_layer_idx(pfs, layer)];

	if (AUFSNG_I_E(root_inode)->numlower < 1)
		return -EINVAL;	/* no lower branch to remove */

	for (tries = 0; ; tries++) {
		/*
		 * s_vfs_rename_mutex excludes cross-directory union
		 * renames - including their VFS-level d_move(), which
		 * runs after ->rename() drops dyn_lock and would
		 * re-parent the aliases whose name/parent the re-point
		 * and opaque tail-merge below resolve.  Same ordering as
		 * the add path (taken before the root's i_rwsem, matching
		 * lock_rename()).
		 */
		mutex_lock(&sb->s_vfs_rename_mutex);
		inode_lock(root_inode);
		percpu_down_write(&pfs->dyn_lock);

		/* shrinks under these locks, so a retry also sees the
		 * references its copy-ups released
		 */
		err = aufsng_dyn_scan_branch(sb, layer, &scan);
		if (err)
			goto out_unlock;

		if (scan.nr_busy) {
			pr_info("aufs (aufs-ng): cannot remove branch '%s': %u file(s) in use (memory-mapped, e.g. inode %llu)\n",
				brname, scan.nr_busy, scan.busy_ino);
			err = -EBUSY;
			goto out_unlock;
		}

		/*
		 * A pinned directory only needs a copy-up when the removed
		 * branch is its sole backing: with a survivor, the rebuilt
		 * stack keeps serving it from a surviving branch.
		 */
		blocker_ino = 0;
		for (i = 0; i < scan.nr; i++) {
			if (S_ISDIR(scan.pinned[i]->i_mode) &&
			    !aufsng_dyn_dir_gone(scan.pinned[i]) &&
			    !aufsng_dyn_has_survivor(scan.pinned[i], layer)) {
				blocker_ino = scan.pinned[i]->i_ino;
				break;
			}
		}
		if (i == scan.nr)
			break;

		percpu_up_write(&pfs->dyn_lock);
		inode_unlock(root_inode);
		mutex_unlock(&sb->s_vfs_rename_mutex);

		err = -EBUSY;
		if (tries >= 4) {
			pr_info("aufs (aufs-ng): cannot remove branch '%s': in-use directory inode %llu has no other provider and its copy-up made no progress\n",
				brname, blocker_ino);
			goto out_scan;
		}
		err = aufsng_dyn_copy_up_dirs(&scan, layer);
		if (err)
			goto out_scan;
		for (i = 0; i < scan.nr; i++)
			iput(scan.pinned[i]);
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
		bool pin_only = false;

		if (S_ISDIR(scan.pinned[i]->i_mode)) {
			/*
			 * A deleted-but-open directory with no survivor has
			 * nothing to rebuild onto: keep its stack and pin the
			 * mounts, as the non-directory path does for the same
			 * state.  (With a survivor the filter below still
			 * produces a valid stack, so let it.)
			 */
			if (aufsng_dyn_dir_gone(scan.pinned[i]) &&
			    !aufsng_dyn_has_survivor(scan.pinned[i], layer))
				pin_only = true;
			else
				new_oes[i] = aufsng_dyn_prep_rebuild(pfs,
								  scan.pinned[i],
								  layer, true);
		} else
			new_oes[i] = aufsng_dyn_prep_repoint(pfs,
							  scan.pinned[i],
							  layer, &pin_only);
		if (IS_ERR(new_oes[i])) {
			err = PTR_ERR(new_oes[i]);
			new_oes[i] = NULL;
			goto out_unlock;
		}
		if (!new_oes[i] && !pin_only)
			continue;
		if (new_oes[i]) {
			err = aufsng_dyn_check_rekey(sb, &scan, new_oes, i,
						  brname);
			if (err)
				goto out_unlock;
		}
		/*
		 * Sized to pin every mount the superseded stack - or, for
		 * a pin-only skip (deleted-but-open object), the KEPT
		 * stack - references.
		 */
		cur = AUFSNG_I_E(scan.pinned[i]);
		parked[i] = kmalloc(struct_size(parked[i], mnts,
						cur ? cur->numlower : 0),
				    GFP_KERNEL);
		if (!parked[i]) {
			err = -ENOMEM;
			goto out_unlock;
		}
	}

	/*
	 * The root stack is rebuilt through the same filter helper as
	 * every cached directory's (it also sizes the copy exactly, by
	 * counted hits).  NULL means the layer was not in the root
	 * stack at all - impossible for an active branch, which the add
	 * path put there exactly once - so it is treated as corruption
	 * rather than silently skipping the swap.
	 */
	new_oe = aufsng_dyn_prep_rebuild(pfs, root_inode, layer, false);
	if (IS_ERR(new_oe)) {
		err = PTR_ERR(new_oe);
		new_oe = NULL;
		goto out_unlock;
	}
	if (WARN_ON(!new_oe)) {
		err = -EIO;
		goto out_unlock;
	}
	err = 0;

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
			aufsng_dyn_commit_rebuild(pfs, scan.pinned[i], new_oes[i],
					      parked[i]);
			/*
			 * Removing a branch that carried a whiteout reveals the
			 * name it was hiding in a lower-priority branch, so drop
			 * cached negative children whose "absent" verdict the
			 * removal may have overturned - the same refresh the add
			 * path does after splicing.  Regular files have no
			 * children to refresh.
			 */
			if (S_ISDIR(scan.pinned[i]->i_mode))
				aufsng_dyn_drop_neg_children(scan.pinned[i]);
		} else if (parked[i]) {
			/*
			 * Deleted-but-open: nothing to re-point and nothing
			 * to re-point TO - keep the stack and pin its mounts
			 * until the inode is evicted, so the open fds keep
			 * working across the branch's kern_unmount().
			 */
			aufsng_dyn_pin_stack(AUFSNG_I(scan.pinned[i]),
					  AUFSNG_I_E(scan.pinned[i]),
					  parked[i]);
		}
		iput(scan.pinned[i]);
	}
	kfree(scan.pinned);
	kfree(new_oes);
	kfree(parked);

	/* the busy-scan skips the root; refresh its negatives too */
	aufsng_dyn_drop_neg_children(root_inode);
	/* one re-check of every cached lower-only dentry (dcache.c) */
	atomic_long_inc(&pfs->branch_gen);

	pr_info("aufs (aufs-ng): branch '%s' removed\n", brname);

	percpu_up_write(&pfs->dyn_lock);
	inode_unlock(root_inode);
	mutex_unlock(&sb->s_vfs_rename_mutex);

	/* expedited for the same reason as on the add path (see there) */
	synchronize_rcu_expedited();
	aufsng_free_entry(old_root_oe);
	/*
	 * Only after the grace period: an RCU show_options snapshot of
	 * the superseded root stack may still be printing this branch's
	 * config strings.  Slot reuse by a later add is safe for the
	 * same reason - it cannot begin before this removal (and its
	 * grace period) completes.
	 */
	aufsng_dyn_release_branch(pfs, layer);

	return 0;

out_unlock:
	percpu_up_write(&pfs->dyn_lock);
	inode_unlock(root_inode);
	mutex_unlock(&sb->s_vfs_rename_mutex);
out_scan:
	for (i = 0; i < scan.nr; i++)
		iput(scan.pinned[i]);
	kfree(scan.pinned);
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
	size_t i;
	int err = 0;

	/*
	 * udba= may be changed on remount, as on real AUFS.  Only an
	 * explicitly given value is applied: legacy remounts replay the
	 * current options, and a remount without udba= must not reset
	 * the mount-time choice to the parser default.
	 */
	if (ctx->udba_set)
		AUFSNG_FS(sb)->config.udba = ctx->config.udba;

	for (i = 0; !err && i < ctx->nr_dyn_add; i++) {
		struct aufsng_ctx_branch *b = &ctx->dyn_add[i];

		/*
		 * An added branch is always a lower, so it is cloned read-only
		 * whatever the mode says (see aufsng_dyn_add_branch()).  Record
		 * the mode it will actually have, or /proc/mounts would echo
		 * back a "=rw" this branch never gets - the same effective-mode
		 * rule aufsng_fill_super() applies to a lower "=rw".
		 */
		if (b->perm == AUFSNG_BR_RW) {
			pr_warn("aufs (aufs-ng): branch '%s' declared rw but only the first branch is writable; using ro\n",
				b->name);
			strscpy(b->permstr, "ro", AUFSNG_PERM_LEN);
		}
		err = aufsng_dyn_add_branch(sb, b->name, &b->path, b->permstr);
		/*
		 * Tolerate re-adding a present branch: legacy remounts
		 * may replay the current mount options.
		 */
		if (err == -EEXIST)
			err = 0;
	}

	for (i = 0; !err && i < ctx->nr_dyn_del; i++)
		err = aufsng_dyn_del_branch(sb, &ctx->dyn_del[i]);

	return err;
}
