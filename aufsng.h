/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * aufs-ng - standalone union filesystem, semantically compatible with
 * AUFS: registers as filesystem type "aufs", takes AUFS's mount option
 * syntax and uses its on-disk whiteout format, so scripts that mount,
 * remount or scan a branch directly need no changes.
 *
 * Branch 0 is the single writable branch; 1.. are read-only, highest
 * priority first ("add=1:" inserts just below branch 0, so the newest
 * branch wins - AUFS's own "last-added-wins").
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

/*
 * Branch ceiling: AUFS's own maximum, and the widest slot number
 * aufsng_map_ino() can still fold into i_ino.  The one limit for both
 * "br:" at mount and "add=" at remount.  Not a preallocation size -
 * the slot table grows on demand.
 */
#define AUFSNG_MAXBRANCH	32767

/* AUFS on-disk whiteout format (verified against aufs-standalone) */
#define AUFSNG_WH_PFX		".wh."
#define AUFSNG_WH_PFX_LEN		4
#define AUFSNG_WH_DIROPQ		".wh..wh..opq"
#define AUFSNG_WH_MODE		0444

/*
 * The one piece of bookkeeping AUFS keeps in xino tables instead: an
 * xattr copy-up sets on an upper non-directory, holding the NAME it
 * was copied up under.
 *
 * A union inode is keyed by that lower so st_ino survives copy-up and
 * eviction, and only a real copy-up may claim the link.  A file that
 * merely shares a name - created over a whiteout, or renamed onto it -
 * is an independent object with its own identity; guessing from the
 * name handed it the deleted file's inode, ETXTBSY included.
 *
 * Storing the NAME is what makes a per-inode xattr answer a per-NAME
 * question: link(2) and rename give the inode names it was never
 * copied up under, and both then simply stop matching.
 *
 * "trusted." keeps it from unprivileged users; inode.c hides it from
 * the union so it never leaks into a listing or a cp -a.
 */
#define AUFSNG_XATTR_PFX	"trusted.aufs_ng."
#define AUFSNG_XATTR_ORIGIN	AUFSNG_XATTR_PFX "origin"

enum aufsng_br_perm {
	AUFSNG_BR_RW,
	AUFSNG_BR_RO,	/* "ro"/"rr": read-only, never written to */
};

/*
 * One branch.  Never moved or freed before umount: aufsng_entry stacks
 * reference it by pointer.  Only the slot table grows.
 */
struct aufsng_layer {
	struct vfsmount *mnt;	/* private clone; NULL marks a free slot */
	unsigned int idx;	/* stable slot number, see aufsng_layer_idx() */
	/* path as given, for log messages; the clone has none.  NULL if free */
	char *path;
};

struct aufsng_path {
	struct aufsng_layer *layer;
	struct dentry *dentry;
	/*
	 * The branch mount as captured when the entry was built.
	 * Lockless readers must use this copy, never layer->mnt: a
	 * removal blanks that (and a later add may reuse the slot) while
	 * a parked stack is still being dereferenced.  The park pins the
	 * vfsmount, so this stays valid until the inode is evicted.
	 */
	struct vfsmount *mnt;
};

/*
 * Lower branch stack of a dentry/inode, top priority first.  Swapped
 * under RCU by branch changes; readers hold rcu_read_lock() or
 * dyn_lock.
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
	 * Branch slots: [0] rw, [1..] ro.  Pointers, so the table can grow
	 * without relocating a branch.  Touched only by mount, umount and
	 * branch add/remove, all under s_umount, so it needs no RCU: no
	 * lookup, readdir or stat path reads it.
	 */
	struct aufsng_layer **layers;
	struct aufsng_layer *upper;	/* == layers[0], the only rw branch */
	unsigned long si_id;		/* the "si=" mount id in /proc/mounts */
	const struct cred *creator_cred;
	/*
	 * Bumped once per branch add/remove (AUFS's "sigen") and compared
	 * against each dentry's d_time, so revalidation re-runs the merge
	 * decision once per branch change and never on ordinary
	 * mutations.  Per-DENTRY: hardlink siblings share one inode, but
	 * the winning branch is per-name.  Unsigned-long wide, the width
	 * of d_time, so no bits are dropped on 32-bit.
	 */
	atomic_long_t branch_gen;
	/*
	 * Excludes lookup/readdir during a branch add/remove.  Per-cpu
	 * because the read side is taken by every uncached lookup,
	 * readdir build and directory mutation, where a plain rwsem would
	 * have all CPUs trading one cacheline.  The cost moves to the
	 * write side, which already stalls for a shrink and a grace
	 * period.
	 */
	struct percpu_rw_semaphore dyn_lock;
	struct backing_file_ctx backing_ctx;
	struct aufsng_config config;
	/* long, as statfs's f_namelen and aufsng_probe_namelen() both are */
	long namelen;
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
 * udba=reval (and =notify) re-examine the branches on access, so an
 * out-of-band change shows up in the merged view; =none trusts the
 * cache instead.
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
 * d_fsdata and the branch generation in d_time.  Stored and compared
 * through this one pair so the width convention lives in one place.
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

/* Stored, not derived from the slot table: the table moves when it grows. */
static inline unsigned int aufsng_layer_idx(const struct aufsng_layer *layer)
{
	return layer->idx;
}

/*
 * Is a pinned real dentry still live in its branch?  Unhashed or
 * negative means an out-of-band unlink/rename took it.  One definition
 * for revalidation, shed-upper and the removal scan alike.
 */
static inline bool aufsng_dentry_alive(const struct dentry *dentry)
{
	return !d_unhashed(dentry) && !d_is_negative(dentry);
}

/*
 * What keys a union inode in the inode hash: the top lower when one
 * exists (stable across copy-up), else the upper.  @key_idx is the
 * providing slot, folded into i_ino.  Lookup, the branch-removal rekey
 * and its duplicate check all derive it here - hardlink-aliasing
 * correctness depends on them never disagreeing.
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
			 * The entry's own copy, NOT layer->mnt: this read is
			 * lockless and @oe may be a superseded stack whose
			 * branch is gone.
			 */
			path->mnt = oe->lowerstack[0].mnt;
			path->dentry = oe->lowerstack[0].dentry;
			return;
		}
		/*
		 * NULL upper plus an empty stack is a torn snapshot: the
		 * removal published upperdentry before the new stack, so a
		 * re-read behind a barrier must find it.  Pairs with the
		 * smp_store_release() in aufsng_dyn_commit_rebuild().
		 */
		smp_rmb();
		upper = aufsng_upperdentry(inode);
	}
	path->mnt = aufsng_upper_mnt(AUFSNG_FS(inode->i_sb));
	path->dentry = upper;
}

