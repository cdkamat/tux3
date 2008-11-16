/*
 * Generic btree operations
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
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "diskio.h"
#include "tux3.h"

#ifndef trace
#define trace trace_off
#endif

struct bnode
{
	be_u32 count, unused;
	struct index_entry { be_u64 key; be_u64 block; } PACKED entries[];
} PACKED;
/*
 * Note that the first key of an index block is never accessed.  This is
 * because for a btree, there is always one more key than nodes in each
 * index node.  In other words, keys lie between node pointers.  I
 * micro-optimize by placing the node count in the first key, which allows
 * a node to contain an esthetically pleasing binary number of pointers.
 * (Not done yet.)
 */

static inline unsigned bcount(struct bnode *node)
{
	return from_be_u32(node->count);
}

static void free_block(SB, block_t block)
{
}

static struct buffer *new_block(struct btree *btree)
{
	block_t block = (btree->ops->balloc)(btree->sb);
	if (block == -1)
		return NULL;
	struct buffer *buffer = blockget(btree->sb->devmap, block);
	if (!buffer)
		return NULL;
	memset(buffer->data, 0, bufsize(buffer));
	set_buffer_dirty(buffer);
	return buffer;
}

static struct buffer *new_leaf(struct btree *btree)
{
	struct buffer *buffer = new_block(btree);
	if (buffer)
		(btree->ops->leaf_init)(btree, buffer->data);
	return buffer;
}

static struct buffer *new_node(struct btree *btree)
{
	struct buffer *buffer = new_block(btree);
	if (buffer)
		((struct bnode *)buffer->data)->count = 0;
	return buffer;
}

struct path { struct buffer *buffer; struct index_entry *next; };
/*
 * A btree path has n + 1 entries for a btree of depth n, with the first n
 * entries pointing at internal nodes and entry n + 1 pointing at a leaf.
 * The next field points at the next index entry that will be loaded in a left
 * to right tree traversal, not the current entry.  The next pointer is null
 * for the leaf, which has its own specialized traversal algorithms.
 */

static inline struct bnode *path_node(struct path path[], int level)
{
	return (struct bnode *)path[level].buffer->data;
}

static void release_path(struct path *path, int levels)
{
	for (int i = 0; i < levels; i++)
		brelse(path[i].buffer);
}

void show_path(struct path *path, int levels)
{
	printf(">>> path %p/%i:", path, levels);
	for (int i = 0; i < levels; i++)
		printf(" [%Lx/%i]", (L)path[i].buffer->index, path[i].buffer->count);
	printf("\n");
}

static struct path *alloc_path(int levels)
{
	return malloc(sizeof(struct path) * levels);
}

static void free_path(struct path *path)
{
	free(path);
}

static int probe(BTREE, tuxkey_t key, struct path *path)
{
	unsigned i, levels = btree->root.depth;
	struct buffer *buffer = blockread(btree->sb->devmap, btree->root.block);
	if (!buffer)
		return -EIO;
	struct bnode *node = buffer->data;

	for (i = 0; i < levels; i++) {
		struct index_entry *next = node->entries, *top = next + bcount(node);
		while (++next < top) /* binary search goes here */
			if (from_be_u64(next->key) > key)
				break;
		//printf("probe level %i, %ti of %i\n", i, next - node->entries, bcount(node));
		path[i] = (struct path){ buffer, next };
		if (!(buffer = blockread(btree->sb->devmap, from_be_u64((next - 1)->block))))
			goto eek;
		node = (struct bnode *)buffer->data;
	}
	assert((btree->ops->leaf_sniff)(btree, buffer->data));
	path[levels] = (struct path){ buffer };
	return 0;
eek:
	release_path(path, i - 1);
	return -EIO; /* stupid, it might have been NOMEM */
}

static inline int level_finished(struct path path[], int level)
{
	struct bnode *node = path_node(path, level);
	return path[level].next == node->entries + bcount(node);
}
// also write level_beginning!!!

