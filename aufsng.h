/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * aufs-ng - standalone union filesystem for live Linux distributions,
 * semantically compatible with AUFS: registers as filesystem type
 * "aufs", accepts
 * genuine AUFS mount option syntax (br:, add=N:PATH=MODE, del=PATH,
 * xino=, udba=, dirperm1, nowarn_perm), and uses AUFS's own on-disk
 * whiteout format (".wh.<name>" regular files, ".wh..wh..opq" opaque
 * directory markers) so that scripts which mount, remount, or scan a
 * branch directly (as live-boot core-scripts do) need no changes.
 *
 * Branch 0 is always the single writable branch; branches 1.. are
 * read-only, highest priority first ("add=1:" always inserts
 * immediately below branch 0, so the most recently added module has
 * highest priority - the same "last-added-wins" semantics as AUFS).
 */
#ifndef AUFSNG_H
#define AUFSNG_H

#include <linux/fs.h>
#include <linux/fs_parser.h>
#include <linux/mount.h>
#include <linux/dcache.h>
#include <linux/cred.h>
#include <linux/percpu-rwsem.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/limits.h>
#include <linux/backing-file.h>
#include "compat.h"

#define AUFSNG_NAME		"aufs"
#define AUFSNG_SUPER_MAGIC	0x61756673	/* "aufs" */
#define AUFSNG_ROOT_INO		2		/* AUFS_ROOT_INO */

#define AUFSNG_MAX_STACK		500
/*
 * Ceiling on the total branch count, the same one real AUFS's largest
 * build-time setting allows (AUFS_BRANCH_MAX_32767).  It is not a
 * preallocation size - the slot table grows on demand - only the point
 * past which a branch slot would no longer fit the inode-number folding
 * in aufsng_map_ino(): a slot is shifted to bit 40 and up, and must
 * stay clear of AUFSNG_ROOT_INO_EVADE at bit 62.
 */
#define AUFSNG_MAXBRANCH	32767

/* AUFS on-disk whiteout format (verified against aufs-standalone) */
#define AUFSNG_WH_PFX		".wh."
#define AUFSNG_WH_PFX_LEN		4
#define AUFSNG_WH_DIROPQ		".wh..wh..opq"
#define AUFSNG_WH_MODE		0444

/*
 * The one piece of bookkeeping AUFS itself has no on-disk equivalent
 * for (it keeps the same knowledge in its xino tables instead): an
 * xattr set on an upper non-directory by copy-up, holding the NAME the
 * object was copied up under - "the lower called this is what I was
 * copied from".
 *
 * A union inode is keyed by that lower so its st_ino survives copy-up
 * and cache eviction, and only a real copy-up may claim the link.  A
 * file that merely happens to share a name with a lower - created over
 * a whiteout, or renamed onto the name - is an independent object and
 * gets its own identity, the way it would on any real filesystem.
 * Guessing the link from the name instead handed such a file the
 * deleted one's inode, including the write block exec puts on a
 * running binary (ETXTBSY on the first write to its replacement).
 *
 * The name is what makes the claim per-NAME on a per-inode xattr, and
 * nothing else would: link(2) gives a second name to the very inode
 * copy-up marked, and rename gives that inode a name it was never
 * copied up under.  Both then stop matching, with no cleanup pass to
 * get wrong - and a rename of one hardlink cannot silently revoke its
 * siblings' claim either.
 *
 * "trusted." keeps it out of reach of unprivileged users, and the
 * handlers in inode.c hide it from the union so it never leaks into a
 * listing or a cp -a.
 */
#define AUFSNG_XATTR_PFX	"trusted.aufs_ng."
#define AUFSNG_XATTR_ORIGIN	AUFSNG_XATTR_PFX "origin"

enum aufsng_br_perm {
	AUFSNG_BR_RW,
	AUFSNG_BR_RO,	/* "ro"/"rr": read-only, never written to */
};

/*
 * Sized to AUFS's own maximum mode-token length (AuBrPermStrSz covers
 * "rw+coo_reg+fhsm+unpin+icexsec+icexsys+icexusr+icexoth+nolwh"):
 * aufsng_parse_perm() accepts any '+'-suffix chain a real aufs command
 * may carry, so the stored token - echoed back verbatim through
 * /proc/mounts - must never be silently truncated into a malformed one.
 */
