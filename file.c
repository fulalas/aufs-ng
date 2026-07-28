// SPDX-License-Identifier: GPL-2.0-only
/*
 * aufs-ng regular file I/O: passthrough to the real file via the
 * kernel's backing-file API (the same infrastructure FUSE passthrough
 * and overlayfs use), so reads, writes, splice and mmap run at native
 * speed on the layer filesystem with no per-superblock locking.
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/backing-file.h>
#include <linux/cred.h>
#include <linux/uio.h>
#include <linux/splice.h>
#include "aufsng.h"

static void aufsng_file_accessed(struct file *file)
{
	struct inode *inode = file_inode(file);

	inode_set_atime_to_ts(inode,
			      inode_get_atime(aufsng_inode_real(inode)));
}

static void aufsng_file_end_write(struct kiocb *iocb, ssize_t ret)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	aufsng_copyattr(inode);
}

static struct backing_file_ctx *aufsng_backing_ctx(struct super_block *sb)
{
	return &AUFSNG_FS(sb)->backing_ctx;
}

static int aufsng_open(struct inode *inode, struct file *file)
{
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	struct file *realfile;
	struct path realpath;

	if ((file->f_flags & O_ACCMODE) != O_RDONLY &&
	    !aufsng_upperdentry(inode)) {
		int err = aufsng_copy_up(file->f_path.dentry);

		if (err)
			return err;
	}

	aufsng_path_real(inode, &realpath);
	realfile = backing_file_open(file, file->f_flags, &realpath,
				     pfs->creator_cred);
	if (IS_ERR(realfile))
		return PTR_ERR(realfile);

	file->private_data = realfile;
	return 0;
}

static int aufsng_release(struct inode *inode, struct file *file)
{
	fput(file->private_data);
	return 0;
}

static loff_t aufsng_llseek(struct file *file, loff_t offset, int whence)
{
	struct file *realfile = file->private_data;
	struct inode *inode = file_inode(file);
	loff_t ret;

	/*
	 * Position queries and a plain rewind need neither the real fs
	 * nor a lock: the union file's f_pos is the master copy (every
	 * I/O path passes iocb->ki_pos explicitly, and the delegated
	 * path below resyncs realfile->f_pos from it before seeking).
	 * Answering them here keeps an exclusive i_rwsem acquire - which
	 * fallocate/truncate/setattr contend on - off a pure query path,
	 * exactly as overlayfs's ovl_llseek does.
	 */
	if (offset == 0) {
		if (whence == SEEK_CUR)
			return file->f_pos;
		if (whence == SEEK_SET)
			return vfs_setpos(file, 0, 0);
	}

	/*
	 * Delegate to the real fs so SEEK_END/SEEK_DATA/SEEK_HOLE see
	 * the authoritative file size, keeping the positions in sync.
	 */
	inode_lock(inode);
	realfile->f_pos = file->f_pos;
	ret = vfs_llseek(realfile, offset, whence);
	file->f_pos = realfile->f_pos;
	inode_unlock(inode);

	return ret;
}

static ssize_t aufsng_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;

	return backing_file_read_iter(file->private_data, iter, iocb,
				      iocb->ki_flags,
				      aufsng_backing_ctx(file_inode(file)->i_sb));
}

static ssize_t aufsng_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;

	if (!iov_iter_count(iter))
		return 0;

	return backing_file_write_iter(file->private_data, iter, iocb,
				       iocb->ki_flags,
				       aufsng_backing_ctx(file_inode(file)->i_sb));
}

static ssize_t aufsng_splice_read(struct file *in, loff_t *ppos,
			       struct pipe_inode_info *pipe, size_t len,
			       unsigned int flags)
{
	struct kiocb iocb;
	ssize_t ret;

	init_sync_kiocb(&iocb, in);
	iocb.ki_pos = *ppos;
	ret = backing_file_splice_read(in->private_data, &iocb, pipe, len,
				       flags,
				       aufsng_backing_ctx(file_inode(in)->i_sb));
	*ppos = iocb.ki_pos;

	return ret;
}

static ssize_t aufsng_splice_write(struct pipe_inode_info *pipe, struct file *out,
				loff_t *ppos, size_t len, unsigned int flags)
{
	struct kiocb iocb;
	ssize_t ret;

	init_sync_kiocb(&iocb, out);
	iocb.ki_pos = *ppos;
	ret = backing_file_splice_write(pipe, out->private_data, &iocb, len,
					flags,
					aufsng_backing_ctx(file_inode(out)->i_sb));
	*ppos = iocb.ki_pos;

	return ret;
}

