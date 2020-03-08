/*
 * File operations for Coda.
 * Original version: (C) 1996 Peter Braam 
 * Rewritten for Linux 2.1: (C) 1997 Carnegie Mellon University
 *
 * Carnegie Mellon encourages users of this code to contribute improvements
 * to the Coda project. Contact Peter Braam <coda@cs.cmu.edu>.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#include <linux/coda.h>
#include <linux/coda_psdev.h>

#include "coda_linux.h"
#include "coda_int.h"

struct coda_vm_ops {
	atomic_t refcnt;
	struct file *coda_file;
	const struct vm_operations_struct *host_vm_ops;
	struct vm_operations_struct vm_ops;
};

static ssize_t
coda_file_read(struct file *coda_file, char __user *buf, size_t count, loff_t *ppos)
{
	struct coda_file_info *cfi;
	struct file *host_file;

	cfi = CODA_FTOC(coda_file);
	BUG_ON(!cfi || cfi->cfi_magic != CODA_MAGIC);
	host_file = cfi->cfi_container;

	if (!host_file->f_op->read)
		return -EINVAL;

	return host_file->f_op->read(host_file, buf, count, ppos);
}

static ssize_t
coda_file_splice_read(struct file *coda_file, loff_t *ppos,
		      struct pipe_inode_info *pipe, size_t count,
		      unsigned int flags)
{
	ssize_t (*splice_read)(struct file *, loff_t *,
			       struct pipe_inode_info *, size_t, unsigned int);
	struct coda_file_info *cfi;
	struct file *host_file;

	cfi = CODA_FTOC(coda_file);
	BUG_ON(!cfi || cfi->cfi_magic != CODA_MAGIC);
	host_file = cfi->cfi_container;

	splice_read = host_file->f_op->splice_read;
	if (!splice_read)
		splice_read = default_file_splice_read;

	return splice_read(host_file, ppos, pipe, count, flags);
}

static ssize_t
coda_file_write(struct file *coda_file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct inode *host_inode, *coda_inode = file_inode(coda_file);
	struct coda_file_info *cfi;
	struct file *host_file;
	ssize_t ret;

	cfi = CODA_FTOC(coda_file);
	BUG_ON(!cfi || cfi->cfi_magic != CODA_MAGIC);
	host_file = cfi->cfi_container;

	if (!host_file->f_op->write)
		return -EINVAL;

	host_inode = file_inode(host_file);
	file_start_write(host_file);
	mutex_lock(&coda_inode->i_mutex);

	ret = host_file->f_op->write(host_file, buf, count, ppos);

	coda_inode->i_size = host_inode->i_size;
	coda_inode->i_blocks = (coda_inode->i_size + 511) >> 9;
	coda_inode->i_mtime = coda_inode->i_ctime = CURRENT_TIME_SEC;
	mutex_unlock(&coda_inode->i_mutex);
	file_end_write(host_file);

	return ret;
}

static void
coda_vm_open(struct vm_area_struct *vma)
{
	struct coda_vm_ops *cvm_ops =
		container_of(vma->vm_ops, struct coda_vm_ops, vm_ops);

	atomic_inc(&cvm_ops->refcnt);

	if (cvm_ops->host_vm_ops && cvm_ops->host_vm_ops->open)
		cvm_ops->host_vm_ops->open(vma);
}

static void
coda_vm_close(struct vm_area_struct *vma)
{
	struct coda_vm_ops *cvm_ops =
		container_of(vma->vm_ops, struct coda_vm_ops, vm_ops);

	if (cvm_ops->host_vm_ops && cvm_ops->host_vm_ops->close)
		cvm_ops->host_vm_ops->close(vma);

	if (atomic_dec_and_test(&cvm_ops->refcnt)) {
		vma->vm_ops = cvm_ops->host_vm_ops;
		fput(cvm_ops->coda_file);
		kfree(cvm_ops);
	}
}

static int
coda_file_mmap(struct file *coda_file, struct vm_area_struct *vma)
{
	struct coda_file_info *cfi;
	struct coda_inode_info *cii;
	struct file *host_file;
	struct inode *coda_inode, *host_inode;
	struct coda_vm_ops *cvm_ops;
	int ret;

	cfi = CODA_FTOC(coda_file);
	BUG_ON(!cfi || cfi->cfi_magic != CODA_MAGIC);
	host_file = cfi->cfi_container;

	if (!host_file->f_op->mmap)
		return -ENODEV;

	if (WARN_ON(coda_file != vma->vm_file))
		return -EIO;

	cvm_ops = kmalloc(sizeof(struct coda_vm_ops), GFP_KERNEL);
	if (!cvm_ops)
		return -ENOMEM;

	coda_inode = file_inode(coda_file);
	host_inode = file_inode(host_file);

	cii = ITOC(coda_inode);
	spin_lock(&cii->c_lock);
	coda_file->f_mapping = host_file->f_mapping;
	if (coda_inode->i_mapping == &coda_inode->i_data)
		coda_inode->i_mapping = host_inode->i_mapping;

	/* only allow additional mmaps as long as userspace isn't changing
	 * the container file on us! */
	else if (coda_inode->i_mapping != host_inode->i_mapping) {
		spin_unlock(&cii->c_lock);
		kfree(cvm_ops);
		return -EBUSY;
	}

	/* keep track of how often the coda_inode/host_file has been mmapped */
	cii->c_mapcount++;
	cfi->cfi_mapcount++;
	spin_unlock(&cii->c_lock);

	vma->vm_file = get_file(host_file);
	ret = host_file->f_op->mmap(host_file, vma);

	if (ret) {
		/* if ->mmap fails, our caller will put coda_file so we
		 * should drop the reference to the host_file that we got.
		 */
		fput(host_file);
		kfree(cvm_ops);
	} else {
		/* here we add redirects for the open/close vm_operations */
		cvm_ops->host_vm_ops = vma->vm_ops;
		if (vma->vm_ops)
			cvm_ops->vm_ops = *vma->vm_ops;

		cvm_ops->vm_ops.open = coda_vm_open;
		cvm_ops->vm_ops.close = coda_vm_close;
		cvm_ops->coda_file = coda_file;
		atomic_set(&cvm_ops->refcnt, 1);

		vma->vm_ops = &cvm_ops->vm_ops;
	}
	return ret;
}