#define AUFSNG_PERM_LEN	64

/*
 * One branch.  Allocated individually and never moved or freed before
 * umount, because aufsng_entry stacks reference it by pointer: the slot
 * TABLE (aufsng_fs.layers) grows as branches are added, the branches
 * themselves stay put.  @path and @perm live here rather than in
 * slot-indexed side arrays so a reader that already holds a layer
 * pointer needs no table lookup at all (aufsng_show_options()).
 */
struct aufsng_layer {
	struct vfsmount *mnt;	/* private clone; NULL marks a free slot */
	unsigned int idx;	/* stable slot number, see aufsng_layer_idx() */
	/*
	 * The branch path as given and the mode as it actually applies,
	 * only consumed by show_options: the private clone mounts have no
	 * namespace path to resolve at print time, and the mode is echoed
	 * back exactly as given ("rr" stays "rr"), so both are kept
	 * verbatim.  @path is NULL on a free slot.
	 */
	char *path;
	char perm[AUFSNG_PERM_LEN];
};

struct aufsng_path {
	struct aufsng_layer *layer;
	struct dentry *dentry;
	/*
	 * The branch mount, captured when the entry was built.  Lockless
	 * readers (aufsng_path_real()) must reach the mount through this
	 * copy, never through layer->mnt: a branch removal blanks
	 * layer->mnt (and a later add may reuse the slot for a different
	 * branch) while a superseded-but-parked stack is still being
	 * dereferenced.  The parked stack pins the vfsmount object
	 * (aufsng_dyn_parked.mnts), and this pointer stays valid with it
	 * until the inode is evicted.
	 */
	struct vfsmount *mnt;
};

/*
 * Lower (read-only) branch stack of a dentry/inode.  Priority is the
 * order of this array, top first.  Swapped under RCU by dynamic
 * branch changes; readers hold rcu_read_lock() or aufsng_fs.dyn_lock.
 */
struct aufsng_entry {
	unsigned int numlower;
	struct aufsng_path lowerstack[];
};

struct aufsng_config {
	char *xino_path;	/* accepted, not functionally used */
	unsigned int udba;	/* AUFSNG_UDBA_*; see aufsng_udba_reval() */
};

enum aufsng_udba { AUFSNG_UDBA_NONE, AUFSNG_UDBA_REVAL, AUFSNG_UDBA_NOTIFY };

struct aufsng_fs {
	unsigned int numlayer;		/* used slots incl. [0] = branch 0 */
	unsigned int numlayer_cap;	/* slots the table can hold; grows */
	/*
	 * Branch slot table: [0] the rw branch, [1..] the ro branches.  A
	 * table of POINTERS, so adding the 129th branch can grow it (real
	 * live systems load hundreds of modules, each one a branch) without
	 * moving any struct aufsng_layer - those are referenced by pointer
	 * from every cached aufsng_entry and must never be relocated.
	 *
	 * Only ever read and written by the mount, umount and branch
	 * add/remove paths, all of which hold sb->s_umount exclusively, so
	 * the table itself needs no RCU: nothing on a lookup, readdir or
	 * stat path touches it (branch 0 is reached through @upper, and a
	 * slot number through layer->idx).
	 */
	struct aufsng_layer **layers;
	struct aufsng_layer *upper;	/* == layers[0], the only rw branch */
	const struct cred *creator_cred;
	/*
	 * Bumped once per runtime branch add/remove (AUFS's "sigen"):
	 * compared against each dentry's d_time stamp so positive
	 * revalidation re-runs the merge decision exactly once per
	 * branch change (dcache.c), never on ordinary mutations.  The
	 * stamp is per-DENTRY, not per-inode: lower hardlink siblings
	 * share one union inode, but the winning-branch decision is
	 * per-name, so each name must re-check for itself.  Kept at
	 * unsigned-long width (the d_time stamp it is compared against,
	 * see aufsng_store_reval_stamps()) so no bits are silently
	 * dropped on 32-bit; a false match needs 2^32 branch mutations.
	 */
	atomic_long_t branch_gen;
	/*
	 * Excludes lookup/readdir during runtime branch add/remove.
	 * Per-cpu: the read side is taken by every uncached lookup, every
	 * merged-readdir build and every directory mutation, so on a
	 * live system's parallel path walks a plain rwsem would have all
	 * CPUs trading one cacheline; percpu_down_read() touches only
	 * this CPU's counter.  The whole cost moves to the write side -
	 * an rcu_sync grace period per branch change - which already
	 * stalls the world for a shrink_dcache_sb() and a
	 * synchronize_rcu_expedited().
	 */
	struct percpu_rw_semaphore dyn_lock;
	struct backing_file_ctx backing_ctx;
	struct aufsng_config config;
	int namelen;
};

