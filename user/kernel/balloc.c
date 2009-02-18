/*
 * Block allocation
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Portions copyright (c) 2006-2008 Google Inc.
 * Licensed under the GPL version 2
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3.h"

#ifndef trace
#define trace trace_off
#endif

/* For lockdep: random value bigger than max of inode_i_mutex_lock_class */
#define I_MUTEX_BITMAP	7

static void set_bits(u8 *bitmap, unsigned start, unsigned count)
{
	unsigned limit = start + count;
	unsigned lmask = (-1 << (start & 7)) & 0xff; // little endian!!!
	unsigned rmask = ~(-1 << (limit & 7)) & 0xff; // little endian!!!
	unsigned loff = start >> 3, roff = limit >> 3;
	if (loff == roff) {
		bitmap[loff] |= lmask & rmask;
		return;
	}
	bitmap[loff] |= lmask;
	memset(bitmap + loff + 1, -1, roff - loff - 1);
	if (rmask)
		bitmap[roff] |= rmask;
}

static void clear_bits(u8 *bitmap, unsigned start, unsigned count)
{
	unsigned limit = start + count;
	unsigned lmask = (-1 << (start & 7)) & 0xff; // little endian!!!
	unsigned rmask = ~(-1 << (limit & 7)) & 0xff; // little endian!!!
	unsigned loff = start >> 3, roff = limit >> 3;
	if (loff == roff) {
		bitmap[loff] &= ~lmask | ~rmask;
		return;
	}
	bitmap[loff] &= ~lmask;
	memset(bitmap + loff + 1, 0, roff - loff - 1);
	if (rmask)
		bitmap[roff] &= ~rmask;
}

static int all_set(u8 *bitmap, unsigned start, unsigned count)
{
	unsigned limit = start + count;
	unsigned lmask = (-1 << (start & 7)) & 0xff; // little endian!!!
	unsigned rmask = ~(-1 << (limit & 7)) & 0xff; // little endian!!!
	unsigned loff = start >> 3, roff = limit >> 3;
	if (loff == roff) {
		unsigned mask = lmask & rmask;
		return (bitmap[loff] & mask) == mask;
	}
	for (unsigned i = loff + 1; i < roff; i++)
		if (bitmap[i] != 0xff)
			return 0;
	return	(bitmap[loff] & lmask) == lmask &&
		(!rmask || (bitmap[roff] & rmask) == rmask);
}

static int all_clear(u8 *bitmap, unsigned start, unsigned count) // untested
{
	unsigned limit = start + count;
	unsigned lmask = (-1 << (start & 7)) & 0xff; // little endian!!!
	unsigned rmask = ~(-1 << (limit & 7)) & 0xff; // little endian!!!
	unsigned loff = start >> 3, roff = limit >> 3;
	if (loff == roff) {
		unsigned mask = lmask & rmask;
		return !(bitmap[loff] & mask);
	}
	for (unsigned i = loff + 1; i < roff; i++)
		if (bitmap[i])
			return 0;
	return	!(bitmap[loff] & lmask) &&
		(!rmask || !(bitmap[roff] & rmask));
}

static int bytebits(unsigned char c)
{
	unsigned count = 0;
	for (; c; c >>= 1)
		count += c & 1;
	return count;
}

block_t count_range(struct inode *inode, block_t start, block_t count)
{
	assert(!(start & 7));
	unsigned char ones[256];
	for (int i = 0; i < sizeof(ones); i++)
		ones[i] = bytebits(i);

	block_t limit = start + count;
	unsigned blocksize = 1 << tux_sb(inode->i_sb)->blockbits;
	unsigned mapshift = tux_sb(inode->i_sb)->blockbits + 3;
	unsigned mapmask = (1 << mapshift) - 1;
	unsigned blocks = (limit + mapmask) >> mapshift;
	unsigned offset = (start & mapmask) >> 3;
	block_t tail = (count + 7) >> 3, total = 0;

	for (unsigned block = start >> mapshift; block < blocks; block++) {
		//trace("count block %x/%x\n", block, blocks);
		struct buffer_head *buffer = blockread(mapping(inode), block);
		if (!buffer)
			return -1;
		unsigned bytes = blocksize - offset;
		if (bytes > tail)
			bytes = tail;
		unsigned char *p = bufdata(buffer) + offset, *top = p + bytes;
		while (p < top)
			total += ones[*p++];
		brelse(buffer);
		tail -= bytes;
		offset = 0;
	}
	return total;
}