int advance(struct map *map, struct path *path, int levels)
{
	int level = levels;
	struct buffer *buffer = path[level].buffer;
	struct bnode *node;
	do {
		brelse(buffer);
		if (!level)
			return 0;
		node = (buffer = path[--level].buffer)->data;
		//printf("pop to level %i, %tx of %x\n", level, path[level].next - node->entries, bcount(node));
	} while (level_finished(path, level));
	do {
		//printf("push from level %i, %tx of %x\n", level, path[level].next - node->entries, bcount(node));
		if (!(buffer = blockread(map, from_be_u64(path[level].next++->block))))
			goto eek;
		path[++level] = (struct path){ .buffer = buffer, .next = (node = buffer->data)->entries };
	} while (level < levels);
	return 1;
eek:
	release_path(path, level);
	return -EIO;
}

/*
 * Climb up the path until we find the first level where we have not yet read
 * all the way to the end of the index block, there we find the key that
 * separates the subtree we are in (a leaf) from the next subtree to the right.
 */
be_u64 *next_keyp(struct path *path, int levels)
{
	for (int level = levels; level--;)
		if (!level_finished(path, level))
			return &path[level].next->key;
	return NULL;
}

tuxkey_t next_key(struct path *path, int levels)
{
	be_u64 *keyp = next_keyp(path, levels);
	return keyp ? from_be_u64(*keyp) : -1;
}
// also write this_key!!!

void show_tree_range(BTREE, tuxkey_t start, unsigned count)
{
	printf("%i level btree at %Li:\n", btree->root.depth, (L)btree->root.block);
	struct path path[30]; // check for overflow!!!
	if (probe(btree, start, path))
		error("tell me why!!!");
	struct buffer *buffer;
	do {
		buffer = path[btree->root.depth].buffer;
		assert((btree->ops->leaf_sniff)(btree, buffer->data));
		(btree->ops->leaf_dump)(btree, buffer->data);
		//tuxkey_t *next = pnext_key(path, btree->levels);
		//printf("next key = %Lx:\n", next ? (L)*next : 0);
	} while (--count && advance(buffer->map, path, btree->root.depth));
}

/* Deletion */

static void brelse_free(SB, struct buffer *buffer)
{
	brelse(buffer);
	if (buffer->count) {
		warn("free block %Lx still in use!", (long long)buffer->index);
		return;
	}
	free_block(sb, buffer->index);
	set_buffer_empty(buffer); // free it!!! (and need a buffer free state)
}

static void remove_index(struct path path[], int level)
{
	struct bnode *node = path_node(path, level);
	int count = bcount(node), i;

	/* stomps the node count (if 0th key holds count) */
	memmove(path[level].next - 1, path[level].next,
		(char *)&node->entries[count] - (char *)path[level].next);
	node->count = to_be_u32(count - 1);
	--(path[level].next);
	set_buffer_dirty(path[level].buffer);

	/* no separator for last entry */
	if (level_finished(path, level))
		return;
	/*
	 * Climb up to common parent and set separating key to deleted key.
	 * What if index is now empty?  (no deleted key)
	 * Then some key above is going to be deleted and used to set sep
	 * Climb the path while at first entry, bail out at root
	 * find the node with the old sep, set it to deleted key
	 */
	if (path[level].next == node->entries && level) {
		be_u64 sep = (path[level].next)->key;
		for (i = level - 1; path[i].next - 1 == path_node(path, i)->entries; i--)
			if (!i)
				return;
		(path[i].next - 1)->key = sep;
		set_buffer_dirty(path[i].buffer);
	}
}

static void merge_nodes(struct bnode *node, struct bnode *node2)
{
	memcpy(&node->entries[bcount(node)], &node2->entries[0], bcount(node2) * sizeof(struct index_entry));
	node->count = to_be_u32(bcount(node) + bcount(node2));
}

struct delete_info { tuxkey_t key; block_t blocks, freed; block_t resume; int create; };