struct aufsng_dir_cache;

struct aufsng_dyn_parked;

struct aufsng_inode {
	struct aufsng_entry *oe;
	struct dentry *upperdentry;
	struct aufsng_dir_cache *cache;	/* merged readdir cache (dirs) */
	struct aufsng_dyn_parked *dyn_parked;
	atomic64_t version;		/* merged readdir cache validity */
	/* synchronize copy up and more */
	struct mutex lock;
	struct inode vfs_inode;
};

static inline struct aufsng_fs *AUFSNG_FS(struct super_block *sb)
{
	return sb->s_fs_info;
}

/*
 * udba=reval (and =notify, a superset) re-examine the real branches on
 * access so that a direct, out-of-band change to a branch - most
 * commonly a ".wh.<name>" whiteout removed by hand in the rw branch -
 * is reflected in the merged view; udba=none trusts the cache instead.
 */
static inline bool aufsng_udba_reval(struct aufsng_fs *pfs)
{
	return pfs->config.udba >= AUFSNG_UDBA_REVAL;
}

static inline struct aufsng_inode *AUFSNG_I(struct inode *inode)
{
	return container_of(inode, struct aufsng_inode, vfs_inode);
}

static inline struct aufsng_entry *AUFSNG_I_E(struct inode *inode)
{
	/* may be swapped by a dynamic branch change */
	return inode ? READ_ONCE(AUFSNG_I(inode)->oe) : NULL;
}

static inline struct aufsng_entry *AUFSNG_E(struct dentry *dentry)
{
	return AUFSNG_I_E(d_inode(dentry));
}

static inline struct dentry *aufsng_upperdentry(struct inode *inode)
{
	return READ_ONCE(AUFSNG_I(inode)->upperdentry);
}

/*
 * A dentry's two revalidation stamps: the upper dir's change signal in
 * d_fsdata (see aufsng_reval_stamp()) and the branch generation in
 * d_time (see branch_gen).  Stored and compared through this single
 * pair so the storage convention - both are unsigned-long-wide, the
 * width of d_time - lives in one place; positive, negative and
 * fresh-lookup paths all prime and gate through these.
 */
static inline void aufsng_store_reval_stamps(struct dentry *dentry,
					     unsigned long stamp,
					     unsigned long gen)
{
	WRITE_ONCE(dentry->d_fsdata, (void *)stamp);
	WRITE_ONCE(dentry->d_time, gen);
}

static inline bool aufsng_reval_stamps_match(struct dentry *dentry,
					     unsigned long stamp,
					     unsigned long gen)
{
	return (unsigned long)READ_ONCE(dentry->d_fsdata) == stamp &&
	       READ_ONCE(dentry->d_time) == gen;
}

static inline struct vfsmount *aufsng_upper_mnt(struct aufsng_fs *pfs)
{
	return pfs->upper->mnt;
}

/*
 * A layer's stable slot number.  Stored, not derived from its address
 * in the slot table: the table is reallocated when it grows, and a
 * reader here may hold nothing but the layer pointer.
 */
static inline unsigned int aufsng_layer_idx(const struct aufsng_layer *layer)
{
	return layer->idx;
}

/*
 * Is a pinned real dentry still a live entry in its branch?  Unhashed
 * or turned negative means an out-of-band unlink/rename removed it -
 * the udba=reval staleness signal, and the survivor test for branch
 * removal.  One definition so the liveness rule cannot drift between
 * revalidation, shed-upper and the removal scan.
 */
