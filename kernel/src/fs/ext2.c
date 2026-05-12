#include <fs/ext2.h>
#include <debug/log.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>

#define EXT2_SUPER_OFFSET 1024
#define EXT2_SUPER_MAGIC 0xEF53
#define EXT2_ROOT_INO 2
#define EXT2_N_BLOCKS 15

typedef struct {
	uint32_t inodes_count;
	uint32_t blocks_count;
	uint32_t r_blocks_count;
	uint32_t free_blocks_count;
	uint32_t free_inodes_count;
	uint32_t first_data_block;
	uint32_t log_block_size;
	uint32_t log_frag_size;
	uint32_t blocks_per_group;
	uint32_t frags_per_group;
	uint32_t inodes_per_group;
	uint32_t mtime;
	uint32_t wtime;
	uint16_t mnt_count;
	uint16_t max_mnt_count;
	uint16_t magic;
	uint16_t state;
	uint16_t errors;
	uint16_t minor_rev_level;
	uint32_t lastcheck;
	uint32_t checkinterval;
	uint32_t creator_os;
	uint32_t rev_level;
	uint16_t def_resuid;
	uint16_t def_resgid;
	uint32_t first_ino;
	uint16_t inode_size;
	uint16_t block_group_nr;
	uint32_t feature_compat;
	uint32_t feature_incompat;
	uint32_t feature_ro_compat;
	uint8_t uuid[16];
	char volume_name[16];
	char last_mounted[64];
} __attribute__((packed)) ext2_super_t;

typedef struct {
	uint32_t block_bitmap;
	uint32_t inode_bitmap;
	uint32_t inode_table;
	uint16_t free_blocks_count;
	uint16_t free_inodes_count;
	uint16_t used_dirs_count;
	uint16_t pad;
	uint8_t reserved[12];
} __attribute__((packed)) ext2_group_desc_t;

typedef struct {
	uint16_t mode;
	uint16_t uid;
	uint32_t size_lo;
	uint32_t atime;
	uint32_t ctime;
	uint32_t mtime;
	uint32_t dtime;
	uint16_t gid;
	uint16_t links_count;
	uint32_t blocks;
	uint32_t flags;
	uint32_t osd1;
	uint32_t block[EXT2_N_BLOCKS];
	uint32_t generation;
	uint32_t file_acl;
	uint32_t size_high;
	uint32_t faddr;
	uint8_t osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct ext2_fs ext2_fs_t;

typedef struct {
	vfs_node_t vnode;
	ext2_fs_t *fs;
	uint32_t ino;
	ext2_inode_t inode;
} ext2_node_t;

struct ext2_fs {
	block_device_t *dev;
	uint64_t dev_id;
	ext2_super_t sb;
	uint32_t block_size;
	uint32_t group_count;
	uint64_t gd_off;
	ext2_group_desc_t *groups;
	vfs_node_t *root;
	char mount_path[64];
	ext2_fs_t *next;
};

static ext2_fs_t *mounts;
static int mounts_published;
static uint32_t mount_count;
static uint64_t next_ext2_dev_id = 1;

static int ext2_lookup(vfs_node_t *dir, const char *name, size_t len,
					   vfs_node_t **out);
static int ext2_create(vfs_node_t *dir, const char *name, size_t len,
					   vfs_mode_t mode, const vfs_cred_t *cred,
					   vfs_node_t **out);
static int ext2_mkdir(vfs_node_t *dir, const char *name, size_t len,
					  vfs_mode_t mode, const vfs_cred_t *cred,
					  vfs_node_t **out);
static int ext2_unlink(vfs_node_t *dir, const char *name, size_t len);
static int ext2_rmdir(vfs_node_t *dir, const char *name, size_t len);
static int ext2_rename(vfs_node_t *old_dir, const char *old_name, size_t old_len,
					   vfs_node_t *new_dir, const char *new_name, size_t new_len);
static int ext2_read(vfs_node_t *node, uint64_t off, void *buf, size_t len,
					 size_t *done);
static int ext2_write(vfs_node_t *node, uint64_t off, const void *buf,
					  size_t len, size_t *done);
static int ext2_readdir(vfs_node_t *dir, size_t index, vfs_dirent_t *out);
static int ext2_truncate(vfs_node_t *node, uint64_t size);
static void ext2_release(vfs_node_t *node);
static int ext2_mounts_read(void *ctx, uint64_t off, void *buf, size_t len,
							size_t *done);

static const vfs_ops_t ext2_ops = {
	.lookup = ext2_lookup,
	.create = ext2_create,
	.mkdir = ext2_mkdir,
	.unlink = ext2_unlink,
	.rmdir = ext2_rmdir,
	.rename = ext2_rename,
	.read = ext2_read,
	.write = ext2_write,
	.readdir = ext2_readdir,
	.truncate = ext2_truncate,
	.release = ext2_release,
};

static int copy_text_slice(const char *tmp, size_t total, uint64_t off,
						   void *buf, size_t len, size_t *done)
{
	if (done)
		*done = 0;
	if (!buf)
		return -EINVAL;
	if (off >= total || len == 0)
		return 0;
	size_t copy = total - (size_t)off;
	if (copy > len)
		copy = len;
	memcpy(buf, tmp + off, copy);
	if (done)
		*done = copy;
	return 0;
}

static int publish_mounts(void)
{
	if (mounts_published)
		return 0;
	int r =
		devfs_register_chr("/dev/mounts", 0444, ext2_mounts_read, NULL, NULL);
	if (r == 0 || r == -EEXIST) {
		mounts_published = 1;
		return 0;
	}
	return r;
}

static uint16_t rec_len_for(size_t name_len)
{
	return (uint16_t)((8 + name_len + 3) & ~3u);
}

static uint8_t file_type_for_mode(vfs_mode_t mode)
{
	switch (mode & VFS_S_IFMT) {
	case VFS_S_IFREG:
		return 1;
	case VFS_S_IFDIR:
		return 2;
	default:
		return 0;
	}
}

static uint64_t inode_size(const ext2_inode_t *inode)
{
	uint64_t size = inode->size_lo;
	if ((inode->mode & VFS_S_IFMT) == VFS_S_IFREG)
		size |= (uint64_t)inode->size_high << 32;
	return size;
}

static vfs_mode_t inode_mode(const ext2_inode_t *inode)
{
	return (vfs_mode_t)inode->mode;
}

static int read_block(ext2_fs_t *fs, uint32_t block, void *buf)
{
	return block_read(fs->dev, (uint64_t)block * fs->block_size, buf,
					  fs->block_size);
}

static int write_block(ext2_fs_t *fs, uint32_t block, const void *buf)
{
	return block_write(fs->dev, (uint64_t)block * fs->block_size, buf,
					   fs->block_size);
}

static int write_metadata(ext2_fs_t *fs)
{
	int r = block_write(fs->dev, EXT2_SUPER_OFFSET, &fs->sb, sizeof(fs->sb));
	if (r != 0)
		return r;
	return block_write(fs->dev, fs->gd_off, fs->groups,
					   fs->group_count * sizeof(ext2_group_desc_t));
}

static int read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out)
{
	if (!ino || ino > fs->sb.inodes_count)
		return -ENOENT;
	uint32_t group = (ino - 1) / fs->sb.inodes_per_group;
	uint32_t index = (ino - 1) % fs->sb.inodes_per_group;
	if (group >= fs->group_count)
		return -ENOENT;

	uint32_t inode_size = fs->sb.inode_size ? fs->sb.inode_size : 128;
	uint64_t off = (uint64_t)fs->groups[group].inode_table * fs->block_size +
				   (uint64_t)index * inode_size;
	return block_read(fs->dev, off, out, sizeof(*out));
}

