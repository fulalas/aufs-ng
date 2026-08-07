// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime branch add/remove ("add=N:PATH=MODE" / "del=PATH" on
 * remount), matching AUFS semantics.
 *
 *  - Branches are never moved or freed before umount (entries point at
 *    them); only the slot table grows, freed slots are reused first.
 *
 *  - A new branch goes to the front of the root's lowerstack, so the
 *    newest wins.  Only N=1 is supported: the splice below assumes the
 *    added branch is the new TOP lower.
 *
 *  - Add and remove update every cached directory in place under
 *    dyn_lock(write), so no dentry is ever invalidated: pinned
 *    dentries and nested mounts survive, where a d_invalidate refresh
 *    would detach the mounts and strand fd-based re-resolution.
 *
 *  - A cached directory inode is re-keyed whenever its top lower
 *    changes, so a fresh lookup finds it instead of an alias.
 *
 *  - Superseded stacks are parked on the inode until eviction, not
 *    freed after one synchronize_rcu(): an in-flight getattr may still
 *    be dereferencing them.
 *
 *  - The removal scan uses one sb-wide evict_inodes() pass, not a
 *    per-inode evict-and-restart loop quadratic in cached inodes.
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
 * Swap the root dentry's entry, under the root inode lock and
 * dyn_lock(write).  The returned old entry is freed by the caller once
 * every possible reader is done.
 */
static struct aufsng_entry *aufsng_dyn_swap_root(struct super_block *sb,
					   struct aufsng_entry *new_oe)
{
	struct inode *inode = aufsng_root_inode(sb);
	struct aufsng_entry *old_oe = AUFSNG_I_E(inode);

	/* Release-publish: lockless readers load the pointer with READ_ONCE */
	smp_store_release(&AUFSNG_I(inode)->oe, new_oe);
	/* force the merged readdir cache of the root to be rebuilt */
	atomic64_inc(&AUFSNG_I(inode)->version);

	return old_oe;
}

static void aufsng_dyn_drop_neg_children(struct inode *inode);

/*
 * Fill @pk and link it onto @oi's parked list: @oe is the stack the
 * node OWNS (NULL for a pin-only node), @upper the superseded upper it
 * owns, @mnt_src the stack whose mounts it must PIN - they have to
 * outlive the branch's kern_unmount().
 *
 * Owned and pinned are separate on purpose: a pin-only node leaves the
 * LIVE stack in place, so passing it as @oe would have eviction free a
 * stack still in use.  @pk has room for @mnt_src's lowers.  Caller
 * holds oi->lock.
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
 * Park the current upper - a lockless reader may hold a pointer until
 * eviction - and publish @new_upper (NULL sheds it so the top lower
 * resurfaces), refreshing the mirrored attributes and the readdir
 * version.  The node pins no mounts: a bare upper points into branch
 * 0, which outlives every inode.  False on allocation failure, with
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
 * An object can be looked up again while an older union inode for it
 * is still pinned: a branch change reorders priorities, so a fresh
 * lookup's key may differ from the cached inode's.  Heal by adopting
 * the new upper instead of returning ESTALE.
 *
 * An rw copy can also be replaced at the same path, so its upper is a
 * different inode: adopt that, park the superseded upper for lockless
 * readers, bump the version to rebuild a merged listing, and drop
 * cached negative children.  This one state machine serves the branch
 * change, lookup's ESTALE heal and udba=reval revalidation alike.
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
	/* A different file type is never adopted: the ops are fixed at the type */
	if (!aufsng_origin_type_ok(upperdentry, inode->i_mode))
		return false;
	if (is_dir && (!lowerdentry || !d_is_dir(lowerdentry)))
		return false;

	mutex_lock(&oi->lock);
	oe = oi->oe;
	/*
	 * Directories must match on the top lower, or an unrelated one
	 * could be grafted on; non-directories are already identified by
	 * the hash key.
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
 * Shed a DEAD upper - one an out-of-band unlink/rename killed - from a
 * lower-backed inode, so the lower resurfaces as udba=reval promises.
 * Keeping it would serve the deleted content forever and route write
 * opens into the unlinked inode.  Parked, not dropped: a lockless
 * reader may hold a pointer until eviction.  False if the upper was
 * alive after all, or on allocation failure.
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
	 * Losing the upper resurfaces names its whiteouts hid.  Cached
	 * negatives were primed against the has-upper state and the
	 * !pupper shortcut would keep them valid forever; drop them.
	 */
	if (shed && S_ISDIR(inode->i_mode))
		aufsng_dyn_drop_neg_children(inode);
	return ok;
}