static inline bool aufsng_dentry_alive(const struct dentry *dentry)
{
	return !d_unhashed(dentry) && !d_is_negative(dentry);
}

/*
 * The single definition of what keys a union inode in the inode hash:
 * the top lower inode when a lower exists (stable across copy-up),
 * else the upper's.  @key_idx (optional) is the providing branch's
 * slot, folded into i_ino by aufsng_map_ino().  Fresh lookup
 * (aufsng_get_inode), branch-removal rekey and its duplicate check all
 * derive the key here - hardlink-aliasing correctness depends on them
 * never disagreeing.
 */
static inline struct inode *aufsng_hash_key(const struct aufsng_entry *oe,
					    struct dentry *upper,
					    unsigned int *key_idx)
{
	if (oe->numlower) {
		if (key_idx)
			*key_idx = aufsng_layer_idx(oe->lowerstack[0].layer);
		return d_inode(oe->lowerstack[0].dentry);
	}
	if (key_idx)
		*key_idx = 0;
	return upper ? d_inode(upper) : NULL;
}

static inline void aufsng_path_real(struct inode *inode, struct path *path)
{
	struct dentry *upper = aufsng_upperdentry(inode);

	if (!upper) {
		struct aufsng_entry *oe = AUFSNG_I_E(inode);

		if (oe && oe->numlower) {
			/*
			 * The entry's own mnt copy, NOT layer->mnt: this read
			 * is lockless and @oe may be a superseded stack whose
			 * branch was since removed - its layer->mnt is NULL
			 * (or reused by a later add), while the entry's copy
			 * stays pinned with the parked stack until eviction.
			 */
			path->mnt = oe->lowerstack[0].mnt;
			path->dentry = oe->lowerstack[0].dentry;
			return;
		}
		/*
		 * NULL upper combined with an empty stack is a torn
		 * snapshot: a branch removal emptied the stack after
		 * copying the object up, and it published upperdentry
		 * before releasing the new stack (same thread), so a
		 * re-read behind a read barrier must find it.  Pairs
		 * with the smp_store_release() publishing the stack in
		 * aufsng_dyn_commit_rebuild().
		 */
		smp_rmb();
		upper = aufsng_upperdentry(inode);
	}
	path->mnt = aufsng_upper_mnt(AUFSNG_FS(inode->i_sb));
	path->dentry = upper;
}

/*
 * aufsng_path_real() owns the lockless upper/stack read protocol (the
 * torn-snapshot re-read behind smp_rmb); a second open-coding here
 * would silently miss any future change to it.
 */
static inline struct inode *aufsng_inode_real(struct inode *inode)
{
	struct path path;

	aufsng_path_real(inode, &path);
	return path.dentry ? d_inode(path.dentry) : NULL;
}

/*
 * Is the lower @origin a valid copy-up origin for an object of type
 * @mode?  Only when they are the same file type: a same-named lower of
 * a different type is an independent object shadowed by the upper, not
 * its origin, and must not be aliased onto the lower's identity.
 */
static inline bool aufsng_origin_type_ok(struct dentry *origin, umode_t mode)
{
	return (d_inode(origin)->i_mode & S_IFMT) == (mode & S_IFMT);
}

/*
 * The single definition of "the union inode mirrors the real inode's
 * attributes", shared by first instantiation (aufsng_fill_inode) and
 * every later refresh (aufsng_copyattr).
 *
 * i_lock serializes concurrent refreshers: they arrive under different
 * sleeping locks (oi->lock for copy-up/adopt, i_rwsem for setattr,
 * none for revalidation and write completion), so without a common
 * lock two multi-field copies interleave and a permission check can
 * read a (mode, uid) pair spliced from two generations.  The source
 * fields are read inside the same section, so the last writer wins
 * with a coherent snapshot.
 */
