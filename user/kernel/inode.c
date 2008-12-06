/*
 * Tux3 versioning filesystem in user space
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3.h"

#ifndef trace
#define trace trace_on
#endif

int store_attrs(struct inode *inode, struct cursor *cursor)
{
	unsigned size = encode_asize(tux_inode(inode)->present) + encode_xsize(inode);
	void *base = tree_expand(&tux_sb(inode->i_sb)->itable, tux_inode(inode)->inum, size, cursor);
	if (!base)
		return -ENOMEM; // what was the actual error???
	void *attr = encode_attrs(inode, base, size);
	attr = encode_xattrs(inode, attr, base + size - attr);
	assert(attr == base + size);
	mark_buffer_dirty(cursor_leafbuf(cursor));
	return 0;
}

/*
 * Inode table expansion algorithm
 *
 * First probe for the inode goal.  This retreives the rightmost leaf that
 * contains an inode less than or equal to the goal.  (We could in theory avoid
 * retrieving any leaf at all in some cases if we observe that the the goal must
 * fall into an unallocated gap between two index keys, for what that is worth.
 * Probably not very much.)
 *
 * If not at end then next key is greater than goal.  This block has the highest
 * ibase less than or equal to goal.  Ibase should be equal to btree key, so
 * assert.  Search block even if ibase is way too low.  If goal comes back equal
 * to next_key then there is no room to create more inodes in it, so advance to
 * the next block and repeat.
 *
 * Otherwise, expand the inum goal that came back.  If ibase was too low to
 * create the inode in that block then the low level split will fail and expand
 * will create a new inode table block with ibase at the goal.  We round the
 * goal down to some binary multiple in ileaf_split to reduce the chance of
 * creating inode table blocks with only a small number of inodes.  (Actually
 * we should only round down the split point, not the returned goal.)
 */

int make_inode(struct inode *inode, struct tux_iattr *iattr)
{
	struct sb *sb = tux_sb(inode->i_sb);
	int err = -ENOENT, depth = sb->itable.root.depth;
	struct cursor *cursor = alloc_cursor(depth + 2); /* +1 for now depth */
	if (!cursor)
		return -ENOMEM;

	if ((err = probe(&sb->itable, tux_inode(inode)->inum, cursor))) {
		free_cursor(cursor);
		return err;
	}
	struct buffer_head *leafbuf = cursor_leafbuf(cursor);
//	struct ileaf *leaf = to_ileaf(bufdata(leafbuf));

	trace("create inode 0x%Lx", (L)tux_inode(inode)->inum);
	assert(!tux_inode(inode)->btree.root.depth);
	inum_t inum = tux_inode(inode)->inum;
	assert(inum < next_key(cursor, depth));
	while (1) {
//		printf("find empty inode in [%Lx] base %Lx\n", (L)bufindex(leafbuf), (L)ibase(leaf));
		inum = find_empty_inode(&sb->itable, bufdata(leafbuf), inum);
		printf("result inum is %Lx, limit is %Lx\n", (L)inum, (L)next_key(cursor, depth));
		if (inum < next_key(cursor, depth))
			break;
		int more = advance(&sb->itable, cursor);
		if (more < 0) {
			err = more;
			goto errout;
		}
		printf("no more inode space here, advance %i\n", more);
		if (!more) {
			err = -ENOSPC;
			goto errout;
		}
	}

	inode->i_mode = iattr->mode;
	inode->i_uid = iattr->uid;
	inode->i_gid = iattr->gid;
	inode->i_mtime = inode->i_ctime = inode->i_atime = iattr->ctime;
	inode->i_nlink = 1;
	tux_inode(inode)->inum = inum;
	tux_inode(inode)->btree = new_btree(sb, &dtree_ops); // error???
	tux_inode(inode)->present = CTIME_SIZE_BIT|MODE_OWNER_BIT|DATA_BTREE_BIT;
	if ((err = store_attrs(inode, cursor)))
		goto errout;
	release_cursor(cursor);
	free_cursor(cursor);
	return 0;

errout:
	/* release_cursor() was already called at error point */
	free_cursor(cursor);
	warn("make_inode 0x%Lx failed (%d)", (L)tux_inode(inode)->inum, err);
	return err;
}

