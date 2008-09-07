/*
 * tux3fuse: Mount tux3 in userspace.
 * Copyright (C) 2008 Conrad Meyer <konrad@tylerc.org>
 * Large portions completely stolen from Daniel Phillip's tux3.c.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Rewrite to fuse low level API by Tero Roponen <tero.roponen@gmail.com>
 */

/*
 * Compile: gcc -std=gnu99 buffer.c diskio.c fuse-tux3.c -D_FILE_OFFSET_BITS=64 -lfuse -o fuse-tux3
 * (-D_FILE_OFFSET_BITS=64 might be only on 64 bit platforms, not sure.)
 * Run:
 * 0. sudo mknod -m 666 /dev/fuse c 10 229
 *    Install libfuse and headers: sudo apt-get install libfuse-dev
 *    Install fuse-utils: sudo apt-get install fuse-utils
 *    build fuse kernel module: cd linux && make ;-)
 *    insert fuse kernel module: sudo insmod fs/fuse/fuse.ko
 * 1. Create a tux3 fs on __fuse__tux3fs using some combination of dd
 *    and ./tux3 make __fuse__tux3fs.
 * 2. Mount on foo/ like: ./fuse-tux3 __fuse__tux3fs -f foo/ (-f for foreground)
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/types.h>
#include "trace.h"
#include "tux3.h"
#include "buffer.h"
#include "diskio.h"

#define FUSE_USE_VERSION 27
#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_lowlevel_compat.h>

#define include_inode_c
#include "inode.c"

static fd_t fd;
static u64 volsize;
static struct sb *sb;
static struct dev *dev;

static struct inode *ino2inode(fuse_ino_t ino)
{
	if (ino == 1)
		return sb->rootdir;
	return (struct inode *)ino;
}

static void tux3_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct inode *inode = tuxopen(ino2inode(parent), name, strlen(name));

	if (inode)
	{
		struct fuse_entry_param ep = {
			.attr.st_mode  = inode->i_mode,
			.attr.st_atime = inode->i_atime,
			.attr.st_mtime = inode->i_mtime,
			.attr.st_ctime = inode->i_ctime,
			.attr.st_size  = inode->i_size,
			.attr.st_uid   = inode->i_uid,
			.attr.st_gid   = inode->i_gid,
			.attr.st_nlink = inode->i_links,

			.ino = (fuse_ino_t)inode,
			.generation = 1,
			.attr_timeout = 1.0,
			.entry_timeout = 1.0,
		};

		fuse_reply_entry(req, &ep);
	} else {
		fuse_reply_err(req, ENOENT);
	}
}

static void tux3_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	printf("---- open file ----\n");
	printf("flags: %i\n", fi->flags);
	fi->flags |= 0666;
	fuse_reply_open(req, fi);
}

static void tux3_read(fuse_req_t req, fuse_ino_t ino, size_t size,
	off_t offset, struct fuse_file_info *fi)
{
	printf("---- read file ----\n");
	struct inode *inode = ino2inode(ino);
	struct file *file = &(struct file){ .f_inode = inode };

	printf("userspace tries to seek to %Li\n", (L)offset);
	if (offset >= inode->i_size)
	{
		printf("EOF!\n");
		fuse_reply_err(req, EINVAL);
		return;
	}
	tuxseek(file, offset);

	char buf[size];
	int read = tuxread(file, buf, size);
	if (read < 0)
	{
		errno = -read;
		goto eek;
	}

	if (offset + read > inode->i_size)
	{
		fuse_reply_err(req, EINVAL);
		return;
	}

	fuse_reply_buf(req, buf, read);
	return;

eek:
	fprintf(stderr, "Eek! %s\n", strerror(errno));
	fuse_reply_err(req, errno);
}

static void tux3_create(fuse_req_t req, fuse_ino_t parent, const char *name,
	mode_t mode, struct fuse_file_info *fi)
{
	printf("---- create file ----\n");
	struct inode *inode = tuxcreate(ino2inode(parent), name, strlen(name),
		&(struct iattr){ .mode = mode | 0666 });
	if (inode)
	{
		struct fuse_entry_param fep = {
			.attr.st_mode  = inode->i_mode,
			.attr.st_atime = inode->i_atime,
			.attr.st_mtime = inode->i_mtime,
			.attr.st_ctime = inode->i_ctime,
			.attr.st_size  = inode->i_size,
			.attr.st_uid   = inode->i_uid,
			.attr.st_gid   = inode->i_gid,
			.attr.st_nlink = inode->i_links,

			.ino = (fuse_ino_t)inode,
			.generation = 1,
			.attr_timeout = 1.0,
			.entry_timeout = 1.0,
		};

		dump_attrs(inode);

		fuse_reply_create(req, &fep, fi);
	} else {
		fuse_reply_err(req, ENOMEM);
	}
}

static void tux3_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
	printf("---- create directory ----\n");
	struct inode *inode = tuxcreate(ino2inode(parent), name, strlen(name),
		&(struct iattr){ .mode = mode | 0666 });
	if (inode)
	{
		struct fuse_entry_param fep = {
			.ino = (fuse_ino_t)inode,
			.generation = 1,
			.attr.st_mode = mode | 0666,
			.attr_timeout = 1.0,
			.entry_timeout = 1.0,
		};

		fuse_reply_entry(req, &fep);
	} else {
		fuse_reply_err(req, ENOMEM);
	}
}

static void tux3_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
	size_t size, off_t offset, struct fuse_file_info *fi)
{
	struct inode *inode = ino2inode(ino);
	struct file *file = &(struct file){ .f_inode = inode };

	if (offset) {
		u64 seek = offset;
		printf("seek to %Li\n", (L)seek);
		tuxseek(file, seek);
	}

	int written = 0;
	if ((written = tuxwrite(file, buf, size)) < 0)
	{
		errno = -written;
		goto eek;
	}

	tuxsync(inode);
	if ((errno = -sync_super(sb)))
		goto eek;

	fuse_reply_write(req, written);
	return;
eek:
	fprintf(stderr, "Eek! %s\n", strerror(errno));
	fuse_reply_err(req, errno);
}

static void _tux3_getattr(struct inode *inode, struct stat *st)
{
	st->st_mode  = inode->i_mode;
	st->st_atime = inode->i_atime;
	st->st_mtime = inode->i_mtime;
	st->st_ctime = inode->i_ctime;
	st->st_size  = inode->i_size;
	st->st_uid   = inode->i_uid;
	st->st_gid   = inode->i_gid;
	st->st_nlink = inode->i_links;
}

static void tux3_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct stat st;
	_tux3_getattr(ino2inode(ino), &st);
	fuse_reply_attr(req, &st, 0.0);
}

struct fillstate { char *dirent; int done; };

int tux3_filler(void *info, char *name, unsigned namelen, loff_t offset, unsigned inode, unsigned type)
{
	struct fillstate *state = info;
	if (state->done || namelen > EXT2_NAME_LEN)
		return -EINVAL;
	printf("'%.*s'\n", namelen, name);
	memcpy(state->dirent, name, namelen);
	state->dirent[namelen] = 0;
	state->done = 1;
	return 0;
}

/* FIXME: this needs to be implemented properly. */
static void tux3_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset,
	struct fuse_file_info *fi)
{
	struct inode *inode = ino2inode(ino);
	struct file *dirfile = &(struct file){ .f_inode = inode, .f_pos = offset };
	char dirent[EXT2_NAME_LEN + 1];
	char buf[1024]; //XXX