static int write_inode(ext2_fs_t *fs, uint32_t ino, const ext2_inode_t *in)
{
	if (!ino || ino > fs->sb.inodes_count)
		return -ENOENT;
	uint32_t group = (ino - 1) / fs->sb.inodes_per_group;
	uint32_t index = (ino - 1) % fs->sb.inodes_per_group;
	if (group >= fs->group_count)
		return -ENOENT;

	uint32_t inode_size = fs->sb.inode_size ? fs->sb.inode_size : 128;
	uint64_t off = (uint64_t)fs->groups[group].inode_table * fs->block_size +
				   (uint64_t)index * inode_size;
	return block_write(fs->dev, off, in, sizeof(*in));
}

static int alloc_ext2_node(ext2_fs_t *fs, uint32_t ino, vfs_node_t **out)
{
	ext2_node_t *node = kzalloc(sizeof(*node));
	if (!node)
		return -ENOMEM;
	int r = read_inode(fs, ino, &node->inode);
	if (r != 0) {
		kfree(node);
		return r;
	}
	node->fs = fs;
	node->ino = ino;
	vfs_node_init(&node->vnode, &ext2_ops, inode_mode(&node->inode),
				  node->inode.uid, node->inode.gid);
	node->vnode.dev = fs->dev_id;
	node->vnode.ino = ino;
	node->vnode.size = inode_size(&node->inode);
	node->vnode.nlink = node->inode.links_count;
	node->vnode.private_data = node;
	*out = &node->vnode;
	return 0;
}

static int bitmap_alloc(ext2_fs_t *fs, uint32_t group, uint32_t bitmap_block,
						uint32_t max_bits, uint32_t *out_bit)
{
	uint8_t *bitmap = kzalloc(fs->block_size);
	if (!bitmap)
		return -ENOMEM;
	int r = read_block(fs, bitmap_block, bitmap);
	if (r != 0) {
		kfree(bitmap);
		return r;
	}
	for (uint32_t bit = 0; bit < max_bits; bit++) {
		uint32_t byte = bit / 8;
		uint8_t mask = (uint8_t)(1u << (bit & 7));
		if (bitmap[byte] & mask)
			continue;
		bitmap[byte] |= mask;
		r = write_block(fs, bitmap_block, bitmap);
		kfree(bitmap);
		if (r != 0)
			return r;
		*out_bit = bit;
		return 0;
	}
	kfree(bitmap);
	return -ENOSYS;
}

static int alloc_block(ext2_fs_t *fs, uint32_t *out)
{
	for (uint32_t group = 0; group < fs->group_count; group++) {
		if (fs->groups[group].free_blocks_count == 0)
			continue;
		uint32_t bit = 0;
		int r = bitmap_alloc(fs, group, fs->groups[group].block_bitmap,
							 fs->sb.blocks_per_group, &bit);
		if (r != 0)
			continue;
		uint32_t block =
			group * fs->sb.blocks_per_group + bit + fs->sb.first_data_block;
		if (block >= fs->sb.blocks_count)
			return -EINVAL;
		fs->groups[group].free_blocks_count--;
		fs->sb.free_blocks_count--;
		r = write_metadata(fs);
		if (r != 0)
			return r;
		uint8_t *zero = kzalloc(fs->block_size);
		if (!zero)
			return -ENOMEM;
		r = write_block(fs, block, zero);
		kfree(zero);
		if (r != 0)
			return r;
		*out = block;
		return 0;
	}
	return -ENOSYS;
}

static int alloc_inode(ext2_fs_t *fs, uint32_t *out)
{
	for (uint32_t group = 0; group < fs->group_count; group++) {
		if (fs->groups[group].free_inodes_count == 0)
			continue;
		uint32_t bit = 0;
		int r = bitmap_alloc(fs, group, fs->groups[group].inode_bitmap,
							 fs->sb.inodes_per_group, &bit);
		if (r != 0)
			continue;
		uint32_t ino = group * fs->sb.inodes_per_group + bit + 1;
		if (ino == 0 || ino > fs->sb.inodes_count)
			return -EINVAL;
		fs->groups[group].free_inodes_count--;
		fs->sb.free_inodes_count--;
		r = write_metadata(fs);
		if (r != 0)
			return r;
		*out = ino;
		return 0;
	}
	return -ENOSYS;
}

static uint64_t ext2_index_span(uint32_t ptrs_per_block, unsigned depth)
{
	uint64_t span = 1;

	while (depth-- > 1)
		span *= ptrs_per_block;

	return span;
}

static int file_block_indirect(ext2_node_t *node, uint32_t block,
							   unsigned depth, uint32_t file_block,
							   uint32_t *out)
{
	*out = 0;

	if (!block)
		return 0;

	uint32_t ptrs_per_block = node->fs->block_size / sizeof(uint32_t);
	uint64_t span = ext2_index_span(ptrs_per_block, depth);

	if ((uint64_t)file_block >= span * ptrs_per_block)
		return -ENOSYS;

	uint32_t *ptrs = kzalloc(node->fs->block_size);
	if (!ptrs)
		return -ENOMEM;

	int r = read_block(node->fs, block, ptrs);
	if (r != 0) {
		kfree(ptrs);
		return r;
	}

	if (depth == 1) {
		*out = ptrs[file_block];
		kfree(ptrs);
		return 0;
	}

	uint32_t slot = (uint32_t)((uint64_t)file_block / span);
	uint32_t next_index = (uint32_t)((uint64_t)file_block % span);
	uint32_t next_block = ptrs[slot];
	kfree(ptrs);

	return file_block_indirect(node, next_block, depth - 1, next_index, out);
}