static int open_inode(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	int err, depth = sb->itable.root.depth;
	struct cursor *cursor = alloc_cursor(depth + 1);
	if (!cursor)
		return -ENOMEM;

	if ((err = probe(&sb->itable, tux_inode(inode)->inum, cursor))) {
		free_cursor(cursor);
		return err;
	}
	unsigned size;
	void *attrs = ileaf_lookup(&sb->itable, tux_inode(inode)->inum, bufdata(cursor_leafbuf(cursor)), &size);
	if (!attrs) {
		err = -ENOENT;
		goto eek;
	}
	trace("found inode 0x%Lx", (L)tux_inode(inode)->inum);
	//ileaf_dump(&sb->itable, bufdata(cursor[depth].buffer));
	//hexdump(attrs, size);
	unsigned xsize = decode_xsize(inode, attrs, size);
	err = -ENOMEM;
	if (!(tux_inode(inode)->xcache = new_xcache(xsize))) // !!! only do this when we hit an xattr !!!
		goto eek;
	decode_attrs(inode, attrs, size); // error???
	dump_attrs(inode);
	if (tux_inode(inode)->xcache)
		xcache_dump(inode);
	err = 0;
eek:
	release_cursor(cursor);
	free_cursor(cursor);
	return err;
}

int save_inode(struct inode *inode)
{
	trace("save inode 0x%Lx", (L)tux_inode(inode)->inum);
	struct sb *sb = tux_sb(inode->i_sb);
	int err, depth = sb->itable.root.depth;
	struct cursor *cursor = alloc_cursor(depth + 2); /* +1 for new depth */
	if (!cursor)
		return -ENOMEM;

	if ((err = probe(&sb->itable, tux_inode(inode)->inum, cursor))) {
		free_cursor(cursor);
		return err;
	}
	unsigned size;
	if (!(ileaf_lookup(&sb->itable, tux_inode(inode)->inum, bufdata(cursor_leafbuf(cursor)), &size)))
		return -EINVAL;
	err = store_attrs(inode, cursor);
	if (err)
		goto error;
	/* release_cursor() was already called at error point */
	release_cursor(cursor);
error:
	free_cursor(cursor);
	return err;
}

int purge_inum(struct btree *btree, inum_t inum)
{
	int err = -ENOENT, depth = btree->root.depth;
	struct cursor *cursor = alloc_cursor(depth + 1);
	if (!cursor)
		return -ENOMEM;

	if (!(err = probe(btree, inum, cursor))) {
		struct ileaf *ileaf = to_ileaf(bufdata(cursor_leafbuf(cursor)));
		err = ileaf_purge(btree, inum, ileaf);
		release_cursor(cursor);
	}
	free_cursor(cursor);
	return err;
}

#ifdef __KERNEL__
void tux3_clear_inode(struct inode *inode)
{
	if (tux_inode(inode)->xcache)
		kfree(tux_inode(inode)->xcache);
}

int tux3_write_inode(struct inode *inode, int do_sync)
{
	BUG_ON(tux_inode(inode)->inum == TUX_BITMAP_INO ||
	       tux_inode(inode)->inum == TUX_VTABLE_INO ||
	       tux_inode(inode)->inum == TUX_ATABLE_INO);
	return save_inode(inode);
}

static const struct file_operations tux_file_fops = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read	= generic_file_aio_read,
	.aio_write	= generic_file_aio_write,
//	.unlocked_ioctl	= fat_generic_ioctl,
#ifdef CONFIG_COMPAT
//	.compat_ioctl	= fat_compat_dir_ioctl,
#endif
	.mmap		= generic_file_mmap,
	.open		= generic_file_open,