	while (dirfile->f_pos < dirfile->f_inode->i_size) {
		if ((errno = -ext2_readdir(dirfile, &(struct fillstate){ .dirent = dirent }, tux3_filler)))
		{
			fuse_reply_err(req, errno);
			return;
		}

		struct stat st;
		struct inode *inode2 = tuxopen(inode, dirent, strlen(dirent));
		if (!inode2)
			continue;

		_tux3_getattr(inode2, &st);

		int len = fuse_dirent_size(strlen(dirent));
		fuse_add_direntry(req, buf, sizeof(buf), dirent, &st, dirfile->f_pos);
		fuse_reply_buf(req, buf, len);
		return;
	}

	fuse_reply_buf(req, NULL, 0);
}

static void tux3_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	printf("---- delete file ----\n");
	struct buffer *buffer;
	ext2_dirent *entry = ext2_find_entry(ino2inode(parent), name, strlen(name), &buffer);
	if (!entry)
		goto noent;
	struct inode inode = { .sb = sb, .inum = entry->inum };

	if ((errno = -open_inode(&inode)))
		goto eek;
	if ((errno = -tree_chop(&inode.btree, &(struct delete_info){ .key = 0 }, -1)))
		goto eek;
	if ((errno = -ext2_delete_entry(buffer, entry)))
		goto eek;

	fuse_reply_err(req, 0);
	return;
noent:
	errno = ENOENT;
eek:
	fprintf(stderr, "Eek! %s\n", strerror(errno));
	fuse_reply_err(req, errno);
}

static void tux3_init(void *data, struct fuse_conn_info *conn)
{
	const char *volname = data;
	if (!(fd = open(volname, O_RDWR, S_IRWXU)))
		error("volume %s not found", volname);

	volsize = 0;
	if (fdsize64(fd, &volsize))
		error("fdsize64 failed for '%s' (%s)", volname, strerror(errno));
	dev = malloc(sizeof(*dev));
	*dev = (struct dev){ fd, .bits = 12 };
	init_buffers(dev, 1<<20);
	sb = malloc(sizeof(*sb));
	*sb = (struct sb){
		.max_inodes_per_block = 64,
		.entries_per_node = 20,
		.devmap = new_map(dev, NULL),
		.blockbits = dev->bits,
		.blocksize = 1 << dev->bits,
		.blockmask = (1 << dev->bits) - 1,
		.volblocks = volsize >> dev->bits,
		.freeblocks = volsize >> dev->bits,
		.itable = (struct btree){ .sb = sb, .ops = &itable_ops,
			.entries_per_leaf = 1 << (dev->bits - 6) } };

	if ((errno = -load_sb(sb)))
		goto eek;
	if (!(sb->bitmap = new_inode(sb, 0)))
		goto eek;
	if (!(sb->rootdir = new_inode(sb, 0xd)))
		goto eek;
	if ((errno = -open_inode(sb->bitmap)))
		goto eek;
	if ((errno = -open_inode(sb->rootdir)))
		goto eek;
	return;
eek:
	fprintf(stderr, "Eek! %s\n", strerror(errno));
	exit(1);
}