int coda_open(struct inode *coda_inode, struct file *coda_file)
{
	struct file *host_file = NULL;
	int error;
	unsigned short flags = coda_file->f_flags & (~O_EXCL);
	unsigned short coda_flags = coda_flags_to_cflags(flags);
	struct coda_file_info *cfi;

	cfi = kmalloc(sizeof(struct coda_file_info), GFP_KERNEL);
	if (!cfi)
		return -ENOMEM;

	error = venus_open(coda_inode->i_sb, coda_i2f(coda_inode), coda_flags,
			   &host_file);
	if (!host_file)
		error = -EIO;

	if (error) {
		kfree(cfi);
		return error;
	}

	host_file->f_flags |= coda_file->f_flags & (O_APPEND | O_SYNC);

	cfi->cfi_magic = CODA_MAGIC;
	cfi->cfi_mapcount = 0;
	cfi->cfi_container = host_file;

	BUG_ON(coda_file->private_data != NULL);
	coda_file->private_data = cfi;
	return 0;
}

int coda_release(struct inode *coda_inode, struct file *coda_file)
{
	unsigned short flags = (coda_file->f_flags) & (~O_EXCL);
	unsigned short coda_flags = coda_flags_to_cflags(flags);
	struct coda_file_info *cfi;
	struct coda_inode_info *cii;
	struct inode *host_inode;
	int err;

	cfi = CODA_FTOC(coda_file);
	BUG_ON(!cfi || cfi->cfi_magic != CODA_MAGIC);

	err = venus_close(coda_inode->i_sb, coda_i2f(coda_inode),
			  coda_flags, coda_file->f_cred->fsuid);

	host_inode = file_inode(cfi->cfi_container);
	cii = ITOC(coda_inode);

	/* did we mmap this file? */
	spin_lock(&cii->c_lock);
	if (coda_inode->i_mapping == &host_inode->i_data) {
		cii->c_mapcount -= cfi->cfi_mapcount;
		if (!cii->c_mapcount)
			coda_inode->i_mapping = &coda_inode->i_data;
	}
	spin_unlock(&cii->c_lock);

	fput(cfi->cfi_container);
	kfree(coda_file->private_data);
	coda_file->private_data = NULL;

	/* VFS fput ignores the return value from file_operations->release, so
	 * there is no use returning an error here */
	return 0;
}

int coda_fsync(struct file *coda_file, loff_t start, loff_t end, int datasync)
{
	struct file *host_file;
	struct inode *coda_inode = file_inode(coda_file);
	struct coda_file_info *cfi;
	int err;

	if (!(S_ISREG(coda_inode->i_mode) || S_ISDIR(coda_inode->i_mode) ||
	      S_ISLNK(coda_inode->i_mode)))
		return -EINVAL;

	err = filemap_write_and_wait_range(coda_inode->i_mapping, start, end);
	if (err)
		return err;
	mutex_lock(&coda_inode->i_mutex);

	cfi = CODA_FTOC(coda_file);
	BUG_ON(!cfi || cfi->cfi_magic != CODA_MAGIC);
	host_file = cfi->cfi_container;

	err = vfs_fsync(host_file, datasync);
	if (!err && !datasync)
		err = venus_fsync(coda_inode->i_sb, coda_i2f(coda_inode));
	mutex_unlock(&coda_inode->i_mutex);

	return err;
}

const struct file_operations coda_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= coda_file_read,
	.write		= coda_file_write,
	.mmap		= coda_file_mmap,
	.open		= coda_open,
	.release	= coda_release,
	.fsync		= coda_fsync,
	.splice_read	= coda_file_splice_read,
};