/* userland only */
block_t bitmap_dump(struct inode *inode, block_t start, block_t count)
{
	block_t limit = start + count;
	unsigned blocksize = 1 << tux_sb(inode->i_sb)->blockbits;
	unsigned mapshift = tux_sb(inode->i_sb)->blockbits + 3;
	unsigned mapmask = (1 << mapshift) - 1;
	unsigned blocks = (limit + mapmask) >> mapshift, active = 0;
	unsigned offset = (start & mapmask) >> 3;
	unsigned startbit = start & 7;
	block_t tail = (count + startbit + 7) >> 3, begin = -1;

	printf("%i bitmap blocks:\n", blocks);
	for (unsigned block = start >> mapshift; block < blocks; block++) {
		int ended = 0, any = 0;
		struct buffer_head *buffer = blockread(mapping(inode), block);
		if (!buffer)
			return -1;
		unsigned bytes = blocksize - offset;
		if (bytes > tail)
			bytes = tail;
		unsigned char *p = bufdata(buffer) + offset, *top = p + bytes;
		for (; p < top; p++, startbit = 0) {
			unsigned c = *p;
			if (!any && c)
				printf("[%x] ", block);
			any |= c;
			if ((!c && begin < 0) || (c == 0xff && begin >= 0))
				continue;
			for (int i = startbit, mask = 1 << startbit; i < 8; i++, mask <<= 1) {
				if (!(c & mask) == (begin < 0))
					continue;
				block_t found = i + (((void *)p - bufdata(buffer)) << 3) + (block << mapshift);
				if (begin < 0)
					begin = found;
				else {
					if ((begin >> mapshift) != block)
						printf("-%Lx ", (L)(found - 1));
					else if (begin == found - 1)
						printf("%Lx ", (L)begin);
					else
						printf("%Lx-%Lx ", (L)begin, (L)(found - 1));
					begin = -1;
					ended++;
				}
			}
		}
		active += !!any;
		brelse(buffer);
		tail -= bytes;
		offset = 0;
		if (begin >= 0)
			printf("%Lx-", (L)begin);
		if (any)
			printf("\n");
	}
	printf("(%i active)\n", active);
	return -1;
}