static int file_block(ext2_node_t *node, uint32_t file_block, uint32_t *out)
{
	*out = 0;

	if (file_block < 12) {
		*out = node->inode.block[file_block];
		return 0;
	}

	file_block -= 12;
	uint32_t ptrs_per_block = node->fs->block_size / sizeof(uint32_t);

	if (file_block < ptrs_per_block)
		return file_block_indirect(node, node->inode.block[12], 1,
								   file_block, out);

	file_block -= ptrs_per_block;
	uint64_t double_blocks = (uint64_t)ptrs_per_block * ptrs_per_block;
	if ((uint64_t)file_block < double_blocks)
		return file_block_indirect(node, node->inode.block[13], 2,
								   file_block, out);

	file_block -= (uint32_t)double_blocks;
	uint64_t triple_blocks = double_blocks * ptrs_per_block;
	if ((uint64_t)file_block < triple_blocks)
		return file_block_indirect(node, node->inode.block[14], 3,
								   file_block, out);

	return -ENOSYS;
}

static int ensure_file_block_indirect(ext2_node_t *node, uint32_t *root,
								  unsigned depth, uint32_t file_block,
								  uint32_t *out)
{
	uint32_t ptrs_per_block = node->fs->block_size / sizeof(uint32_t);
	uint64_t span = ext2_index_span(ptrs_per_block, depth);

	if ((uint64_t)file_block >= span * ptrs_per_block)
		return -ENOSYS;

	if (!*root) {
		int r = alloc_block(node->fs, root);
		if (r != 0)
			return r;

		node->inode.blocks += node->fs->block_size / 512;
		r = write_inode(node->fs, node->ino, &node->inode);
		if (r != 0)
			return r;
	}

	uint32_t *ptrs = kzalloc(node->fs->block_size);
	if (!ptrs)
		return -ENOMEM;

	int r = read_block(node->fs, *root, ptrs);
	if (r != 0) {
		kfree(ptrs);
		return r;
	}

	if (depth == 1) {
		if (!ptrs[file_block]) {
			r = alloc_block(node->fs, &ptrs[file_block]);
			if (r == 0) {
				node->inode.blocks += node->fs->block_size / 512;
				r = write_block(node->fs, *root, ptrs);
				if (r == 0)
					r = write_inode(node->fs, node->ino, &node->inode);
			}
		}

		if (r == 0)
			*out = ptrs[file_block];
		kfree(ptrs);
		return r;
	}

	uint32_t slot = (uint32_t)((uint64_t)file_block / span);
	uint32_t next_index = (uint32_t)((uint64_t)file_block % span);

	if (!ptrs[slot]) {
		r = alloc_block(node->fs, &ptrs[slot]);
		if (r != 0) {
			kfree(ptrs);
			return r;
		}

		node->inode.blocks += node->fs->block_size / 512;
		r = write_block(node->fs, *root, ptrs);
		if (r == 0)
			r = write_inode(node->fs, node->ino, &node->inode);
		if (r != 0) {
			kfree(ptrs);
			return r;
		}
	}

	uint32_t child = ptrs[slot];
	kfree(ptrs);

	return ensure_file_block_indirect(node, &child, depth - 1, next_index, out);
}

static int ensure_file_block(ext2_node_t *node, uint32_t file_block_index,
							 uint32_t *out)
{
	int r = file_block(node, file_block_index, out);
	if (r != 0 || *out)
		return r;

	if (file_block_index < 12) {
		r = alloc_block(node->fs, out);
		if (r != 0)
			return r;
		node->inode.block[file_block_index] = *out;
		node->inode.blocks += node->fs->block_size / 512;
		return write_inode(node->fs, node->ino, &node->inode);
	}

	file_block_index -= 12;
	uint32_t ptrs_per_block = node->fs->block_size / sizeof(uint32_t);

	if (file_block_index < ptrs_per_block) {
		uint32_t root = node->inode.block[12];
		r = ensure_file_block_indirect(node, &root, 1, file_block_index, out);
		if (r == 0 && node->inode.block[12] != root) {
			node->inode.block[12] = root;
			r = write_inode(node->fs, node->ino, &node->inode);
		}
		return r;
	}

	file_block_index -= ptrs_per_block;
	uint64_t double_blocks = (uint64_t)ptrs_per_block * ptrs_per_block;
	if ((uint64_t)file_block_index < double_blocks) {
		uint32_t root = node->inode.block[13];
		r = ensure_file_block_indirect(node, &root, 2, file_block_index, out);
		if (r == 0 && node->inode.block[13] != root) {
			node->inode.block[13] = root;
			r = write_inode(node->fs, node->ino, &node->inode);
		}
		return r;
	}

	file_block_index -= (uint32_t)double_blocks;
	uint64_t triple_blocks = double_blocks * ptrs_per_block;
	if ((uint64_t)file_block_index < triple_blocks) {
		uint32_t root = node->inode.block[14];
		r = ensure_file_block_indirect(node, &root, 3, file_block_index, out);
		if (r == 0 && node->inode.block[14] != root) {
			node->inode.block[14] = root;
			r = write_inode(node->fs, node->ino, &node->inode);
		}
		return r;
	}

	return -ENOSYS;
}

static int ext2_read(vfs_node_t *vnode, uint64_t off, void *buf, size_t len,
					 size_t *done)
{
	if (done)
		*done = 0;
	if (!VFS_S_ISREG(vnode->mode))
		return -EISDIR;
	if (off >= vnode->size || len == 0)
		return 0;
	if (len > vnode->size - off)
		len = (size_t)(vnode->size - off);

	ext2_node_t *node = vnode->private_data;
	uint8_t *blk = kzalloc(node->fs->block_size);
	if (!blk)
		return -ENOMEM;

	size_t copied = 0;
	while (copied < len) {
		uint64_t pos = off + copied;
		uint32_t fblk = (uint32_t)(pos / node->fs->block_size);
		size_t boff = (size_t)(pos % node->fs->block_size);
		size_t chunk = node->fs->block_size - boff;
		if (chunk > len - copied)
			chunk = len - copied;

		uint32_t disk_block = 0;
		int r = file_block(node, fblk, &disk_block);
		if (r != 0) {
			kfree(blk);
			return r;
		}
		if (disk_block) {
			r = read_block(node->fs, disk_block, blk);
			if (r != 0) {
				kfree(blk);
				return r;
			}
			memcpy((uint8_t *)buf + copied, blk + boff, chunk);
		} else {
			memset((uint8_t *)buf + copied, 0, chunk);
		}
		copied += chunk;
	}
	kfree(blk);
	if (done)
		*done = copied;
	return 0;
}