/* aufsng_path_real() owns the lockless upper/stack read protocol */
static inline struct inode *aufsng_inode_real(struct inode *inode)
{
	struct path path;

	aufsng_path_real(inode, &path);
	return path.dentry ? d_inode(path.dentry) : NULL;
}

/*
 * Is @origin a valid copy-up origin for type @mode?  Only at the same
 * file type: a same-named lower of another type is an independent
 * object, and must not be aliased onto the lower's identity.
 */
static inline bool aufsng_origin_type_ok(struct dentry *origin, umode_t mode)
{
	return (d_inode(origin)->i_mode & S_IFMT) == (mode & S_IFMT);
}

/*
 * "The union inode mirrors the real inode's attributes", shared by
 * first instantiation and every later refresh.
 *
 * i_lock serializes refreshers: they arrive under different sleeping
 * locks, so without a common one two multi-field copies interleave and
 * a permission check reads a (mode, uid) pair from two generations.
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
	struct inode *real = aufsng_inode_real(inode);

	/*
	 * No real object at all: the upper is gone and the stack is
	 * empty.  Nothing to mirror, and the union inode's attributes
	 * are as current as they can be - keep them.
	 */
	if (real)
		aufsng_copyattr_from(inode, real);
}

/*
 * Is @name reserved for AUFS bookkeeping?  Such names are never shown
 * by readdir nor matched by lookup.
 */
static inline bool aufsng_is_wh_name(const char *name, int len)
{
	return len >= AUFSNG_WH_PFX_LEN &&
	       !memcmp(name, AUFSNG_WH_PFX, AUFSNG_WH_PFX_LEN);
}

/*
 * Is @name in aufs-ng's own xattr namespace?  Nothing in it is exposed
 * or copied up.  The test is on the namespace, not today's one name,
 * so a branch built from an old rw branch cannot smuggle any in.
 */
static inline bool aufsng_is_private_xattr(const char *name)
{
	return !strncmp(name, AUFSNG_XATTR_PFX, sizeof(AUFSNG_XATTR_PFX) - 1);
}

/*
 * Build ".wh.<name>" into @buf (at least NAME_MAX + 1 bytes) - the one
 * definition of the format.  A name too long to have a whiteout is
 * rejected here, which also caps what namelen advertises.
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
 * Branch filesystems number inodes independently, so raw i_ino values
 * collide while st_dev is the union's for all - breaking hardlink
 * detection in cp -a/tar and find's loop check.  Fold the branch's
 * slot into the high bits, as overlayfs's xino does.  Branch 0, and
 * any fs already using those bits, are left alone.
 *
 * The result lands in i_ino, an unsigned long, so both this shift and
 * the root evasion below need 64 bits - hence Kconfig's "depends on
 * 64BIT", as overlayfs gates its own xino.
 */
#define AUFSNG_XINO_SHIFT	40

/*
 * AUFSNG_ROOT_INO is invented, not taken from a branch, so a branch-0
 * object with the same raw number would report the root's exact
 * (st_dev, st_ino) - a filesystem loop to find, a hardlink to
 * archivers.  Move that one number aside; bit 62 is above every
 * slot-folded value and any plausible raw number.
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
 * A stack superseded by a branch change or a replaced upper, kept
 * until the inode is evicted: an operation that resolved a path
 * before the change may still hold pointers into it.
 */
struct aufsng_dyn_parked {
	struct aufsng_dyn_parked *next;
	struct aufsng_entry *oe;
	struct dentry *upper;
	/*
	 * EVERY mount the parked dentries point into, pinned for the
	 * park's lifetime: dropping a branch's last reference while an
	 * older park still holds dentries in its sb tears the branch down
	 * under them.  Each node pins its own (@nr_mnts: one per @oe
	 * lower, or per lower of the CURRENT stack for a pin-only node),
	 * so nodes release in any order.
	 */
	unsigned int nr_mnts;
	struct vfsmount *mnts[];
};

/* mount-time branch context collected by params.c */
struct aufsng_ctx_branch {
	char *name;
	struct path path;
	enum aufsng_br_perm perm;
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
		       const struct path *path);
int aufsng_dyn_del_branch(struct super_block *sb, const struct path *path);
bool aufsng_dyn_adopt_upper(struct inode *inode, struct dentry *lowerdentry,
			 struct dentry *upperdentry);
bool aufsng_dyn_shed_upper(struct inode *inode);
void aufsng_dyn_put_parked(struct aufsng_inode *oi);
void aufsng_dyn_free_parked(struct aufsng_inode *oi);

#endif /* AUFSNG_H */