/*
 * Re-key a cached inode when its top real object changes, recomputing
 * i_ino as a fresh lookup would: the number folds in the providing
 * slot, so a re-pointed object must not keep the REMOVED branch's.
 * readdir derives d_ino live, and a later add reusing that slot could
 * mint an identical (st_dev, st_ino) for a different file.
 */
static void aufsng_dyn_rekey_inode(struct inode *inode, struct aufsng_entry *oe)
{
	unsigned int key_idx;
	struct inode *key = aufsng_hash_key(oe, aufsng_upperdentry(inode),
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
		struct aufsng_layer *l = pfs->layers[i];

		if (l->mnt && l->mnt->mnt_root == dentry)
			return l;
	}
	return NULL;
}

static unsigned int aufsng_find_free_slot(struct aufsng_fs *pfs)
{
	unsigned int i;

	for (i = 1; i < pfs->numlayer; i++) {
		if (!pfs->layers[i]->mnt)
			return i;
	}
	return pfs->numlayer;
}

/* defined later; used by the surgical-add path below */
static bool aufsng_entry_has_layer(struct aufsng_entry *oe,
				const struct aufsng_layer *layer);
static void aufsng_dyn_commit_rebuild(struct inode *inode,
				   struct aufsng_entry *new_oe,
				   struct aufsng_dyn_parked *parked);

#define AUFSNG_RESOLVE_MAXDEPTH 64
#define AUFSNG_MEMO_BITS 9

/*
 * Per-pass memo of "union directory -> its counterpart in the branch
 * being added" (NULL = blocked or absent).  Cached directories share
 * ancestor chains, so memoizing makes the pass O(dirs) lookups instead
 * of O(dirs * depth) - and it runs under dyn_lock(write), with every
 * lookup and readdir blocked behind it.
 */
struct aufsng_memo_ent {
	struct hlist_node node;
	struct dentry *uniond;	/* key; ref held for the memo's lifetime */
	struct dentry *branchd;	/* counterpart, ref held; NULL = absent */
	/*
	 * The new branch HIDES this object - stronger than absent: the
	 * union name is gone, so every cached directory at or below it
	 * must be unhashed, not kept on its pre-add stack.
	 */
	bool blocked;
};