static inline void aufsng_copyattr_from(struct inode *inode,
					struct inode *realinode)
{
	spin_lock(&inode->i_lock);
	inode->i_uid = realinode->i_uid;
	inode->i_gid = realinode->i_gid;
	inode->i_mode = realinode->i_mode;
	inode_set_atime_to_ts(inode, inode_get_atime(realinode));
	inode_set_mtime_to_ts(inode, inode_get_mtime(realinode));
	inode_set_ctime_to_ts(inode, inode_get_ctime(realinode));
	i_size_write(inode, i_size_read(realinode));
	set_nlink(inode, realinode->i_nlink);
	spin_unlock(&inode->i_lock);
}

static inline void aufsng_copyattr(struct inode *inode)
{
	aufsng_copyattr_from(inode, aufsng_inode_real(inode));
}

/*
 * Is @name (length @len) reserved for AUFS bookkeeping?  Any such
 * name must never be exposed to userspace via readdir, and must
 * never be treated as a "real" file matched by lookup.
 */
static inline bool aufsng_is_wh_name(const char *name, int len)
{
	return len >= AUFSNG_WH_PFX_LEN &&
	       !memcmp(name, AUFSNG_WH_PFX, AUFSNG_WH_PFX_LEN);
}

/*
 * Is @name in aufs-ng's own xattr namespace?  Nothing in it is ever
 * exposed through the union or copied up from a branch.  The test is on
 * the namespace, not the one name in it today, so a branch that once
 * WAS a rw branch (a squashfs module built from one) cannot smuggle any
 * of this bookkeeping into a new upper.
 */
static inline bool aufsng_is_private_xattr(const char *name)
{
	return !strncmp(name, AUFSNG_XATTR_PFX, sizeof(AUFSNG_XATTR_PFX) - 1);
}

/*
 * Build the on-disk whiteout name ".wh.<name>" into @buf (which must
 * hold at least NAME_MAX + 1 bytes).  The single definition of the
 * format shared by lookup, mutation and readdir.  A name too long to
 * ever have a whiteout is rejected here, which also caps the length
 * pfs->namelen advertises (see aufsng_get_namelen()).
 */
static inline int aufsng_wh_name(char *buf, const struct qstr *name,
			      struct qstr *wh)
{
	if (name->len > NAME_MAX - AUFSNG_WH_PFX_LEN)
		return -ENAMETOOLONG;
	memcpy(buf, AUFSNG_WH_PFX, AUFSNG_WH_PFX_LEN);
	memcpy(buf + AUFSNG_WH_PFX_LEN, name->name, name->len);
	*wh = QSTR_LEN(buf, AUFSNG_WH_PFX_LEN + name->len);
	return 0;
}

/*
 * Branch filesystems (one squashfs per module) number their inodes
 * independently, so raw i_ino values collide across branches while
 * st_dev is the union's for all of them - breaking hardlink detection
 * in cp -a/tar/mksquashfs and find's loop check.  Disambiguate the
 * way overlayfs's xino does: fold the branch's stable slot index into
 * the high bits.  Branch 0 and any filesystem already using the high
 * bits are left untouched.
 */
#define AUFSNG_XINO_SHIFT	40

/*
 * The union root's inode number (AUFSNG_ROOT_INO) is invented, not taken
 * from any branch, so a branch-0 object whose raw number happens to be
 * the same (a fresh tmpfs rw branch hands out 2 to the very first object
 * created in it) would otherwise report the root's exact (st_dev,
 * st_ino) - making find report a filesystem loop and archive tools
 * hardlink the two.  Move that one number out of the way instead;
 * bit 62 is above every slot-folded value (idx << 40) and practically
 * above any raw branch number.
 */
#define AUFSNG_ROOT_INO_EVADE	(1ULL << 62)

static inline u64 aufsng_map_ino(u64 ino, unsigned int idx)
{
	if (!idx)
		return ino == AUFSNG_ROOT_INO ? (ino | AUFSNG_ROOT_INO_EVADE) :
						ino;
	if (ino >> AUFSNG_XINO_SHIFT)
		return ino;
	return ino | ((u64)idx << AUFSNG_XINO_SHIFT);
}

/*
 * A lower stack superseded by a dynamic branch change (or a replaced
 * upper), kept until the inode is evicted: an operation that resolved
 * a lower path before the change may still hold pointers into it, and
 * it holds the inode for its duration.
 */