int delete_from_leaf(BTREE, vleaf *leaf, struct delete_info *info)
{
	(btree->ops->leaf_chop)(btree, info->key, leaf);
	return 0;
}

int tree_chop(BTREE, struct delete_info *info, millisecond_t deadline)
{
	int levels = btree->root.depth, level = levels - 1, suspend = 0;
	struct path *path, *prev;
	struct buffer *leafbuf, *leafprev = NULL;
	struct btree_ops *ops = btree->ops;
	struct sb *sb = btree->sb;

	path = alloc_path(levels + 1);
	prev = alloc_path(levels + 1);
	memset(prev, 0, sizeof(struct path) * (levels + 1));

	probe(btree, info->resume, path);
	leafbuf = path[levels].buffer;

	/* leaf walk */
	while (1) {
		if (delete_from_leaf(btree, leafbuf->data, info))
			set_buffer_dirty(leafbuf);

		/* try to merge this leaf with prev */
		if (leafprev) {
			struct vleaf *this = leafbuf->data;
			struct vleaf *that = leafprev->data;
			trace_off("check leaf %p against %p", leafbuf, leafprev);
			trace_off("need = %i, free = %i", (ops->dleaf_need)(btree, this), dleaf_free(sb, that));
			/* try to merge leaf with prev */
			if ((ops->leaf_need)(btree, this) <= (ops->leaf_free)(btree, that)) {
				trace(">>> can merge leaf %p into leaf %p", leafbuf, leafprev);
				(ops->leaf_merge)(btree, that, this);
				remove_index(path, level);
				set_buffer_dirty(leafprev);
				brelse_free(sb, leafbuf);
				//dirty_buffer_count_check(sb);
				goto keep_prev_leaf;
			}
			brelse(leafprev);
		}
		leafprev = leafbuf;
keep_prev_leaf:

		//nanosleep(&(struct timespec){ 0, 50 * 1000000 }, NULL);
		//printf("time remaining: %Lx\n", deadline - gettime());
//		if (deadline && gettime() > deadline)
//			suspend = -1;
		if (info->blocks && info->freed >= info->blocks)
			suspend = -1;

		/* pop and try to merge finished nodes */
		while (suspend || level_finished(path, level)) {
			/* try to merge node with prev */
			if (prev[level].buffer) {
				assert(level); /* node has no prev */
				struct bnode *this = path_node(path, level);
				struct bnode *that = path_node(prev, level);
				trace_off("check node %p against %p", this, that);
				trace_off("this count = %i prev count = %i", bcount(this), bcount(that));
				/* try to merge with node to left */
				if (bcount(this) <= sb->entries_per_node - bcount(that)) {
					trace(">>> can merge node %p into node %p", this, that);
					merge_nodes(that, this);
					remove_index(path, level - 1);
					set_buffer_dirty(prev[level].buffer);
					brelse_free(sb, path[level].buffer);
					//dirty_buffer_count_check(sb);
					goto keep_prev_node;
				}
				brelse(prev[level].buffer);
			}
			prev[level].buffer = path[level].buffer;
keep_prev_node:

			/* deepest key in the path is the resume address */
			if (suspend == -1 && !level_finished(path, level)) {
				suspend = 1; /* only set resume once */
				info->resume = from_be_u64((path[level].next)->key);
			}
			if (!level) { /* remove levels if possible */
				while (levels > 1 && bcount(path_node(prev, 0)) == 1) {
					trace("drop btree level");
					btree->root.block = prev[1].buffer->index;
					brelse_free(sb, prev[0].buffer);
					//dirty_buffer_count_check(sb);
					levels = --btree->root.depth;
					memcpy(prev, prev + 1, levels * sizeof(prev[0]));
					//set_sb_dirty(sb);
				}
				brelse(leafprev);
				release_path(prev, levels);
				free_path(path);
				free_path(prev);
				//sb->snapmask &= ~snapmask; delete_snapshot_from_disk();
				//set_sb_dirty(sb);
				//save_sb(sb);
				return suspend;
			}
			level--;
			trace_off(printf("pop to level %i, block %Lx, %i of %i nodes\n", level, path[level].buffer->index, path[level].next - path_node(path, level)->entries, bcount(path_node(path, level))););
		}

		/* push back down to leaf level */
		while (level < levels - 1) {
			struct buffer *buffer = blockread(sb->devmap, from_be_u64(path[level++].next++->block));
			if (!buffer) {
				brelse(leafprev);
				release_path(path, level - 1);
				free_path(path);
				free_path(prev);
				return -ENOMEM;
			}
			path[level].buffer = buffer;
			path[level].next = ((struct bnode *)buffer->data)->entries;
			trace_off(printf("push to level %i, block %Lx, %i nodes\n", level, buffer->index, bcount(path_node(path, level))););
		};
		//dirty_buffer_count_check(sb);
		/* go to next leaf */
		if (!(leafbuf = blockread(sb->devmap, from_be_u64(path[level].next++->block)))) {
			release_path(path, level);
			free_path(path);
			free_path(prev);
			return -ENOMEM;
		}
	}
}