/*
 * The upper diropq verdict is per PARENT: without its own memo, k
 * siblings under one missed parent each re-probe the same
 * ".wh..wh..opq", inside the stall dyn_lock(write) imposes.
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
 * Advance one path component: resolve the union child @d in the new
 * branch under the resolved branch parent @pbase, by the same
 * per-level rules a fresh lookup applies.  The branch child (ref held)
 * or NULL when the path is not provided - also on transient errors,
 * where skipping just leaves the pre-add view until eviction.
 *
 * @blocked is set when the new branch HIDES the object at this level:
 * a whiteout, or a same-named non-directory, with no upper to win.  A
 * fresh lookup would not find the cached directory at all, so the
 * caller must unhash it.
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
	 * Upper masking, as lookup enforces it level by level: an opaque
	 * upper ancestor hides every lower at and below it - the added
	 * branch included, since only the upper can mask a top lower -
	 * and an upper whiteout kills the path the same way.  Without
	 * this a pinned directory shows content no fresh lookup merges.
	 */
	if (pupper && aufsng_memo_diropq(pfs, memo, pupper))
		return NULL;

	/*
	 * The name is read via a snapshot.  Cross-directory renames - the
	 * ones that would tear the ancestor chain this walk replays - are
	 * excluded by s_vfs_rename_mutex, which spans ->rename() and the
	 * union-level d_move() (dyn_lock would not: rename drops it
	 * first).  Same-directory renames take no such mutex and can
	 * still swap a d_name mid-walk; the snapshot makes that safe,
	 * and at worst the pre-add view survives until eviction.
	 */
	take_dentry_name_snapshot(&ns, d);
	if (pupper &&
	    aufsng_check_whiteout(aufsng_upper_mnt(pfs), pupper, &ns.name))
		goto out;
	/*
	 * The new branch's whiteout hides the name in every branch below,
	 * so with no upper the object is gone from the union.  A probe
	 * error skips instead, keeping the pre-add view.
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
		 * A same-named non-directory ends the merge at itself: a
		 * lower-only directory is shadowed, as if deleted.
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
 * Resolve the directory in the just-added branch matching a cached
 * union directory, by replaying its ancestor chain from the branch
 * root through the memo.  A positive dentry (ref held), or NULL if the
 * branch does not provide the path as a visible directory.  *@blocked
 * means the new branch hides it or an ancestor: the path is gone.
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
 * Build the new stack for a cached directory after an add: @nd becomes
 * the top lower.  The new entry, NULL to skip, or ERR_PTR.  Caller
 * holds dyn_lock(write).
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
 * Drop cached negative children so names the new branch provides are
 * re-resolved.  Negatives are never mountpoints, so no mount is hit.
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
 * Splice @layer into every cached union directory in place, so pinned
 * directories show the new content at once - without an invalidation
 * that would detach nested mounts.  Under the root inode lock and
 * dyn_lock(write).
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
			 * The new branch hides this directory: the name is
			 * gone from the union, as if deleted.  Unhash the
			 * alias so fresh lookups re-resolve; pinned users
			 * keep the old view until they let go.
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
		aufsng_dyn_commit_rebuild(dirs[i], neu, pk);
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
		       const struct path *path)
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

	err = aufsng_check_layer(sb, path, name);
	if (err)
		return err;
	if (aufsng_dyn_find_branch(pfs, path->dentry))
		return -EEXIST;	/* legacy remount replay tolerates this */
	err = aufsng_check_overlap(pfs, path->dentry, name);
	if (err)
		return err;
	/*
	 * The advertised name limit must fit the SHALLOWEST branch.  Only
	 * PROBED here; the clamp lands under the locks below, once
	 * nothing can fail: it is never widened back, so applying it
	 * before a failure would under-report f_namelen forever for a
	 * branch that was never added.
	 */
	err = aufsng_probe_namelen(path, &namelen);
	if (err)
		return err;

	/*
	 * Grows the slot table if this add needs a new slot.  Before the
	 * point of no return, and idempotent: a slot left by a failed add
	 * is reused by the next one.
	 */
	layer = aufsng_layer_reserve(pfs, idx);
	if (IS_ERR(layer))
		return PTR_ERR(layer);

	dup_name = kstrdup(name, GFP_KERNEL);
	if (!dup_name)
		return -ENOMEM;

	mnt = clone_private_mount(path);
	if (IS_ERR(mnt)) {
		err = PTR_ERR(mnt);
		goto out_name;
	}
	/* Only branch 0 is ever written, so even a "=rw" add clones read-only */
	mnt->mnt_flags |= MNT_READONLY | MNT_NOATIME;

	cur_oe = AUFSNG_I_E(root_inode);

	/*
	 * The new branch becomes the top lower.  @mnt is passed
	 * explicitly: layer->mnt is only published below, under the locks.
	 */
	new_oe = aufsng_entry_prepend(cur_oe, cur_oe->numlower, layer,
				  path->dentry, mnt);
	if (!new_oe) {
		err = -ENOMEM;
		goto out_mnt;
	}

	/*
	 * s_vfs_rename_mutex excludes cross-directory renames, d_move()
	 * included, which would re-parent the ancestor chains the splice
	 * replays.  Before the root's i_rwsem, as lock_rename() orders it.
	 */
	mutex_lock(&sb->s_vfs_rename_mutex);
	inode_lock(root_inode);
	percpu_down_write(&pfs->dyn_lock);

	layer->mnt = mnt;
	pfs->namelen = min(pfs->namelen, namelen);
	layer->path = dup_name;
	if (idx == pfs->numlayer)
		pfs->numlayer++;

	/*
	 * The root swap plus the in-place splice refresh the whole tree
	 * without invalidating a dentry, so nested mounts survive.
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
	 * against its RCU readers (getattr's dir-nlink walk).  Expedited:
	 * a plain synchronize_rcu() stalls every add ~10-40ms for a stack
	 * nothing hot depends on - precedent in kern_unmount_array().
	 * (Parking the root entry instead would mntget every pre-existing
	 * lower until umount, keeping removed branches attached - worse.)
	 */
	synchronize_rcu_expedited();
	aufsng_free_entry(old_oe);

	pr_info("aufs (aufs-ng): branch '%s' added\n", name);
	return 0;

