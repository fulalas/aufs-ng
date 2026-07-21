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
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/limits.h>
#include <linux/backing-file.h>
#include "compat.h"

#define AUFSNG_NAME		"aufs"
#define AUFSNG_SUPER_MAGIC	0x61756673	/* "aufs" */
#define AUFSNG_ROOT_INO		2		/* AUFS_ROOT_INO */

#define AUFSNG_MAX_STACK		500
#define AUFSNG_MAXBRANCH_DEF	128

/* AUFS on-disk whiteout format (verified against aufs-standalone) */
#define AUFSNG_WH_PFX		".wh."
#define AUFSNG_WH_PFX_LEN		4
#define AUFSNG_WH_DIROPQ		".wh..wh..opq"
#define AUFSNG_WH_MODE		0444

enum aufsng_br_perm {
	AUFSNG_BR_RW,
	AUFSNG_BR_RO,	/* "ro"/"rr": read-only, never written to */
};

struct aufsng_layer {
	struct vfsmount *mnt;	/* private clone; NULL marks a free slot */
	int idx;		/* stable slot number for the layer's lifetime */
};

struct aufsng_path {
	struct aufsng_layer *layer;
	struct dentry *dentry;
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

/* longest AUFS branch mode token is "rw+nolwh" */
#define AUFSNG_PERM_LEN	12

struct aufsng_config {
	/*
	 * Slot-indexed branch path strings and mode tokens, only
	 * consumed by show_options: the private clone mounts have no
	 * namespace path to resolve at print time, and the mode is
	 * echoed back exactly as given ("rr" stays "rr"), so both are
	 * kept verbatim.
	 */
	char **br_paths;
	char (*br_perms)[AUFSNG_PERM_LEN];
	char *xino_path;	/* accepted, not functionally used */
	unsigned int udba;	/* accepted, not functionally used */
	unsigned int maxbranch;
};

enum aufsng_udba { AUFSNG_UDBA_NONE, AUFSNG_UDBA_REVAL, AUFSNG_UDBA_NOTIFY };

struct aufsng_fs {
	unsigned int numlayer;		/* used slots incl. [0] = branch 0 */
	unsigned int numlayer_cap;	/* maxbranch + 1, allocated up front */
	struct aufsng_layer *layers;	/* [0] rw branch, [1..] ro branches */
	const struct cred *creator_cred;
	/* excludes lookup/readdir during runtime branch add/remove */
	struct rw_semaphore dyn_lock;
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

static inline struct vfsmount *aufsng_upper_mnt(struct aufsng_fs *pfs)
{
	return pfs->layers[0].mnt;
}

static inline struct inode *aufsng_inode_real(struct inode *inode)
{
	struct dentry *upper = aufsng_upperdentry(inode);
	struct aufsng_entry *oe;

	if (upper)
		return d_inode(upper);
	oe = AUFSNG_I_E(inode);
	return oe && oe->numlower ? d_inode(oe->lowerstack[0].dentry) : NULL;
}

static inline void aufsng_path_real(struct inode *inode, struct path *path)
{
	struct dentry *upper = aufsng_upperdentry(inode);

	if (upper) {
		path->mnt = aufsng_upper_mnt(AUFSNG_FS(inode->i_sb));
		path->dentry = upper;
	} else {
		struct aufsng_entry *oe = AUFSNG_I_E(inode);

		path->mnt = oe->lowerstack[0].layer->mnt;
		path->dentry = oe->lowerstack[0].dentry;
	}
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
 */
static inline void aufsng_copyattr_from(struct inode *inode,
					struct inode *realinode)
{
	inode->i_uid = realinode->i_uid;
	inode->i_gid = realinode->i_gid;
	inode->i_mode = realinode->i_mode;
	inode_set_atime_to_ts(inode, inode_get_atime(realinode));
	inode_set_mtime_to_ts(inode, inode_get_mtime(realinode));
	inode_set_ctime_to_ts(inode, inode_get_ctime(realinode));
	i_size_write(inode, i_size_read(realinode));
	set_nlink(inode, realinode->i_nlink);
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

static inline u64 aufsng_map_ino(u64 ino, unsigned int idx)
{
	if (!idx || ino >> AUFSNG_XINO_SHIFT)
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
	 * node pins its own referenced mounts, so nodes are safe to
	 * release in any order.
	 */
	unsigned int nr_mnt;
	struct vfsmount *mnts[];
};

/* mount-time branch context collected by params.c */
struct aufsng_ctx_branch {
	char *name;
	struct path path;
	enum aufsng_br_perm perm;
	char permstr[AUFSNG_PERM_LEN];	/* the mode as given, for show_options */
	unsigned int pos;	/* insert position; only used by dyn_add */
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
	struct aufsng_config config;
};

/* params.c */
int aufsng_init_fs_context(struct fs_context *fc);
extern const struct fs_parameter_spec aufsng_parameter_spec[];
int aufsng_dyn_reconfigure(struct fs_context *fc);

/* super.c */
struct aufsng_entry *aufsng_alloc_entry(unsigned int numlower);
void aufsng_free_entry(struct aufsng_entry *oe);
int aufsng_fill_super(struct super_block *sb, struct fs_context *fc);
int aufsng_check_layer(struct super_block *sb, const struct path *path,
		    const char *name);
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
int aufsng_find_origin(struct aufsng_entry *poe, const struct qstr *name,
		    struct aufsng_path *out);

/* inode.c */
extern const struct inode_operations aufsng_dir_inode_operations;
extern const struct inode_operations aufsng_file_inode_operations;
extern const struct inode_operations aufsng_symlink_inode_operations;
extern const struct inode_operations aufsng_special_inode_operations;
extern const struct xattr_handler * const aufsng_xattr_handlers[];
ssize_t aufsng_listxattr(struct dentry *dentry, char *list, size_t size);

/* copy_up.c */
int aufsng_copy_up(struct dentry *dentry);

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
int aufsng_create_whiteout(struct aufsng_fs *pfs, struct dentry *upperdir,
			const struct qstr *name);
int aufsng_remove_whiteout(struct aufsng_fs *pfs, struct dentry *upperdir,
			const struct qstr *name);
struct dentry *aufsng_create_slot(struct dentry *upperdir,
			       const struct qstr *name);

/* dcache.c */
extern const struct dentry_operations aufsng_dentry_operations;

/* dynlayer.c */
int aufsng_dyn_add_branch(struct super_block *sb, const char *name,
		       const struct path *path, unsigned int pos,
		       enum aufsng_br_perm perm, const char *permstr);
int aufsng_dyn_del_branch(struct super_block *sb, const struct path *path,
		       bool *dcache_fresh);
bool aufsng_dyn_adopt_upper(struct inode *inode, struct dentry *lowerdentry,
			 struct dentry *upperdentry);
void aufsng_dyn_put_parked(struct aufsng_inode *oi);
void aufsng_dyn_free_parked(struct aufsng_inode *oi);

#endif /* AUFSNG_H */