/* Insertion */

static void add_child(struct bnode *node, struct index_entry *p, block_t child, u64 childkey)
{
	vecmove(p + 1, p, node->entries + bcount(node) - p);
	p->block = to_be_u64(child);
	p->key = to_be_u64(childkey);
	node->count = to_be_u32(bcount(node) + 1);
}

int insert_node(struct btree *btree, u64 childkey, block_t childblock, struct path path[])
{
	trace("insert node 0x%Lx key 0x%Lx into node 0x%Lx", (L)childblock, (L)childkey, (L)btree->root.block);
	int levels = btree->root.depth;
	while (levels--) {
		struct index_entry *next = path[levels].next;
		struct buffer *parentbuf = path[levels].buffer;
		struct bnode *parent = parentbuf->data;

		/* insert and exit if not full */
		if (bcount(parent) < btree->sb->entries_per_node) {
			add_child(parent, next, childblock, childkey);
			set_buffer_dirty(parentbuf);
			return 0;
		}

		/* split a full index node */
		struct buffer *newbuf = new_node(btree);
		if (!newbuf)
			goto eek;
		struct bnode *newnode = newbuf->data;
		unsigned half = bcount(parent) / 2;
		u64 newkey = from_be_u64(parent->entries[half].key);
		newnode->count = to_be_u32(bcount(parent) - half);
		memcpy(&newnode->entries[0], &parent->entries[half], bcount(newnode) * sizeof(struct index_entry));
		parent->count = to_be_u32(half);

		/* if the path is in the new node, use that as the parent */
		if (next > parent->entries + half) {
			next = next - &parent->entries[half] + newnode->entries;
			set_buffer_dirty(parentbuf);
			parentbuf = newbuf;
			parent = newnode;
		} else set_buffer_dirty(newbuf);
		add_child(parent, next, childblock, childkey);
		set_buffer_dirty(parentbuf);
		childkey = newkey;
		childblock = newbuf->index;
		brelse(newbuf);
	}
	trace("add tree level");
	struct buffer *newbuf = new_node(btree);
	if (!newbuf)
		goto eek;
	struct bnode *newroot = newbuf->data;
	newroot->count = to_be_u32(2);
	newroot->entries[0].block = to_be_u64(btree->root.block);
	newroot->entries[1].key = to_be_u64(childkey);
	newroot->entries[1].block = to_be_u64(childblock);
	btree->root.block = newbuf->index;
	vecmove(path + 1, path, btree->root.depth++ + 1);
	path[0] = (struct path){ .buffer = newbuf }; // .next = ???
	//set_sb_dirty(sb);
	set_buffer_dirty(newbuf);
	return 0;
eek:
	release_path(path, levels + 1);
	return -ENOMEM;
}