static int ext2_write(vfs_node_t *vnode, uint64_t off, const void *buf,
					  size_t len, size_t *done)
{
	if (done)
		*done = 0;
	if (!VFS_S_ISREG(vnode->mode))
		return -EISDIR;
	if (len == 0)
		return 0;

	ext2_node_t *node = vnode->private_data;
	uint8_t *blk = kzalloc(node->fs->block_size);
	if (!blk)
		return -ENOMEM;

	size_t copied = 0;
	while (copied < len) {
		uint64_t pos = off + copied;
		uint32_t fblk = (uint32_t)(pos / node->fs->block_size);
		size_t boff = (size_t)(pos % node->fs->block_size);
		size_t chunk = node->fs->block_size - boff;
		if (chunk > len - copied)
			chunk = len - copied;

		uint32_t disk_block = 0;
		int r = ensure_file_block(node, fblk, &disk_block);
		if (r != 0) {
			kfree(blk);
			return copied ? 0 : r;
		}
		if (boff != 0 || chunk != node->fs->block_size) {
			r = read_block(node->fs, disk_block, blk);
			if (r != 0) {
				kfree(blk);
				return copied ? 0 : r;
			}
		} else {
			memset(blk, 0, node->fs->block_size);
		}
		memcpy(blk + boff, (const uint8_t *)buf + copied, chunk);
		r = write_block(node->fs, disk_block, blk);
		if (r != 0) {
			kfree(blk);
			return copied ? 0 : r;
		}
		copied += chunk;
	}

	if (off + copied > vnode->size) {
		vnode->size = off + copied;
		node->inode.size_lo = (uint32_t)vnode->size;
		node->inode.size_high = (uint32_t)(vnode->size >> 32);
		int r = write_inode(node->fs, node->ino, &node->inode);
		if (r != 0) {
			kfree(blk);
			return r;
		}
	}
	kfree(blk);
	if (done)
		*done = copied;
	return 0;
}

static int read_dir_chunk(ext2_node_t *dir, uint64_t off, void *buf, size_t len)
{
	size_t done = 0;
	if (!VFS_S_ISDIR(dir->vnode.mode))
		return -ENOTDIR;
	uint8_t *blk = kzalloc(dir->fs->block_size);
	if (!blk)
		return -ENOMEM;

	size_t copied = 0;
	while (copied < len) {
		uint64_t pos = off + copied;
		uint32_t fblk = (uint32_t)(pos / dir->fs->block_size);
		size_t boff = (size_t)(pos % dir->fs->block_size);
		size_t chunk = dir->fs->block_size - boff;
		if (chunk > len - copied)
			chunk = len - copied;

		uint32_t disk_block = 0;
		int r = file_block(dir, fblk, &disk_block);
		if (r != 0) {
			kfree(blk);
			return r;
		}
		if (disk_block) {
			r = read_block(dir->fs, disk_block, blk);
			if (r != 0) {
				kfree(blk);
				return r;
			}
			memcpy((uint8_t *)buf + copied, blk + boff, chunk);
		} else {
			memset((uint8_t *)buf + copied, 0, chunk);
		}
		copied += chunk;
		done += chunk;
	}
	kfree(blk);
	return done == len ? 0 : -EINVAL;
}

static int write_dir_chunk(ext2_node_t *dir, uint64_t off, const void *buf,
						   size_t len)
{
	if (!VFS_S_ISDIR(dir->vnode.mode))
		return -ENOTDIR;
	uint8_t *blk = kzalloc(dir->fs->block_size);
	if (!blk)
		return -ENOMEM;
	size_t copied = 0;
	while (copied < len) {
		uint64_t pos = off + copied;
		uint32_t fblk = (uint32_t)(pos / dir->fs->block_size);
		size_t boff = (size_t)(pos % dir->fs->block_size);
		size_t chunk = dir->fs->block_size - boff;
		if (chunk > len - copied)
			chunk = len - copied;
		uint32_t disk_block = 0;
		int r = ensure_file_block(dir, fblk, &disk_block);
		if (r != 0) {
			kfree(blk);
			return r;
		}
		r = read_block(dir->fs, disk_block, blk);
		if (r != 0) {
			kfree(blk);
			return r;
		}
		memcpy(blk + boff, (const uint8_t *)buf + copied, chunk);
		r = write_block(dir->fs, disk_block, blk);
		if (r != 0) {
			kfree(blk);
			return r;
		}
		copied += chunk;
	}
	kfree(blk);
	return 0;
}

static int ext2_lookup(vfs_node_t *vdir, const char *name, size_t len,
					   vfs_node_t **out)
{
	if (!VFS_S_ISDIR(vdir->mode))
		return -ENOTDIR;
	ext2_node_t *dir = vdir->private_data;
	if (len == 1 && name[0] == '.') {
		vfs_node_ref(vdir);
		*out = vdir;
		return 0;
	}

	uint8_t hdr[8];
	for (uint64_t off = 0; off + sizeof(hdr) <= vdir->size;) {
		int r = read_dir_chunk(dir, off, hdr, sizeof(hdr));
		if (r != 0)
			return r;
		uint32_t ino = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
					   ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
		uint16_t rec_len = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
		uint8_t name_len = hdr[6];
		if (rec_len < 8 || off + rec_len > vdir->size)
			return -EINVAL;
		if (ino && name_len == len) {
			char tmp[VFS_NAME_MAX + 1];
			r = read_dir_chunk(dir, off + 8, tmp, name_len);
			if (r != 0)
				return r;
			if (memcmp(tmp, name, len) == 0)
				return alloc_ext2_node(dir->fs, ino, out);
		}
		off += rec_len;
	}
	return -ENOENT;
}

