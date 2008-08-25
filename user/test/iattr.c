/*
 * Inode table attributes
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Portions copyright (c) 2006-2008 Google Inc.
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include "hexdump.c"
#include "tux3.h"

enum {
	CTIME_OWNER_ATTR = 6,
	MTIME_SIZE_ATTR = 7,
	DATA_BTREE_ATTR = 9,
	LINK_COUNT_ATTR = 8,
};

unsigned atsize[16] = {
	[CTIME_OWNER_ATTR] = 18,
	[MTIME_SIZE_ATTR] = 14,
	[DATA_BTREE_ATTR] = 8,
	[LINK_COUNT_ATTR] = 4,
};

struct size_mtime_attr { u64 size:60, mtime:54; };
struct data_btree_attr { struct root root; };

struct iattrs {
	struct root root;
	u64 mtime, ctime, isize;
	u32 mode, uid, gid, links;
} iattrs;

void *decode16(SB, void *attrs, unsigned *val)
{
	*val = be_to_u16(*(be_u16 *)attrs);
	return attrs + sizeof(u16);
}

void *decode32(SB, void *attrs, unsigned *val)
{
	*val = be_to_u32(*(be_u32 *)attrs);
	return attrs + sizeof(u32);
}

void *decode64(SB, void *attrs, u64 *val)
{
	*val = be_to_u64(*(be_u64 *)attrs);
	return attrs + sizeof(u64);
}

void *decode48(SB, void *attrs, u64 *val)
{
	unsigned part1, part2;
	attrs = decode16(sb, attrs, &part1);
	attrs = decode32(sb, attrs, &part2);
	*val = (u64)part1 << 32 | part2;
	return attrs;
}

int decode_attrs(SB, void *attrs, unsigned size)
{
	printf("decode %u attr bytes\n", size);
	struct iattrs iattrs = { };
	void *limit = attrs + size;
	u64 v64;
	while (attrs < limit - 1) {
		unsigned head, kind, version;
		attrs = decode16(sb, attrs, &head);
		if ((version = head & 0xfff))
			continue;
		switch (kind = (head >> 12)) {
		case CTIME_OWNER_ATTR:
			attrs = decode48(sb, attrs, &iattrs.ctime);
			attrs = decode32(sb, attrs, &iattrs.mode);
			attrs = decode32(sb, attrs, &iattrs.uid);
			attrs = decode32(sb, attrs, &iattrs.gid);
			//printf("ctime = %Lx, mode = %x\n", (L)iattrs.ctime, iattrs.mode);
			//printf("uid = %x, gid = %x\n", iattrs.uid, iattrs.gid);
			break;
		case MTIME_SIZE_ATTR:
			attrs = decode64(sb, attrs - 2, &v64);
			attrs = decode64(sb, attrs, &iattrs.isize);
			iattrs.mtime = v64 & (-1ULL >> 16);
			//printf("mtime = %Lx, isize = %Lx\n", (L)iattrs.mtime, (L)iattrs.isize);
			break;
		case DATA_BTREE_ATTR:
			attrs = decode64(sb, attrs, &v64);
			iattrs.root = (struct root){
				.block = v64 & (-1ULL >> 16),
				.depth = v64 >> 48 };
			//printf("btree block = %Lx, depth = %u\n", (L)iattrs.root.block, iattrs.root.depth);
			break;
		case LINK_COUNT_ATTR:
			attrs = decode32(sb, attrs, &iattrs.links);
			//printf("links = %u\n", iattrs.links);
			break;
		default:
			warn("unknown attribute kind %i", kind);
			return 0;
		}
	}
	return 0;
}

int dump_attrs(SB, void *attrs, unsigned size)
{
	struct iattrs iattrs = { };
	void *limit = attrs + size;
	u64 v64;
	while (attrs < limit - 1) {
		unsigned head, kind, version;
		attrs = decode16(sb, attrs, &head);
		if ((version = head & 0xfff))
			continue;
		switch (kind = (head >> 12)) {
		case CTIME_OWNER_ATTR:
			attrs = decode48(sb, attrs, &iattrs.ctime);
			attrs = decode32(sb, attrs, &iattrs.mode);
			attrs = decode32(sb, attrs, &iattrs.uid);
			attrs = decode32(sb, attrs, &iattrs.gid);
			printf("ctime %Lx mode %x ", (L)iattrs.ctime, iattrs.mode);
			printf("uid %x gid %x ", iattrs.uid, iattrs.gid);
			break;
		case MTIME_SIZE_ATTR:
			attrs = decode64(sb, attrs - 2, &v64);
			attrs = decode64(sb, attrs, &iattrs.isize);
			iattrs.mtime = v64 & (-1ULL >> 16);
			printf("mtime %Lx isize %Lx ", (L)iattrs.mtime, (L)iattrs.isize);
			break;
		case DATA_BTREE_ATTR:
			attrs = decode64(sb, attrs, &v64);
			iattrs.root = (struct root){
				.block = v64 & (-1ULL >> 16),
				.depth = v64 >> 48 };
			printf("btree (block %Lx depth %u) ", (L)iattrs.root.block, iattrs.root.depth);
			break;
		case LINK_COUNT_ATTR:
			attrs = decode32(sb, attrs, &iattrs.links);
			printf("links %u ", iattrs.links);
			break;
		default:
			printf("<?%i?> ", kind);
			break;
		}
	}
	printf("(%u bytes)\n", size);
	return 0;
}

void *encode16(SB, void *attrs, unsigned val)
{
	*(be_u16 *)attrs = u16_to_be(val);
	return attrs + sizeof(u16);
}

void *encode32(SB, void *attrs, unsigned val)
{
	*(be_u32 *)attrs = u32_to_be(val);
	return attrs + sizeof(u32);
}

void *encode64(SB, void *attrs, u64 val)
{
	*(be_u64 *)attrs = u64_to_be(val);
	return attrs + sizeof(u64);
}

void *encode48(SB, void *attrs, u64 val)
{
	attrs = encode16(sb, attrs, val >> 32);
	return encode32(sb, attrs, val);
}

void *encode_kind(SB, void *attrs, unsigned kind)
{
	return encode16(sb, attrs, (kind << 12) | sb->version);
}

void *encode_btree(SB, void *attrs, struct root *root)
{
	attrs = encode_kind(sb, attrs, DATA_BTREE_ATTR);
	return encode64(sb, attrs, ((u64)root->depth) << 48 | root->block);
}

void *encode_msize(SB, void *attrs, u64 mtime, u64 isize)
{
	attrs = encode_kind(sb, attrs, MTIME_SIZE_ATTR);
	attrs = encode48(sb, attrs, mtime);
	return encode64(sb, attrs, isize);
}

void *encode_owner(SB, void *attrs, u64 ctime, u32 mode, u32 uid, u32 gid)
{
	attrs = encode_kind(sb, attrs, CTIME_OWNER_ATTR);
	attrs = encode48(sb, attrs, ctime);
	attrs = encode32(sb, attrs, mode);
	attrs = encode32(sb, attrs, uid);
	return encode32(sb, attrs, gid);
}

void *encode_links(SB, void *attrs, u32 links)
{
	attrs = encode_kind(sb, attrs, LINK_COUNT_ATTR);
	return encode32(sb, attrs, links);
}

unsigned howbig(u8 kind[], unsigned howmany)
{
	unsigned need = 0;
	for (int i = 0; i < howmany; i++)
		need += 2 + atsize[kind[i]];
	return need;
}

#ifndef main
#ifndef iattr_included_from_ileaf
int main(int argc, char *argv[])
{
	SB = &(struct sb){ .version = 0 };
	u8 alist[] = { DATA_BTREE_ATTR, MTIME_SIZE_ATTR, CTIME_OWNER_ATTR, LINK_COUNT_ATTR };
	printf("need %i attr bytes\n", howbig(alist, sizeof(alist)));
	char iattrs[1000] = { };
	char *attrs = iattrs;
	attrs = encode_owner(sb, attrs, 0xdeadfaced00d, 0x666, 0x12121212, 0x34343434);
	attrs = encode_btree(sb, attrs, &(struct root){ .block = 0xcaba1f00d, .depth = 3 });
	attrs = encode_msize(sb, attrs, 0xdec0debead, 0x123456789);
	attrs = encode_links(sb, attrs, 999);
	decode_attrs(sb, iattrs, attrs - iattrs);
	dump_attrs(sb, iattrs, attrs - iattrs);
	return 0;
}
#endif
#endif