int btree_leaf_split(struct btree *btree, struct path path[], tuxkey_t key)
{
	trace("split leaf");
	struct buffer *leafbuf = path[btree->root.depth].buffer;
	struct buffer *newbuf = new_leaf(btree);
	if (!newbuf) {
		/* the rule: release path at point of error */
		release_path(path, btree->root.depth);
		return -ENOMEM;
	}
	u64 newkey = (btree->ops->leaf_split)(btree, key, leafbuf->data, newbuf->data);
	block_t childblock = newbuf->index;
	trace_off("use upper? %Li %Li", key, newkey);
	if (key >= newkey) {
		struct buffer *swap = leafbuf;
		leafbuf = path[btree->root.depth].buffer = newbuf;
		newbuf = swap;
	}
	set_buffer_dirty(newbuf);
	brelse(newbuf);
	return insert_node(btree, newkey, childblock, path);
}

void *tree_expand(struct btree *btree, tuxkey_t key, unsigned newsize, struct path path[])
{
	for (int i = 0; i < 2; i++) {
		struct buffer *leafbuf = path[btree->root.depth].buffer;
		void *space = (btree->ops->leaf_resize)(btree, key, leafbuf->data, newsize);
		set_buffer_dirty(leafbuf);
		if (space)
			return space;
		assert(!i);
		int err = btree_leaf_split(btree, path, key);
		if (err) {
			warn("insert_node failed (%s)", strerror(-err));
			break;
		}
	}
	return NULL;
}

struct btree new_btree(SB, struct btree_ops *ops)
{
	struct btree btree = { .sb = sb, .ops = ops };
	struct buffer *rootbuf = new_node(&btree);
	struct buffer *leafbuf = new_leaf(&btree);
	if (!rootbuf || !leafbuf)
		goto eek;
	struct bnode *root = rootbuf->data;
	root->entries[0].block = to_be_u64(leafbuf->index);
	root->count = to_be_u32(1);
	btree.root = (struct root){ .block = rootbuf->index, .depth = 1 };
	printf("root at %Lx\n", (L)rootbuf->index);
	printf("leaf at %Lx\n", (L)leafbuf->index);
	brelse_dirty(rootbuf);
	brelse_dirty(leafbuf);
	return btree;
eek:
	if (rootbuf)
		brelse(rootbuf);
	if (leafbuf)
		brelse(leafbuf);
	return (struct btree){ };
}

void free_btree(struct btree *btree)
{
	// write me
}

#ifndef main
struct uleaf { u32 magic, count; struct entry { u32 key, val; } entries[]; };

static inline struct uleaf *to_uleaf(vleaf *leaf)
{
	return leaf;
}

int uleaf_sniff(BTREE, vleaf *leaf)
{
	return to_uleaf(leaf)->magic == 0xc0de;
}

int uleaf_init(BTREE, vleaf *leaf)
{
	*to_uleaf(leaf) = (struct uleaf){ .magic = 0xc0de };
	return 0;
}

unsigned uleaf_need(BTREE, vleaf *leaf)
{
	return to_uleaf(leaf)->count;
}

unsigned uleaf_free(BTREE, vleaf *leaf)
{
	return btree->entries_per_leaf - to_uleaf(leaf)->count;
}

void uleaf_dump(BTREE, vleaf *data)
{
	struct uleaf *leaf = data;
	printf("leaf %p/%i", leaf, leaf->count);
	struct entry *entry, *limit = leaf->entries + leaf->count;
	for (entry = leaf->entries; entry < limit; entry++)
		printf(" %x:%x", entry->key, entry->val);
	printf(" (%x free)\n", uleaf_free(btree, leaf));
}

tuxkey_t uleaf_split(BTREE, tuxkey_t key, vleaf *from, vleaf *into)
{
	assert(uleaf_sniff(btree, from));
	struct uleaf *leaf = from;
	unsigned at = leaf->count / 2;
	if (leaf->count && key > leaf->entries[leaf->count - 1].key) // binsearch!
		at = leaf->count;
	unsigned tail = leaf->count - at;
	uleaf_init(btree, into);
	veccopy(to_uleaf(into)->entries, leaf->entries + at, tail);
	to_uleaf(into)->count = tail;
	leaf->count = at;
	return at < leaf->count ? to_uleaf(into)->entries[0].key : key;
}