static int add_dirent(ext2_node_t *dir, const char *name, size_t len,
					  uint32_t ino, uint8_t file_type)
{
	uint16_t need = rec_len_for(len);
	uint8_t *entry = kzalloc(need);
	if (!entry)
		return -ENOMEM;
	entry[0] = (uint8_t)ino;
	entry[1] = (uint8_t)(ino >> 8);
	entry[2] = (uint8_t)(ino >> 16);
	entry[3] = (uint8_t)(ino >> 24);
	entry[6] = (uint8_t)len;
	entry[7] = file_type;
	memcpy(entry + 8, name, len);

	uint8_t hdr[8];
	for (uint64_t off = 0; off + sizeof(hdr) <= dir->vnode.size;) {
		int r = read_dir_chunk(dir, off, hdr, sizeof(hdr));
		if (r != 0) {
			kfree(entry);
			return r;
		}
		uint16_t rec_len = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
		uint8_t name_len = hdr[6];
		if (rec_len < 8 || off + rec_len > dir->vnode.size) {
			kfree(entry);
			return -EINVAL;
		}
		uint16_t used = rec_len_for(name_len);
		if (rec_len >= used + need) {
			hdr[4] = (uint8_t)used;
			hdr[5] = (uint8_t)(used >> 8);
			r = write_dir_chunk(dir, off, hdr, sizeof(hdr));
			if (r != 0) {
				kfree(entry);
				return r;
			}
			uint16_t new_len = rec_len - used;
			entry[4] = (uint8_t)new_len;
			entry[5] = (uint8_t)(new_len >> 8);
			r = write_dir_chunk(dir, off + used, entry, need);
			kfree(entry);
			return r;
		}
		off += rec_len;
	}

	uint64_t off = dir->vnode.size;
	uint16_t rec_len = dir->fs->block_size;
	entry[4] = (uint8_t)rec_len;
	entry[5] = (uint8_t)(rec_len >> 8);
	int r = write_dir_chunk(dir, off, entry, need);
	if (r == 0) {
		dir->vnode.size += dir->fs->block_size;
		dir->inode.size_lo = (uint32_t)dir->vnode.size;
		dir->inode.size_high = (uint32_t)(dir->vnode.size >> 32);
		r = write_inode(dir->fs, dir->ino, &dir->inode);
	}
	kfree(entry);
	return r;
}

static int find_dirent(ext2_node_t *dir, const char *name, size_t len,
					   uint64_t *entry_off, uint64_t *prev_off,
					   uint32_t *ino_out, uint16_t *rec_len_out,
					   uint8_t *name_len_out)
{
	uint8_t hdr[8];
	uint64_t prev = UINT64_MAX;

	for (uint64_t off = 0; off + sizeof(hdr) <= dir->vnode.size;) {
		int r = read_dir_chunk(dir, off, hdr, sizeof(hdr));
		if (r != 0)
			return r;

		uint32_t ino = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
					   ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
		uint16_t rec_len = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
		uint8_t name_len = hdr[6];
		if (rec_len < 8 || off + rec_len > dir->vnode.size)
			return -EINVAL;

		if (ino && name_len == len) {
			char tmp[VFS_NAME_MAX + 1];
			r = read_dir_chunk(dir, off + 8, tmp, name_len);
			if (r != 0)
				return r;
			if (memcmp(tmp, name, len) == 0) {
				if (entry_off)
					*entry_off = off;
				if (prev_off)
					*prev_off = prev;
				if (ino_out)
					*ino_out = ino;
				if (rec_len_out)
					*rec_len_out = rec_len;
				if (name_len_out)
					*name_len_out = name_len;
				return 0;
			}
		}

		prev = off;
		off += rec_len;
	}

	return -ENOENT;
}

static int remove_dirent(ext2_node_t *dir, const char *name, size_t len)
{
	uint64_t off = 0;
	uint64_t prev = UINT64_MAX;
	uint16_t rec_len = 0;
	int r = find_dirent(dir, name, len, &off, &prev, NULL, &rec_len, NULL);
	if (r != 0)
		return r;

	if (prev != UINT64_MAX) {
		uint8_t prev_hdr[8];
		r = read_dir_chunk(dir, prev, prev_hdr, sizeof(prev_hdr));
		if (r != 0)
			return r;
		uint16_t prev_len = (uint16_t)prev_hdr[4] | ((uint16_t)prev_hdr[5] << 8);
		if (prev_len < 8 || prev + prev_len > dir->vnode.size)
			return -EINVAL;
		prev_len = (uint16_t)(prev_len + rec_len);
		prev_hdr[4] = (uint8_t)prev_len;
		prev_hdr[5] = (uint8_t)(prev_len >> 8);
		return write_dir_chunk(dir, prev, prev_hdr, sizeof(prev_hdr));
	}

	uint8_t hdr[8];
	r = read_dir_chunk(dir, off, hdr, sizeof(hdr));
	if (r != 0)
		return r;
	memset(hdr, 0, 4);
	return write_dir_chunk(dir, off, hdr, sizeof(hdr));
}

static int replace_dirent_ino(ext2_node_t *dir, const char *name, size_t len,
							  uint32_t ino, uint8_t file_type)
{
	uint64_t off = 0;
	int r = find_dirent(dir, name, len, &off, NULL, NULL, NULL, NULL);
	if (r != 0)
		return r;

	uint8_t hdr[8];
	r = read_dir_chunk(dir, off, hdr, sizeof(hdr));
	if (r != 0)
		return r;
	hdr[0] = (uint8_t)ino;
	hdr[1] = (uint8_t)(ino >> 8);
	hdr[2] = (uint8_t)(ino >> 16);
	hdr[3] = (uint8_t)(ino >> 24);
	hdr[7] = file_type;
	return write_dir_chunk(dir, off, hdr, sizeof(hdr));
}

static int dir_is_empty(ext2_node_t *dir)
{
	uint8_t hdr[8];
	for (uint64_t off = 0; off + sizeof(hdr) <= dir->vnode.size;) {
		int r = read_dir_chunk(dir, off, hdr, sizeof(hdr));
		if (r != 0)
			return r;

		uint32_t ino = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
					   ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
		uint16_t rec_len = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
		uint8_t name_len = hdr[6];
		if (rec_len < 8 || off + rec_len > dir->vnode.size)
			return -EINVAL;

		if (ino && name_len) {
			char tmp[VFS_NAME_MAX + 1];
			r = read_dir_chunk(dir, off + 8, tmp, name_len);
			if (r != 0)
				return r;
			if (!((name_len == 1 && tmp[0] == '.') ||
				  (name_len == 2 && tmp[0] == '.' && tmp[1] == '.')))
				return 0;
		}

		off += rec_len;
	}

	return 1;
}