//	.fsync		= file_fsync,
	.splice_read	= generic_file_splice_read,
	.splice_write	= generic_file_splice_write,
};

static const struct inode_operations tux_file_iops = {
//	.truncate	= ext4_truncate,
//	.permission	= ext4_permission,
//	.setattr	= ext4_setattr,
//	.getattr	= ext4_getattr
#ifdef CONFIG_EXT4DEV_FS_XATTR
//	.setxattr	= generic_setxattr,
//	.getxattr	= generic_getxattr,
//	.listxattr	= ext4_listxattr,
//	.removexattr	= generic_removexattr,
#endif
//	.fallocate	= ext4_fallocate,
//	.fiemap		= ext4_fiemap,
};

static void tux3_setup_inode(struct inode *inode)
{
	struct sb *sbi = tux_sb(inode->i_sb);

	/* FIXME: will overflow on 32bit arch */
	inode->i_ino = tux_inode(inode)->inum;
	inode->i_version = 1;
	inode->i_blocks = ((inode->i_size + sbi->blockmask)
			   & ~(loff_t)sbi->blockmask) >> 9;
//	inode->i_generation = 0;
//	inode->i_flags = 0;

	switch (inode->i_mode & S_IFMT) {
	default:
//		inode->i_op = &tux3_special_inode_operations;
//		init_special_inode(inode, inode->i_mode, new_decode_dev(dev));
		break;
	case S_IFREG:
		inode->i_op = &tux_file_iops;
		inode->i_fop = &tux_file_fops;
		inode->i_mapping->a_ops = &tux_aops;
		break;
	case S_IFDIR:
		inode->i_op = &tux_dir_iops;
		inode->i_fop = &tux_dir_fops;
		inode->i_mapping->a_ops = &tux_blk_aops;
//		mapping_set_gfp_mask(inode->i_mapping, GFP_USER_PAGECACHE);
		mapping_set_gfp_mask(inode->i_mapping, GFP_USER);
		break;
	case S_IFLNK:
//		inode->i_op = &tux_symlink_iops;
//		inode->i_mapping->a_ops = &tux_aops;
		break;
	case 0:
		/* FIXME: bitmap, vtable, atable doesn't have S_IFMT */
		/* set fake i_size to escape the check of read/writepage */
		inode->i_size = MAX_LFS_FILESIZE;
		inode->i_mapping->a_ops = &tux_blk_aops;
//		mapping_set_gfp_mask(inode->i_mapping, GFP_USER_PAGECACHE);
		mapping_set_gfp_mask(inode->i_mapping, GFP_USER);
		break;
	}
}

struct inode *tux_create_inode(struct inode *dir, int mode)
{
	struct inode *inode = new_inode(dir->i_sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	tux_inode(inode)->inum = tux_sb(dir->i_sb)->nextalloc;

	struct tux_iattr iattr;
	iattr.mode = mode;
	iattr.uid = current->fsuid;
	if (dir->i_mode & S_ISGID) {
		iattr.gid = dir->i_gid;
		if (S_ISDIR(iattr.mode))
			iattr.mode |= S_ISGID;
	} else
		iattr.gid = current->fsgid;
	iattr.ctime = iattr.mtime = iattr.atime = gettime();

	int err = make_inode(inode, &iattr);
	if (err)
		return ERR_PTR(err);
	tux3_setup_inode(inode);
	insert_inode_hash(inode);
	return inode;
}

struct inode *tux3_iget(struct super_block *sb, inum_t inum)
{
	struct inode *inode;
	int err;

	/* FIXME: inum is 64bit, ino is unsigned long */
	inode = iget_locked(sb, inum);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	tux_inode(inode)->inum = inum;
	err = open_inode(inode);
	if (err) {
		iget_failed(inode);
		return ERR_PTR(err);
	}

	tux3_setup_inode(inode);

	unlock_new_inode(inode);
	return inode;
}
#endif /* !__KERNEL__ */