/* Stub methods */
static void tux3_destroy(void *userdata)
{
}

static void tux3_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
	fuse_reply_none(req);
}

static void tux3_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
	int to_set, struct fuse_file_info *fi)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_readlink(fuse_req_t req, fuse_ino_t ino)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
	mode_t mode, dev_t rdev)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_link(fuse_req_t req, fuse_ino_t ino,
	fuse_ino_t newparent, const char *newname)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_symlink(fuse_req_t req, const char *link,
	fuse_ino_t parent, const char *name)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_rename(fuse_req_t req, fuse_ino_t parent,
	const char *name, fuse_ino_t newparent, const char *newname)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_statfs(fuse_req_t req, fuse_ino_t ino)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
	/* Allow all accesses, for now */
	fuse_reply_err(req, 0);
}

static void tux3_opendir(fuse_req_t req, fuse_ino_t ino,
	struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi); /* Success */
}

static void tux3_releasedir(fuse_req_t req, fuse_ino_t ino,
	struct fuse_file_info *fi)
{
	fuse_reply_err(req, 0); /* Success */
}

static void tux3_fsyncdir(fuse_req_t req, fuse_ino_t ino,
	int datasync, struct fuse_file_info *fi)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
	struct fuse_file_info *fi)
{
	fuse_reply_err(req, ENOSYS);
}
static void tux3_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
	const char *value, size_t size, int flags)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_getlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
	struct flock *lock)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_setlk(fuse_req_t req, fuse_ino_t ino,
	struct fuse_file_info *fi, struct flock *lock, int sleep)
{
	fuse_reply_err(req, ENOSYS);
}

static void tux3_bmap(fuse_req_t req, fuse_ino_t ino, size_t blocksize, uint64_t idx)
{
	fuse_reply_err(req, ENOSYS);
}

static struct fuse_lowlevel_ops tux3_ops = {
	.init = tux3_init,
	.destroy = tux3_destroy,
	.lookup = tux3_lookup,
	.forget = tux3_forget,
	.getattr = tux3_getattr,
	.setattr = tux3_setattr,
	.readlink = tux3_readlink,
	.mknod = tux3_mknod,
	.mkdir = tux3_mkdir,
	.rmdir = tux3_rmdir,
	.link = tux3_link,
	.symlink = tux3_symlink,
	.unlink = tux3_unlink,
	.rename = tux3_rename,
	.create = tux3_create,
	.open = tux3_open,
	.read = tux3_read,
	.write = tux3_write,
	.statfs = tux3_statfs,
	.access = tux3_access,
	.opendir = tux3_opendir,
	.readdir = tux3_readdir,
	.releasedir = tux3_releasedir,
	.fsyncdir = tux3_fsyncdir,
	.flush = tux3_flush,
	.release = tux3_release,
	.fsync = tux3_fsync,
	.setxattr = tux3_setxattr,
	.getxattr = tux3_getxattr,
	.listxattr = tux3_listxattr,
	.removexattr = tux3_removexattr,
	.getlk = tux3_getlk,
	.setlk = tux3_setlk,
	.bmap = tux3_bmap,
};

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc-1, argv+1);

	char *mountpoint;
	int foreground;
	int err = -1;

	if (argc < 3)
		error("usage: %s <volname> <mountpoint>", argv[0]);

	if (fuse_parse_cmdline(&args, &mountpoint, NULL, &foreground) != -1)
	{
		int fd = open(mountpoint, O_RDONLY);
		struct fuse_chan *fc = fuse_mount(mountpoint, &args);
		if (fc)
		{
			struct fuse_session *fs = fuse_lowlevel_new(&args,
				&tux3_ops,
				sizeof(tux3_ops),
				argv[1]);

			if (fs)
			{
				if (fuse_set_signal_handlers(fs) != -1)
				{
					fuse_session_add_chan(fs, fc);
					fuse_daemonize(foreground);
					fchdir(fd);
					close(fd);
					err = fuse_session_loop(fs);
					fuse_remove_signal_handlers(fs);
					fuse_session_remove_chan(fc);
				}

				fuse_session_destroy(fs);
			}

			fuse_unmount(mountpoint, fc);
		}
	}

	fuse_opt_free_args(&args);
	return err ? 1 : 0;
}