struct aufsng_dyn_parked {
	struct aufsng_dyn_parked *next;
	struct aufsng_entry *oe;
	struct dentry *upper;
	/*
	 * EVERY branch mount the parked stack's dentries point into,
	 * pinned for the park's lifetime: dropping a branch's last
	 * mount reference while an older parked stack still held
	 * dentries in that branch's sb would tear the branch down
	 * under them ("Dentry still in use" panic on umount).  Each
	 * node pins its own referenced mounts (@nr_mnts of them - one
	 * per @oe lower, or, for a pin-only node with no @oe, one per
	 * lower of the inode's CURRENT stack: a deleted-but-open
	 * object keeps its stack in place across the removal of the
	 * branch serving it, see aufsng_dyn_pin_stack()), and nodes
	 * are safe to release in any order.
	 */
	unsigned int nr_mnts;
	struct vfsmount *mnts[];
};

/* mount-time branch context collected by params.c */
struct aufsng_ctx_branch {
	char *name;
	struct path path;
	enum aufsng_br_perm perm;
	char permstr[AUFSNG_PERM_LEN];	/* the mode as given, for show_options */
};

struct aufsng_fs_context {
	struct aufsng_ctx_branch *br;
	size_t nr;
	size_t cap;
	/* "add=N:PATH=MODE" branch insertions collected during remount */
	struct aufsng_ctx_branch *dyn_add;
	size_t nr_dyn_add;
	size_t cap_dyn_add;
	/* "del=PATH" branch removals collected during remount */
	struct path *dyn_del;
	size_t nr_dyn_del;
	size_t cap_dyn_del;
	/* udba= was given explicitly (a replayed remount must not
	 * reset the mount-time choice to the parser default) */
	bool udba_set;
	struct aufsng_config config;
};

/* params.c */
int aufsng_init_fs_context(struct fs_context *fc);
extern const struct fs_parameter_spec aufsng_parameter_spec[];
int aufsng_dyn_reconfigure(struct fs_context *fc);

/* super.c */
struct aufsng_entry *aufsng_alloc_entry(unsigned int numlower);
void aufsng_free_entry(struct aufsng_entry *oe);
struct aufsng_entry *aufsng_entry_from_origin(int found,
					struct aufsng_path *origin);
struct aufsng_entry *aufsng_entry_prepend(struct aufsng_entry *cur,
				     unsigned int base_n,
				     struct aufsng_layer *layer,
				     struct dentry *dentry,
				     struct vfsmount *mnt);
int aufsng_fill_super(struct super_block *sb, struct fs_context *fc);
int aufsng_check_layer(struct super_block *sb, const struct path *path,
		    const char *name);
int aufsng_check_overlap(struct aufsng_fs *pfs, struct dentry *dentry,
		      const char *name);
struct aufsng_layer *aufsng_layer_reserve(struct aufsng_fs *pfs,
					  unsigned int idx);
int aufsng_grow_array(void **arr, size_t *cap, size_t need, size_t elemsize,
		   gfp_t gfp);
int aufsng_probe_namelen(const struct path *path, long *namelen);
int aufsng_get_namelen(struct aufsng_fs *pfs, const struct path *path);
extern const struct super_operations aufsng_super_operations;

/* file.c */
extern const struct file_operations aufsng_file_operations;
void aufsng_backing_ctx_init(struct aufsng_fs *pfs);

/* readdir.c */
extern const struct file_operations aufsng_dir_operations;
void aufsng_dir_cache_release(struct aufsng_inode *oi);
int aufsng_check_empty_dir(struct dentry *dentry);
int aufsng_clear_whiteouts(struct aufsng_fs *pfs, struct dentry *upperdir);

/* namei.c */
int aufsng_inode_test(struct inode *inode, void *data);
struct dentry *aufsng_lookup(struct inode *dir, struct dentry *dentry,
			  unsigned int flags);
struct dentry *aufsng_lookup_once(struct vfsmount *mnt, struct dentry *base,
			       const struct qstr *name, int *whiteout);