unsigned uleaf_seek(BTREE, tuxkey_t key, struct uleaf *leaf)
{
	unsigned at = 0;
	while (at < leaf->count && leaf->entries[at].key < key)
		at++;
	return at;
}

int uleaf_chop(BTREE, tuxkey_t key, vleaf *vleaf)
{
	struct uleaf *leaf = vleaf;
	unsigned at = uleaf_seek(btree, key, leaf);
	leaf->count = at;
	return 0;
}

void *uleaf_resize(BTREE, tuxkey_t key, vleaf *data, unsigned one)
{
	assert(uleaf_sniff(btree, data));
	struct uleaf *leaf = data;
	if (uleaf_free(btree, leaf) < one)
		return NULL;
	unsigned at = uleaf_seek(btree, key, leaf);
	printf("expand leaf at 0x%x by %i\n", at, one);
	vecmove(leaf->entries + at + one, leaf->entries + at, leaf->count++ - at);
	return leaf->entries + at;
}

void uleaf_merge(BTREE, vleaf *into, vleaf *from)
{
}

struct btree_ops ops = {
	.leaf_sniff = uleaf_sniff,
	.leaf_init = uleaf_init,
	.leaf_split = uleaf_split,
	.leaf_resize = uleaf_resize,
	.leaf_dump = uleaf_dump,
	.leaf_need = uleaf_need,
	.leaf_free = uleaf_free,
	.leaf_merge = uleaf_merge,
	.leaf_chop = uleaf_chop,
	.balloc = balloc,
};

block_t balloc(SB)
{
	printf("-> %Lx\n", (L)sb->nextalloc);
	return sb->nextalloc++;
}

int uleaf_insert(BTREE, struct uleaf *leaf, unsigned key, unsigned val)
{
	printf("insert 0x%x -> 0x%x\n", key, val);
	struct entry *entry = uleaf_resize(btree, key, leaf, 1);
	if (!entry)
		return 1; // need to expand
	assert(entry);
	*entry = (struct entry){ .key = key, .val = val };
	return 0;
}

int main(int argc, char *argv[])
{
	struct dev *dev = &(struct dev){ .bits = 6 };
	struct map *map = new_map(dev, NULL);
	SB = &(struct sb){ .devmap = map, .blocksize = 1 << dev->bits };
	map->inode = &(struct inode){ .sb = sb, .map = map };
	init_buffers(dev, 1 << 20);
	sb->entries_per_node = (sb->blocksize - offsetof(struct bnode, entries)) / sizeof(struct index_entry);
	printf("entries_per_node = %i\n", sb->entries_per_node);
	struct btree btree = new_btree(sb, &ops);
	btree.entries_per_leaf = (sb->blocksize - offsetof(struct uleaf, entries)) / sizeof(struct entry);

	if (0) {
		struct buffer *buffer = new_leaf(&btree);
		for (int i = 0; i < 7; i++)
			uleaf_insert(&btree, buffer->data, i, i + 0x100);
		uleaf_dump(&btree, buffer->data);
		return 0;
	}

	struct path path[30];
	for (int key = 0; key < 30; key++) {
		if (probe(&btree, key, path))
			error("probe for %i failed", key);
		struct entry *entry = tree_expand(&btree, key, 1, path);
		*entry = (struct entry){ .key = key, .val = key + 0x100 };
		release_path(path, btree.root.depth + 1);
	}
	show_tree_range(&btree, 0, -1);
	show_buffers(sb->devmap);
	tree_chop(&btree, &(struct delete_info){ .key = 0x10 }, -1);
	show_tree_range(&btree, 0, -1);
	return 0;
}
#endif