static block_t balloc_from_range(struct sb *sb, block_t start, unsigned count, unsigned blocks)
{
	struct inode *inode = sb->bitmap;
	trace_off("balloc %i blocks from [%Lx/%Lx]", blocks, (L)start, (L)count);
	assert(blocks > 0);
	block_t limit = start + count;
	unsigned blocksize = 1 << sb->blockbits;
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapmask = (1 << mapshift) - 1;
	unsigned mapblocks = (limit + mapmask) >> mapshift;
	unsigned offset = (start & mapmask) >> 3;
	unsigned startbit = start & 7;
	block_t tail = (count + startbit + 7) >> 3;
	for (unsigned mapblock = start >> mapshift; mapblock < mapblocks; mapblock++) {
		trace_off("search mapblock %x/%x", mapblock, mapblocks);
		struct buffer_head *buffer = blockread(mapping(inode), mapblock);
		if (!buffer) {
			warn("block read failed"); // !!! error return sucks here
			return -1;
		}
		mutex_lock_nested(&sb->bitmap->i_mutex, I_MUTEX_BITMAP);
		unsigned bytes = blocksize - offset, run = 0;
		if (bytes > tail)
			bytes = tail;
		unsigned char *p = bufdata(buffer) + offset, *top = p + bytes, c;
		for (; p < top; p++, startbit = 0) {
			if ((c = *p) == 0xff) {
				run = 0;
				continue;
			}
			for (int i = startbit, mask = 1 << startbit; i < 8; i++, mask <<= 1) {
				if ((c & mask)) {
					run = 0;
					continue;
				}
				if (++run < blocks)
					continue;
				assert(run == blocks);
				block_t found = i + (((void *)p - bufdata(buffer)) << 3) + (mapblock << mapshift);
				if (found >= limit) {
					assert(mapblock == mapblocks - 1);
					goto final_partial_byte;
				}
				found -= run - 1;
				buffer = blockdirty(buffer, sb->flush);
				// FIXME: error check of buffer
				set_bits(bufdata(buffer), found & mapmask, run);
				mark_buffer_dirty(buffer);
				brelse(buffer);
				sb->nextalloc = found + run;
				sb->freeblocks -= run;
				//set_sb_dirty(sb);
				mutex_unlock(&sb->bitmap->i_mutex);
				return found;
			}
		}
final_partial_byte:
		mutex_unlock(&sb->bitmap->i_mutex);
		brelse(buffer);
		tail -= bytes;
		offset = 0;
	}
	return -1;
}

int balloc(struct sb *sb, unsigned blocks, block_t *block)
{
	assert(blocks > 0);
	trace_off("balloc %x blocks at goal %Lx", blocks, (L)sb->nextalloc);
	block_t goal = sb->nextalloc, total = sb->volblocks;
	if ((*block = balloc_from_range(sb, goal, total - goal, blocks)) >= 0)
		goto found;
	if ((*block = balloc_from_range(sb, 0, goal, blocks)) >= 0)
		goto found;
	return -ENOSPC;
found:
	trace("balloc extent -> [%Lx/%x]\n", (L)*block, blocks);
	return 0;
}

int bfree(struct sb *sb, block_t start, unsigned blocks)
{
	assert(blocks > 0);
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapmask = (1 << mapshift) - 1;
	unsigned mapblock = start >> mapshift;
	char *why = "could not read bitmap buffer";
	struct buffer_head *buffer = blockread(mapping(sb->bitmap), mapblock);
	trace("free <- [%Lx]\n", (L)start);
	if (!buffer)
		goto eek;
	mutex_lock_nested(&sb->bitmap->i_mutex, I_MUTEX_BITMAP);
	if (!all_set(bufdata(buffer), start &= mapmask, blocks))
		goto eeek;
	buffer = blockdirty(buffer, sb->flush);
	// FIXME: error check of buffer
	clear_bits(bufdata(buffer), start, blocks);
	mark_buffer_dirty(buffer);
	brelse(buffer);
	sb->freeblocks += blocks;
	//set_sb_dirty(sb);
	mutex_unlock(&sb->bitmap->i_mutex);
	return 0;
eeek:
	why = "blocks already free";
	mutex_unlock(&sb->bitmap->i_mutex);
	brelse(buffer);
eek:
	warn("extent 0x%Lx %s!\n", (L)start, why);
	return -1; // error???
}

int update_bitmap(struct sb *sb, block_t start, unsigned count, int set)
{
	unsigned shift = sb->blockbits + 3, mask = (1 << shift) - 1;
	struct buffer_head *buffer = blockread(mapping(sb->bitmap), start >> shift);
	if (!buffer)
		return -ENOMEM;
	if (!(set ? all_clear : all_set)(bufdata(buffer), start & mask, count)) {
		brelse(buffer);
		return -EINVAL;
	}
	(set ? set_bits : clear_bits)(bufdata(buffer), start & mask, count);
	sb->freeblocks += set ? count : -count;
	brelse_dirty(buffer);
	return 0;
}