out_mnt:
	/* Private clones are longterm mounts: only kern_unmount() tears them down */
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
		 * Match the mount too: a deleted-but-open inode keeps a
		 * stack on a REMOVED branch, and a later add may reuse the
		 * slot - the stale entry must not claim the new branch.
		 */
		if (oe->lowerstack[i].layer == layer &&
		    oe->lowerstack[i].mnt == layer->mnt)
			return true;
	}
	return false;
}

/*
 * A directory removed through the union but still pinned by a cwd or
 * fd: rmdir cleared its link count, so no lookup can reach it again.
 * Like a deleted-but-open file it must not block a branch removal -
 * there is nothing to re-point to - so it takes the same pin-only
 * path: the stack stays, mounts pinned, until the last user lets go.
 */
static bool aufsng_dyn_dir_gone(struct inode *inode)
{
	return S_ISDIR(inode->i_mode) && !inode->i_nlink;
}

/*
 * Does anything other than @layer still back @inode - a live upper, or
 * a lower from another branch?  A dead upper is not a survivor:
 * counting it would strip the stack the shed heal needs and serve the
 * deleted upper forever.
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
 * Is @inode's object on @layer memory-mapped?  Checked on the BACKING
 * inode: mmap goes through backing_file_mmap, which links the vma into
 * the real address_space.  Non-sleeping; called under i_lock.
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
 * Such a stack records only the topmost provider, so the survivor
 * cannot be read off it: re-resolve the name against the surviving
 * branches exactly as the first fresh lookup will, so the rekeyed
 * inode keeps the identity that lookup computes.
 *
 * The name is read through a snapshot and the parent through its own
 * reference: dyn_lock keeps neither stable, since a same-directory
 * rename takes no rename mutex and can swap d_name across the sleeping
 * lookups here.  Cross-directory ones are excluded by the caller's
 * s_vfs_rename_mutex, as on the add path.
 *
 * With no survivor for any name and no live upper, the object is
 * already invisible to fresh lookups: *@pin_only is set and NULL
 * returned, so the caller keeps the stack and pins its mounts rather
 * than failing the removal with EBUSY.  (Which of several hardlink
 * aliases is re-resolved is the arbitrary d_find_alias() pick.)  The
 * rebuilt entry or ERR_PTR; caller holds the root inode lock,
 * s_vfs_rename_mutex and dyn_lock(write).
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
		 * DEAD upper is passed as none: it has no inode to read a
		 * marker from, and a lower is about to serve the object.
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
 * Pin every mount @oe references WITHOUT swapping the stack: a
 * deleted-but-open object whose branch is being removed keeps working
 * through its fds, but the stack's mounts must outlive the branch's
 * kern_unmount().  @pk has room for oe->numlower.  Under dyn_lock(write).
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
 * Drop every cache-only reference to @layer and classify the rest: a
 * memory-mapped non-directory makes the branch busy, since a live
 * mapping cannot have its backing pulled; every other in-use inode is
 * collected for a stack rebuild.  Under the root inode lock and
 * dyn_lock(write), so no new reference can appear meanwhile.
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
	 * Shrink INSIDE the removal locks, every pass: i_count is all
	 * that separates "in use" from "merely cached", so a lookup
	 * landing between an unlocked shrink and this walk makes a
	 * cache-only inode look pinned - and a sole-backed directory
	 * misclassified that way gets physically copied up.
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
		 * Cache-only: mark I_DONTCACHE and take a reference so the
		 * deferred iput evicts it after the walk.  An in-place
		 * iput would drop the list lock and restart, making the
		 * pass O(n * evicted) under dyn_lock(write).
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
 * Copy up the collected directories that would lose their only backing
 * with @layer; survivor-backed ones need nothing.
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
 * Filter @layer out of a cached directory's stack.  If the removed
 * entry was the BOTTOM and carried an opaque marker, the merge that
 * built this stack stopped there and never probed the branches below,
 * which a fresh lookup will now merge - the marker leaves with its
 * branch.  Re-run that tail here by lookup's own rules, or the pinned
 * view stays permanently missing what the opaque branch hid.
 *
 * @tail_merge is false for the ROOT, whose stack is the branch list
 * itself.  Caller holds the root inode lock, s_vfs_rename_mutex and
 * dyn_lock(write).
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

static void aufsng_dyn_commit_rebuild(struct inode *inode,
				  struct aufsng_entry *new_oe,
				  struct aufsng_dyn_parked *parked)
{
	struct aufsng_inode *oi = AUFSNG_I(inode);
	struct aufsng_entry *old_oe;

	/*
	 * No inode_lock(): dyn_lock(write) alone excludes every other oe
	 * reader and writer here, and taking it too would be ABBA - a
	 * lookup holds the parent's i_rwsem before blocking on
	 * dyn_lock(read).
	 *
	 * oi->lock IS taken: copy-up never takes dyn_lock, so this swap
	 * is the one publication a mid-flight copy-up can interleave
	 * with.  Under oi->lock the two serialize - copy-up re-reads the
	 * stack there and aborts with -ESTALE if this swap won.  Order
	 * dyn_lock(write) -> oi->lock matches the documented one.
	 *
	 * @parked has room for old_oe->numlower mounts.
	 */
	mutex_lock(&oi->lock);
	old_oe = oi->oe;
	/* release-publish for lockless READ_ONCE readers, as in swap_root */
	smp_store_release(&oi->oe, new_oe);
	atomic64_inc(&oi->version);
	aufsng_dyn_rekey_inode(inode, new_oe);
	/*
	 * The top real object may have changed, so the union inode's
	 * attributes must follow it, as a fresh lookup would.
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
 * A rekey re-hashes @inode under its rebuilt stack's key, and
 * __insert_inode_hash() checks no duplicates - a second inode under
 * one key shadows the first for every lookup, splitting one hardlink
 * family into two identities.  Reachable through hardlinks: two pinned
 * inodes for sibling names, one re-pointing onto the key the other
 * already holds.  Refuse the removal instead.  @new_oes[0..@i] are
 * this batch's stacks: two of them may also target the SAME new key,
 * which the hash cannot show while neither is rekeyed.
 */
