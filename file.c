// SPDX-License-Identifier: GPL-2.0-only
/*
 * Regular file I/O: passthrough to the real file via the kernel's
 * backing-file API, so reads, writes, splice and mmap run at native
 * speed with no per-superblock locking.
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
	 * Queries and a plain rewind need neither the real fs nor a lock:
	 * the union file's f_pos is the master copy.  Answering here keeps
	 * an exclusive i_rwsem acquire off a pure query path.
	 */
	if (offset == 0) {
		if (whence == SEEK_CUR)
			return file->f_pos;
		if (whence == SEEK_SET)
			return vfs_setpos(file, 0, 0);
	}

	/* SEEK_END/DATA/HOLE need the real fs; keep the positions in sync */
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
	struct inode *inode = file_inode(file);
	ssize_t ret;

	if (!iov_iter_count(iter))
		return 0;

	/*
	 * Locked for the set-id kill: backing_file_write_iter drops the
	 * bits through THIS inode's setattr, and notify_change demands
	 * its i_rwsem (as ovl_write_iter).
	 */
	inode_lock(inode);
	ret = backing_file_write_iter(file->private_data, iter, iocb,
				      iocb->ki_flags,
				      aufsng_backing_ctx(inode->i_sb));
	inode_unlock(inode);

	return ret;
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
	struct inode *inode = file_inode(out);
	struct kiocb iocb;
	ssize_t ret;

	/* locked for the same set-id kill as aufsng_write_iter */
	inode_lock(inode);
	init_sync_kiocb(&iocb, out);
	iocb.ki_pos = *ppos;
	ret = backing_file_splice_write(pipe, out->private_data, &iocb, len,
					flags,
					aufsng_backing_ctx(inode->i_sb));
	*ppos = iocb.ki_pos;
	inode_unlock(inode);

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

	/* No upper: every lower is read-only, so nothing can be dirty */
	if (!upper)
		return 0;

	/* the common case: this fd already writes through the current upper */
	if (file_inode(realfile) == d_inode(upper))
		return vfs_fsync_range(realfile, start, end, datasync);

	/*
	 * The fd predates a copy-up, or an adopt/shed replaced the upper
	 * under it.  fsync(2) is a barrier for the FILE, so returning 0
	 * would claim durability for the upper's dirty pages - or for this
	 * fd's own writes.  Sync both halves instead.
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
	 * fallocate needs a write-opened fd, and a write open already
	 * copied up, so the backing file is the upper: plain passthrough.
	 * The size may change; re-mirror.
	 */
	inode_lock(inode);
	/*
	 * The set-id kill, under the CALLER's creds - the mounter's, which
	 * everything below runs as, have CAP_FSETID and would keep the bits
	 * (as ovl_fallocate).
	 */
	ret = file_remove_privs(file);
	if (ret) {
		inode_unlock(inode);
		return ret;
	}
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
