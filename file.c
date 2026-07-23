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
	struct file *realfile = file->private_data;
	struct dentry *upper = aufsng_upperdentry(inode);

	/*
	 * Nothing to sync unless this fd's own backing file is the
	 * upper one: lower layers are read-only, and an fd opened
	 * O_RDONLY before a copy-up still points at the (possibly
	 * fsync-less, e.g. squashfs) lower file and has written
	 * nothing.
	 */
	if (!upper || file_inode(realfile) != d_inode(upper))
		return 0;

	return vfs_fsync_range(realfile, start, end, datasync);
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
		aufsng_copyattr(inode);
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