static int aufsng_dyn_check_rekey(struct super_block *sb,
			       struct aufsng_dyn_scan *scan,
			       struct aufsng_entry **new_oes, unsigned int i,
			       const char *brname)
{
	struct inode *inode = scan->pinned[i];
	struct inode *key, *dup = NULL;
	unsigned int k;

	key = aufsng_hash_key(new_oes[i], aufsng_upperdentry(inode), NULL);
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
		if (aufsng_hash_key(new_oes[k],
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

static void aufsng_dyn_release_branch(struct aufsng_layer *layer)
{
	kfree(layer->path);
	layer->path = NULL;
	/*
	 * Private clones are longterm mounts: mntput()'s fast path never
	 * schedules cleanup, so it would leak the mount, its root dentry
	 * and the branch sb's active count forever, backing device
	 * included.  kern_unmount() makes it shortterm first; the
	 * one-element _array form is the same teardown behind an
	 * expedited grace period, so a removal does not stall ~10-40ms.
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
	 * '%pd' on the branch root prints "/", so use the stored path -
	 * the name the user knows.  Freed only in release_branch(), after
	 * the last message below.
	 */
	brname = layer->path;

	if (AUFSNG_I_E(root_inode)->numlower < 1)
		return -EINVAL;	/* no lower branch to remove */

	for (tries = 0; ; tries++) {
		/*
		 * s_vfs_rename_mutex excludes cross-directory renames,
		 * whose d_move() would re-parent the aliases the re-point
		 * and tail merge resolve.  Same ordering as the add path.
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
		 * A pinned directory needs a copy-up only when the removed
		 * branch is its sole backing.
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
			 * nothing to rebuild onto: keep the stack and pin
			 * the mounts, as the non-directory path does.
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
		/* Sized for every mount the superseded - or KEPT - stack uses */
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
	 * The root stack goes through the same filter as every cached
	 * directory's.  NULL means the layer was not in it at all -
	 * impossible for an active branch, so treat it as corruption.
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
	 * Removal invalidates no dentries: the shrink, evict and in-place
	 * rebuild keep every survivor consistent, and invalidating would
	 * strand pinned dentries no fresh lookup can replace.
	 */
	old_root_oe = aufsng_dyn_swap_root(sb, new_oe);

	/*
	 * Swap the prepared stacks in before releasing dyn_lock: while it
	 * is held for writing, no lookup can resolve a child through a
	 * stack that still references the removed branch.
	 */
	for (i = 0; i < scan.nr; i++) {
		if (new_oes[i]) {
			aufsng_dyn_commit_rebuild(scan.pinned[i], new_oes[i],
					      parked[i]);
			/*
			 * A removed branch's whiteout stops hiding the name
			 * below it, so drop cached negatives whose "absent"
			 * verdict the removal overturned.
			 */
			if (S_ISDIR(scan.pinned[i]->i_mode))
				aufsng_dyn_drop_neg_children(scan.pinned[i]);
		} else if (parked[i]) {
			/*
			 * Deleted-but-open: nothing to re-point to.  Keep the
			 * stack and pin its mounts until eviction, so open
			 * fds survive the branch's kern_unmount().
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
	 * Only after the grace period: an RCU reader of the superseded
	 * root stack may still be dereferencing this branch's dentries
	 * and mount.  Slot reuse by a later add is safe for the same
	 * reason - it cannot begin before this removal completes.
	 */
	aufsng_dyn_release_branch(layer);

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
 * Called with sb->s_umount held for writing: applies the add= and del=
 * changes collected in @ctx.
 */
int aufsng_dyn_reconfigure(struct fs_context *fc)
{
	struct super_block *sb = fc->root->d_sb;
	struct aufsng_fs_context *ctx = fc->fs_private;
	size_t i;
	int err = 0;

	/*
	 * udba= may change on remount, as on AUFS.  Only an explicit
	 * value applies: a remount without one must not reset the
	 * mount-time choice to the parser default.
	 */
	if (ctx->udba_set)
		AUFSNG_FS(sb)->config.udba = ctx->config.udba;

	for (i = 0; !err && i < ctx->nr_dyn_add; i++) {
		struct aufsng_ctx_branch *b = &ctx->dyn_add[i];

		/*
		 * An added branch is always a lower, so read-only whatever the
		 * mode says.  Warn: no branch mode is echoed to userspace.
		 */
		if (b->perm == AUFSNG_BR_RW)
			pr_warn("aufs (aufs-ng): branch '%s' declared rw but only the first branch is writable; using ro\n",
				b->name);
		err = aufsng_dyn_add_branch(sb, b->name, &b->path);
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