static int aufsng_mmap(struct file *file, struct vm_area_struct *vma)
{
	return backing_file_mmap(file->private_data, vma,
				 aufsng_backing_ctx(file_inode(file)->i_sb));
}

static int aufsng_fsync(struct file *file, loff_t start, loff_t end,
		     int datasync)
{
	struct inode *inode = file_inode(file);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	struct file *realfile = file->private_data;
	struct dentry *upper = aufsng_upperdentry(inode);
	struct file *upperfile;
	struct path upperpath;
	int err;

	/*
	 * No upper at all: every lower branch is read-only (and may be
	 * fsync-less, e.g. squashfs), so nothing can be dirty.
	 */
	if (!upper)
		return 0;

	/* the common case: this fd already writes through the current upper */
	if (file_inode(realfile) == d_inode(upper))
		return vfs_fsync_range(realfile, start, end, datasync);

	/*
	 * This fd's backing file is NOT the current upper: it was opened
	 * before the copy-up (so it still points at the lower), or an
	 * adopt/shed replaced the upper under it (udba=reval).  fsync(2) is
	 * a barrier for the FILE, not for the descriptor it is called on, so
	 * returning 0 here would report durability for data that was never
	 * written back - the upper's dirty pages after a copy-up, or, when
	 * the upper was replaced, even this fd's OWN writes.
	 *
	 * Sync both halves: the fd's backing file when it could have been
	 * written through (a superseded upper stays pinned while the fd
	 * lives), and the current upper - opened for the sync exactly as
	 * aufsng_dir_fsync() opens it, and as overlayfs's ovl_fsync does for
	 * this same case, rather than skipping the sync.
	 */
	if (realfile->f_mode & FMODE_WRITE) {
		err = vfs_fsync_range(realfile, start, end, datasync);
		if (err)
			return err;
	}

	upperpath.mnt = aufsng_upper_mnt(pfs);
	upperpath.dentry = upper;
	/* read-only: fsync needs no write mode, and O_WRONLY could truncate */
	upperfile = kernel_file_open(&upperpath, O_RDONLY | O_LARGEFILE,
				     pfs->creator_cred);
	if (IS_ERR(upperfile))
		return PTR_ERR(upperfile);
	err = vfs_fsync_range(upperfile, start, end, datasync);
	fput(upperfile);
	return err;
}

static int aufsng_flush(struct file *file, fl_owner_t id)
{
	struct file *realfile = file->private_data;

	if (realfile->f_op->flush)
		return realfile->f_op->flush(realfile, id);
	return 0;
}

static long aufsng_fallocate(struct file *file, int mode, loff_t offset,
			  loff_t len)
{
	struct inode *inode = file_inode(file);
	struct aufsng_fs *pfs = AUFSNG_FS(inode->i_sb);
	const struct cred *old_cred;
	long ret;

	/*
	 * The VFS only allows fallocate on a write-opened fd, and a
	 * write open already copied up (aufsng_open), so the backing
	 * file is the upper one - a plain passthrough, like original
	 * AUFS's aufs_fallocate.  The size may change; re-mirror.
	 */
	inode_lock(inode);
	old_cred = override_creds(pfs->creator_cred);
	ret = vfs_fallocate(file->private_data, mode, offset, len);
	revert_creds(old_cred);
	if (!ret)
		aufsng_copyattr_from(inode, file_inode(file->private_data));
	inode_unlock(inode);

	return ret;
}

const struct file_operations aufsng_file_operations = {
	.open		= aufsng_open,
	.release	= aufsng_release,
	.llseek		= aufsng_llseek,
	.read_iter	= aufsng_read_iter,
	.write_iter	= aufsng_write_iter,
	.splice_read	= aufsng_splice_read,
	.splice_write	= aufsng_splice_write,
	.mmap		= aufsng_mmap,
	.fsync		= aufsng_fsync,
	.flush		= aufsng_flush,
	.fallocate	= aufsng_fallocate,
};

void aufsng_backing_ctx_init(struct aufsng_fs *pfs)
{
	pfs->backing_ctx.cred = pfs->creator_cred;
	pfs->backing_ctx.accessed = aufsng_file_accessed;
	pfs->backing_ctx.end_write = aufsng_file_end_write;
}