bool aufsng_lookup_negative_valid(struct inode *dir, const struct qstr *name);
struct inode *aufsng_get_inode(struct super_block *sb,
			    struct dentry *upperdentry,
			    struct aufsng_entry *oe);
const char *aufsng_get_link(struct dentry *dentry, struct inode *inode,
			 struct delayed_call *done);
int aufsng_check_whiteout(struct vfsmount *mnt, struct dentry *parent,
		       const struct qstr *name);
int aufsng_check_diropq(struct vfsmount *mnt, struct dentry *dir);
int aufsng_find_origin_ex(struct aufsng_entry *poe, const struct qstr *name,
		       const struct aufsng_layer *skip_layer, umode_t mode,
		       struct aufsng_path *out);

/* in-progress merged stack; see aufsng_merge_dirs() */
struct aufsng_merge {
	struct aufsng_entry *oe;	/* lazily allocated when NULL */
	unsigned int n;			/* entries filled so far */
	bool allow_top_nondir;
};

int aufsng_merge_dirs(struct aufsng_merge *m, struct aufsng_entry *poe,
		   unsigned int from, const struct qstr *name,
		   const struct aufsng_layer *skip);

static inline int aufsng_find_origin(struct aufsng_entry *poe,
				  const struct qstr *name,
				  struct aufsng_path *out)
{
	return aufsng_find_origin_ex(poe, name, NULL, 0, out);
}
int aufsng_lower_covers(struct inode *dir, const struct qstr *name);
bool aufsng_upper_claims_origin(struct aufsng_fs *pfs, struct dentry *upper,
			     const struct qstr *name);

/* inode.c */
extern const struct inode_operations aufsng_dir_inode_operations;
extern const struct inode_operations aufsng_file_inode_operations;
extern const struct inode_operations aufsng_symlink_inode_operations;
extern const struct inode_operations aufsng_special_inode_operations;
extern const struct xattr_handler * const aufsng_xattr_handlers[];

/* copy_up.c */
int aufsng_copy_up(struct dentry *dentry);
struct dentry *aufsng_copy_up_upper(struct dentry *dentry);
bool aufsng_has_origin(struct aufsng_fs *pfs, struct dentry *upper,
		    const struct qstr *name);

/* dir.c */
int aufsng_create(struct mnt_idmap *idmap, struct inode *dir,
	       struct dentry *dentry, umode_t mode, bool excl);
struct dentry *aufsng_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode);
int aufsng_mknod(struct mnt_idmap *idmap, struct inode *dir,
	      struct dentry *dentry, umode_t mode, dev_t rdev);
int aufsng_symlink(struct mnt_idmap *idmap, struct inode *dir,
		struct dentry *dentry, const char *link);
int aufsng_link(struct dentry *old, struct inode *dir, struct dentry *new);
int aufsng_unlink(struct inode *dir, struct dentry *dentry);
int aufsng_rmdir(struct inode *dir, struct dentry *dentry);
int aufsng_rename(struct mnt_idmap *idmap, struct inode *olddir,
	       struct dentry *old, struct inode *newdir,
	       struct dentry *new, unsigned int flags);
int aufsng_remove_object(struct aufsng_fs *pfs, struct dentry *upperdir,
		      const struct qstr *name, bool is_dir);
struct dentry *aufsng_create_slot(struct dentry *upperdir,
			       const struct qstr *name);

/* dcache.c */
extern const struct dentry_operations aufsng_dentry_operations;
unsigned long aufsng_reval_stamp(struct aufsng_fs *pfs, struct inode *dir);

/* dynlayer.c */
int aufsng_dyn_add_branch(struct super_block *sb, const char *name,
		       const struct path *path, const char *permstr);
int aufsng_dyn_del_branch(struct super_block *sb, const struct path *path);
bool aufsng_dyn_adopt_upper(struct inode *inode, struct dentry *lowerdentry,
			 struct dentry *upperdentry);
bool aufsng_dyn_shed_upper(struct inode *inode);
void aufsng_dyn_put_parked(struct aufsng_inode *oi);
void aufsng_dyn_free_parked(struct aufsng_inode *oi);

#endif /* AUFSNG_H */