static int is_ancestor_dir(ext2_node_t *dir, ext2_node_t *candidate_child)
{
	vfs_node_t *cur = &candidate_child->vnode;
	for (;;) {
		if (cur->ino == dir->vnode.ino) {
			if (cur != &candidate_child->vnode)
				vfs_node_release(cur);
			return 1;
		}
		vfs_node_t *parent = NULL;
		int r = ext2_lookup(cur, "..", 2, &parent);
		if (r != 0) {
			if (cur != &candidate_child->vnode)
				vfs_node_release(cur);
			return 0;
		}
		if (parent->ino == cur->ino) {
			if (cur != &candidate_child->vnode)
				vfs_node_release(cur);
			vfs_node_release(parent);
			return 0;
		}
		vfs_node_t *old = cur;
		cur = parent;
		if (old != &candidate_child->vnode)
			vfs_node_release(old);
	}
}

static int ext2_create(vfs_node_t *vdir, const char *name, size_t len,
					   vfs_mode_t mode, const vfs_cred_t *cred,
					   vfs_node_t **out)
{
	if (!VFS_S_ISDIR(vdir->mode))
		return -ENOTDIR;
	if (len == 0 || len > 255)
		return len == 0 ? -EINVAL : -ENAMETOOLONG;

	vfs_node_t *existing = NULL;
	int r = ext2_lookup(vdir, name, len, &existing);
	if (r == 0) {
		vfs_node_release(existing);
		return -EEXIST;
	}
	if (r != -ENOENT)
		return r;

	ext2_node_t *dir = vdir->private_data;
	uint32_t ino = 0;
	r = alloc_inode(dir->fs, &ino);
	if (r != 0)
		return r;

	ext2_inode_t inode;
	memset(&inode, 0, sizeof(inode));
	inode.mode = (uint16_t)(VFS_S_IFREG | (mode & VFS_S_PERM));
	inode.uid = cred ? cred->uid : 0;
	inode.gid = cred ? cred->gid : 0;
	inode.links_count = 1;
	r = write_inode(dir->fs, ino, &inode);
	if (r != 0)
		return r;

	r = add_dirent(dir, name, len, ino, 1);
	if (r != 0)
		return r;
	return alloc_ext2_node(dir->fs, ino, out);
}

static int ext2_mkdir(vfs_node_t *vdir, const char *name, size_t len,
					  vfs_mode_t mode, const vfs_cred_t *cred, vfs_node_t **out)
{
	if (!VFS_S_ISDIR(vdir->mode))
		return -ENOTDIR;
	if (len == 0 || len > 255)
		return len == 0 ? -EINVAL : -ENAMETOOLONG;

	vfs_node_t *existing = NULL;
	int r = ext2_lookup(vdir, name, len, &existing);
	if (r == 0) {
		vfs_node_release(existing);
		return -EEXIST;
	}
	if (r != -ENOENT)
		return r;

	ext2_node_t *parent = vdir->private_data;
	uint32_t ino = 0;
	r = alloc_inode(parent->fs, &ino);
	if (r != 0)
		return r;

	ext2_inode_t inode;
	memset(&inode, 0, sizeof(inode));
	inode.mode = (uint16_t)(VFS_S_IFDIR | (mode & VFS_S_PERM));
	inode.uid = cred ? cred->uid : 0;
	inode.gid = cred ? cred->gid : 0;
	inode.links_count = 2;
	inode.size_lo = parent->fs->block_size;

	uint32_t block = 0;
	r = alloc_block(parent->fs, &block);
	if (r != 0)
		return r;
	inode.block[0] = block;
	inode.blocks = parent->fs->block_size / 512;

	uint8_t *blk = kzalloc(parent->fs->block_size);
	if (!blk)
		return -ENOMEM;
	uint16_t dot_len = rec_len_for(1);
	blk[0] = (uint8_t)ino;
	blk[1] = (uint8_t)(ino >> 8);
	blk[2] = (uint8_t)(ino >> 16);
	blk[3] = (uint8_t)(ino >> 24);
	blk[4] = (uint8_t)dot_len;
	blk[5] = (uint8_t)(dot_len >> 8);
	blk[6] = 1;
	blk[7] = 2;
	blk[8] = '.';
	uint16_t dotdot_len = parent->fs->block_size - dot_len;
	blk[dot_len + 0] = (uint8_t)parent->ino;
	blk[dot_len + 1] = (uint8_t)(parent->ino >> 8);
	blk[dot_len + 2] = (uint8_t)(parent->ino >> 16);
	blk[dot_len + 3] = (uint8_t)(parent->ino >> 24);
	blk[dot_len + 4] = (uint8_t)dotdot_len;
	blk[dot_len + 5] = (uint8_t)(dotdot_len >> 8);
	blk[dot_len + 6] = 2;
	blk[dot_len + 7] = 2;
	blk[dot_len + 8] = '.';
	blk[dot_len + 9] = '.';
	r = write_block(parent->fs, block, blk);
	kfree(blk);
	if (r != 0)
		return r;

	r = write_inode(parent->fs, ino, &inode);
	if (r != 0)
		return r;
	parent->inode.links_count++;
	parent->vnode.nlink = parent->inode.links_count;
	r = write_inode(parent->fs, parent->ino, &parent->inode);
	if (r != 0)
		return r;
	r = add_dirent(parent, name, len, ino, 2);
	if (r != 0)
		return r;
	return alloc_ext2_node(parent->fs, ino, out);
}

static int ext2_readdir(vfs_node_t *vdir, size_t index, vfs_dirent_t *out)
{
	if (!VFS_S_ISDIR(vdir->mode))
		return -ENOTDIR;
	ext2_node_t *dir = vdir->private_data;
	uint8_t hdr[8];
	size_t seen = 0;
	for (uint64_t off = 0; off + sizeof(hdr) <= vdir->size;) {
		int r = read_dir_chunk(dir, off, hdr, sizeof(hdr));
		if (r != 0)
			return r;
		uint32_t ino = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
					   ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
		uint16_t rec_len = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
		uint8_t name_len = hdr[6];
		if (rec_len < 8 || off + rec_len > vdir->size)
			return -EINVAL;
		if (ino && name_len) {
			if (seen++ == index) {
				r = read_dir_chunk(dir, off + 8, out->name, name_len);
				if (r != 0)
					return r;
				out->name[name_len] = 0;
				vfs_node_t *child = NULL;
				r = alloc_ext2_node(dir->fs, ino, &child);
				if (r != 0)
					return r;
				out->mode = child->mode;
				out->uid = child->uid;
				out->gid = child->gid;
				out->size = child->size;
				out->nlink = child->nlink;
				vfs_node_release(child);
				return 0;
			}
		}
		off += rec_len;
	}
	return -ENOENT;
}

static int ext2_unlink(vfs_node_t *vdir, const char *name, size_t len)
{
	if (!VFS_S_ISDIR(vdir->mode))
		return -ENOTDIR;

	vfs_node_t *target_vnode = NULL;
	int r = ext2_lookup(vdir, name, len, &target_vnode);
	if (r != 0)
		return r;

	if (VFS_S_ISDIR(target_vnode->mode)) {
		vfs_node_release(target_vnode);
		return -EISDIR;
	}

	ext2_node_t *dir = vdir->private_data;
	ext2_node_t *target = target_vnode->private_data;
	uint16_t old_links = target->inode.links_count;

	if (target->inode.links_count > 0)
		target->inode.links_count--;
	target_vnode->nlink = target->inode.links_count;

	r = write_inode(target->fs, target->ino, &target->inode);
	if (r == 0)
		r = remove_dirent(dir, name, len);

	if (r != 0) {
		target->inode.links_count = old_links;
		target_vnode->nlink = old_links;
		(void)write_inode(target->fs, target->ino, &target->inode);
	}

	vfs_node_release(target_vnode);
	return r;
}

static int ext2_rmdir(vfs_node_t *vdir, const char *name, size_t len)
{
	if (!VFS_S_ISDIR(vdir->mode))
		return -ENOTDIR;
	if ((len == 1 && name[0] == '.') ||
		(len == 2 && name[0] == '.' && name[1] == '.'))
		return -EINVAL;

	vfs_node_t *target_vnode = NULL;
	int r = ext2_lookup(vdir, name, len, &target_vnode);
	if (r != 0)
		return r;

	if (!VFS_S_ISDIR(target_vnode->mode)) {
		vfs_node_release(target_vnode);
		return -ENOTDIR;
	}

	ext2_node_t *dir = vdir->private_data;
	ext2_node_t *target = target_vnode->private_data;

	r = dir_is_empty(target);
	if (r < 0) {
		vfs_node_release(target_vnode);
		return r;
	}
	if (!r) {
		vfs_node_release(target_vnode);
		return -ENOTEMPTY;
	}
	if (dir->inode.links_count == 0) {
		vfs_node_release(target_vnode);
		return -EINVAL;
	}

	uint16_t old_parent_links = dir->inode.links_count;
	uint16_t old_target_links = target->inode.links_count;

	dir->inode.links_count--;
	vdir->nlink = dir->inode.links_count;
	target->inode.links_count = 0;
	target_vnode->nlink = 0;

	r = write_inode(target->fs, target->ino, &target->inode);
	if (r == 0)
		r = write_inode(dir->fs, dir->ino, &dir->inode);
	if (r == 0)
		r = remove_dirent(dir, name, len);

	if (r != 0) {
		dir->inode.links_count = old_parent_links;
		vdir->nlink = old_parent_links;
		target->inode.links_count = old_target_links;
		target_vnode->nlink = old_target_links;
		(void)write_inode(dir->fs, dir->ino, &dir->inode);
		(void)write_inode(target->fs, target->ino, &target->inode);
	}

	vfs_node_release(target_vnode);
	return r;
}

static int ext2_rename(vfs_node_t *old_dir_node, const char *old_name, size_t old_len,
					   vfs_node_t *new_dir_node, const char *new_name, size_t new_len)
{
	if (!VFS_S_ISDIR(old_dir_node->mode) || !VFS_S_ISDIR(new_dir_node->mode))
		return -ENOTDIR;
	if (old_len == 0 || old_len > VFS_NAME_MAX || new_len == 0 ||
		new_len > VFS_NAME_MAX)
		return -ENAMETOOLONG;
	if ((old_len == 1 && old_name[0] == '.') ||
		(old_len == 2 && old_name[0] == '.' && old_name[1] == '.') ||
		(new_len == 1 && new_name[0] == '.') ||
		(new_len == 2 && new_name[0] == '.' && new_name[1] == '.'))
		return -EINVAL;

	ext2_node_t *old_dir = old_dir_node->private_data;
	ext2_node_t *new_dir = new_dir_node->private_data;
	if (old_dir->fs != new_dir->fs)
		return -EXDEV;

	vfs_node_t *source_vnode = NULL;
	int r = ext2_lookup(old_dir_node, old_name, old_len, &source_vnode);
	if (r != 0)
		return r;
	ext2_node_t *source = source_vnode->private_data;

	if (old_dir_node == new_dir_node && old_len == new_len &&
		memcmp(old_name, new_name, old_len) == 0) {
		vfs_node_release(source_vnode);
		return 0;
	}

	if (VFS_S_ISDIR(source_vnode->mode) &&
		is_ancestor_dir(source, new_dir)) {
		vfs_node_release(source_vnode);
		return -EINVAL;
	}

	vfs_node_t *target_vnode = NULL;
	r = ext2_lookup(new_dir_node, new_name, new_len, &target_vnode);
	if (r == 0) {
		ext2_node_t *target = target_vnode->private_data;
		if (target_vnode->ino == source_vnode->ino) {
			vfs_node_release(target_vnode);
			vfs_node_release(source_vnode);
			return 0;
		}
		if (VFS_S_ISDIR(source_vnode->mode) != VFS_S_ISDIR(target_vnode->mode)) {
			r = VFS_S_ISDIR(source_vnode->mode) ? -ENOTDIR : -EISDIR;
			vfs_node_release(target_vnode);
			vfs_node_release(source_vnode);
			return r;
		}
		if (VFS_S_ISDIR(target_vnode->mode)) {
			r = dir_is_empty(target);
			if (r < 0) {
				vfs_node_release(target_vnode);
				vfs_node_release(source_vnode);
				return r;
			}
			if (!r) {
				vfs_node_release(target_vnode);
				vfs_node_release(source_vnode);
				return -ENOTEMPTY;
			}
			target->inode.links_count = 0;
			target_vnode->nlink = 0;
			r = write_inode(target->fs, target->ino, &target->inode);
			if (r != 0) {
				vfs_node_release(target_vnode);
				vfs_node_release(source_vnode);
				return r;
			}
			if (new_dir != old_dir) {
				if (new_dir->inode.links_count == 0) {
					vfs_node_release(target_vnode);
					vfs_node_release(source_vnode);
					return -EINVAL;
				}
				new_dir->inode.links_count--;
				new_dir_node->nlink = new_dir->inode.links_count;
				r = write_inode(new_dir->fs, new_dir->ino, &new_dir->inode);
				if (r != 0) {
					vfs_node_release(target_vnode);
					vfs_node_release(source_vnode);
					return r;
				}
			}
		} else {
			if (target->inode.links_count > 0)
				target->inode.links_count--;
			target_vnode->nlink = target->inode.links_count;
			r = write_inode(target->fs, target->ino, &target->inode);
			if (r != 0) {
				vfs_node_release(target_vnode);
				vfs_node_release(source_vnode);
				return r;
			}
		}

		r = remove_dirent(new_dir, new_name, new_len);
		vfs_node_release(target_vnode);
		if (r != 0) {
			vfs_node_release(source_vnode);
			return r;
		}
	} else if (r != -ENOENT) {
		vfs_node_release(source_vnode);
		return r;
	}

	r = add_dirent(new_dir, new_name, new_len, source->ino,
				   file_type_for_mode(source_vnode->mode));
	if (r != 0) {
		vfs_node_release(source_vnode);
		return r;
	}

	r = remove_dirent(old_dir, old_name, old_len);
	if (r != 0) {
		remove_dirent(new_dir, new_name, new_len);
		vfs_node_release(source_vnode);
		return r;
	}

	if (VFS_S_ISDIR(source_vnode->mode) && old_dir != new_dir) {
		r = replace_dirent_ino(source, "..", 2, new_dir->ino, 2);
		if (r != 0) {
			vfs_node_release(source_vnode);
			return r;
		}
		if (old_dir->inode.links_count == 0) {
			vfs_node_release(source_vnode);
			return -EINVAL;
		}
		old_dir->inode.links_count--;
		old_dir_node->nlink = old_dir->inode.links_count;
		r = write_inode(old_dir->fs, old_dir->ino, &old_dir->inode);
		if (r != 0) {
			vfs_node_release(source_vnode);
			return r;
		}
		new_dir->inode.links_count++;
		new_dir_node->nlink = new_dir->inode.links_count;
		r = write_inode(new_dir->fs, new_dir->ino, &new_dir->inode);
		if (r != 0) {
			vfs_node_release(source_vnode);
			return r;
		}
	}

	vfs_node_release(source_vnode);
	return 0;
}

static void ext2_release(vfs_node_t *vnode)
{
	kfree(vnode->private_data);
}

static int ext2_mounts_read(void *ctx, uint64_t off, void *buf, size_t len,
							size_t *done)
{
	(void)ctx;
	char tmp[1024];
	size_t pos = 0;
	for (ext2_fs_t *fs = mounts; fs && pos < sizeof(tmp); fs = fs->next) {
		int n = npf_snprintf(tmp + pos, sizeof(tmp) - pos,
							 "%s %s ext2 rw,auto 0 0\n", fs->dev->name,
							 fs->mount_path);
		if (n < 0)
			break;
		if ((size_t)n >= sizeof(tmp) - pos) {
			pos = sizeof(tmp) - 1;
			break;
		}
		pos += (size_t)n;
	}
	return copy_text_slice(tmp, pos, off, buf, len, done);
}

static int ext2_truncate(vfs_node_t *vnode, uint64_t size)
{
	if (!VFS_S_ISREG(vnode->mode))
		return -EISDIR;
	if (size > vnode->size)
		return 0;
	ext2_node_t *node = vnode->private_data;
	vnode->size = size;
	node->inode.size_lo = (uint32_t)size;
	node->inode.size_high = (uint32_t)(size >> 32);
	return write_inode(node->fs, node->ino, &node->inode);
}

int ext2_mount(block_device_t *dev, const char *path)
{
	if (!dev)
		return -EINVAL;
	for (ext2_fs_t *cur = mounts; cur; cur = cur->next) {
		if (cur->dev == dev)
			return -EEXIST;
	}

	ext2_super_t sb;
	int r = block_read(dev, EXT2_SUPER_OFFSET, &sb, sizeof(sb));
	if (r != 0)
		return r;
	if (sb.magic != EXT2_SUPER_MAGIC)
		return -ENOENT;
	if (sb.feature_incompat & ~(0x2u))
		return -ENOSYS;

	ext2_fs_t *fs = kzalloc(sizeof(*fs));
	if (!fs)
		return -ENOMEM;
	fs->dev = dev;
	fs->dev_id = next_ext2_dev_id++;
	memcpy(&fs->sb, &sb, sizeof(sb));
	fs->block_size = 1024u << sb.log_block_size;
	fs->group_count =
		(sb.blocks_count + sb.blocks_per_group - 1) / sb.blocks_per_group;
	if (fs->block_size < 1024 || fs->block_size > 4096 ||
		fs->group_count == 0) {
		kfree(fs);
		return -EINVAL;
	}

	size_t gd_bytes = fs->group_count * sizeof(ext2_group_desc_t);
	fs->groups = kzalloc(gd_bytes);
	if (!fs->groups) {
		kfree(fs);
		return -ENOMEM;
	}
	fs->gd_off = (fs->block_size == 1024) ? 2048 : fs->block_size;
	r = block_read(dev, fs->gd_off, fs->groups, gd_bytes);
	if (r != 0) {
		kfree(fs->groups);
		kfree(fs);
		return r;
	}

	r = alloc_ext2_node(fs, EXT2_ROOT_INO, &fs->root);
	if (r != 0) {
		kfree(fs->groups);
		kfree(fs);
		return r;
	}

	if (path && path[0]) {
		size_t len = strlen(path);
		if (len >= sizeof(fs->mount_path)) {
			vfs_node_release(fs->root);
			kfree(fs->groups);
			kfree(fs);
			return -ENAMETOOLONG;
		}
		memcpy(fs->mount_path, path, len + 1);
	} else if (mount_count == 0) {
		npf_snprintf(fs->mount_path, sizeof(fs->mount_path), "/mnt");
	} else {
		npf_snprintf(fs->mount_path, sizeof(fs->mount_path), "/mnt-%s",
					 dev->name);
	}
	r = vfs_mkdir(fs->mount_path, 0755, &vfs_root_cred);
	if (r != 0 && r != -EEXIST) {
		vfs_node_release(fs->root);
		kfree(fs->groups);
		kfree(fs);
		return r;
	}
	r = vfs_mount(fs->mount_path, fs->root, &vfs_root_cred);
	if (r != 0) {
		vfs_node_release(fs->root);
		kfree(fs->groups);
		kfree(fs);
		return r;
	}
	fs->next = mounts;
	mounts = fs;
	mount_count++;
	publish_mounts();
	log_debug("ext2", "mounted %s on %s block_size=%u groups=%u", dev->name,
			  fs->mount_path, fs->block_size, fs->group_count);
	return 0;
}
