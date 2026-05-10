#include <fs/cpio.h>
#include <fs/vfs.h>
#include <debug/log.h>
#include <mm/heap.h>
#include <lib/string.h>

#define CPIO_NEWC_HEADER_SIZE 110
#define CPIO_NEWC_MAGIC "070701"
#define CPIO_TRAILER "TRAILER!!!"

typedef struct {
	char magic[6];
	char ino[8];
	char mode[8];
	char uid[8];
	char gid[8];
	char nlink[8];
	char mtime[8];
	char filesize[8];
	char devmajor[8];
	char devminor[8];
	char rdevmajor[8];
	char rdevminor[8];
	char namesize[8];
	char check[8];
} cpio_newc_header_t;

static size_t _align4(size_t v)
{
	return (v + 3) & ~(size_t)3;
}

static int _hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int _hex8(const char in[8], uint32_t *out)
{
	uint32_t v = 0;
	for (int i = 0; i < 8; i++) {
		int n = _hex_nibble(in[i]);
		if (n < 0)
			return -EINVAL;
		v = (v << 4) | (uint32_t)n;
	}
	*out = v;
	return 0;
}

static const char *_clean_name(const char *name)
{
	while (name[0] == '.' && name[1] == '/')
		name += 2;
	while (*name == '/')
		name++;
	return name;
}

static int _make_path(const char *name, char **out)
{
	name = _clean_name(name);
	if (*name == '\0')
		return -EINVAL;

	size_t len = strlen(name);
	char *path = kmalloc(len + 2);
	if (!path)
		return -ENOMEM;
	path[0] = '/';
	memcpy(path + 1, name, len + 1);
	*out = path;
	return 0;
}

static int _ensure_parent_dirs(const char *path)
{
	size_t len = strlen(path);
	if (len < 2)
		return 0;

	char *tmp = kmalloc(len + 1);
	if (!tmp)
		return -ENOMEM;
	memcpy(tmp, path, len + 1);

	for (size_t i = 1; tmp[i]; i++) {
		if (tmp[i] != '/')
			continue;
		tmp[i] = '\0';
		int r = vfs_mkdir(tmp, 0755, &vfs_root_cred);
		if (r != 0 && r != -EEXIST) {
			kfree(tmp);
			return r;
		}
		tmp[i] = '/';
	}

	kfree(tmp);
	return 0;
}

static int _extract_dir(const char *path, vfs_mode_t mode, vfs_uid_t uid,
						vfs_gid_t gid)
{
	log_debug("cpio", "dir  %s mode=0%o uid=%u gid=%u", path, mode & VFS_S_PERM,
			  uid, gid);
	int r = vfs_mkdir(path, mode & VFS_S_PERM, &vfs_root_cred);
	if (r != 0 && r != -EEXIST)
		return r;
	r = vfs_chown(path, uid, gid, &vfs_root_cred);
	if (r != 0)
		return r;
	return vfs_chmod(path, mode & VFS_S_PERM, &vfs_root_cred);
}

static int _extract_file(const char *path, vfs_mode_t mode, vfs_uid_t uid,
						 vfs_gid_t gid, const void *data, size_t size)
{
	log_debug("cpio", "file %s size=%zu mode=0%o uid=%u gid=%u", path, size,
			  mode & VFS_S_PERM, uid, gid);
	vfs_file_t *file = NULL;
	int r = vfs_open(path, VFS_O_CREAT | VFS_O_TRUNC | VFS_O_RDWR,
					 mode & VFS_S_PERM, &vfs_root_cred, &file);
	if (r != 0)
		return r;

	size_t done = 0;
	if (size > 0)
		r = vfs_write(file, data, size, &done);
	int close_r = vfs_close(file);
	if (r == 0 && close_r != 0)
		r = close_r;
	if (r != 0)
		return r;
	if (done != size)
		return -ENOSYS;

	r = vfs_chown(path, uid, gid, &vfs_root_cred);
	if (r != 0)
		return r;
	return vfs_chmod(path, mode & VFS_S_PERM, &vfs_root_cred);
}

int cpio_newc_extract(const void *archive, size_t size, size_t *entries_out)
{
	if (entries_out)
		*entries_out = 0;
	if (!archive)
		return -EINVAL;

	const uint8_t *base = archive;
	size_t off = 0;
	size_t entries = 0;

	log_debug("cpio", "extract newc archive addr=%p size=%zu", archive, size);

	while (off + CPIO_NEWC_HEADER_SIZE <= size) {
		const cpio_newc_header_t *hdr =
			(const cpio_newc_header_t *)(const void *)(base + off);
		if (memcmp(hdr->magic, CPIO_NEWC_MAGIC, 6) != 0)
			return -EINVAL;

		uint32_t mode;
		uint32_t uid;
		uint32_t gid;
		uint32_t filesize;
		uint32_t namesize;
		int r = _hex8(hdr->mode, &mode);
		if (r != 0)
			return r;
		if ((r = _hex8(hdr->uid, &uid)) != 0)
			return r;
		if ((r = _hex8(hdr->gid, &gid)) != 0)
			return r;
		if ((r = _hex8(hdr->filesize, &filesize)) != 0)
			return r;
		if ((r = _hex8(hdr->namesize, &namesize)) != 0)
			return r;
		if (namesize == 0)
			return -EINVAL;

		off += CPIO_NEWC_HEADER_SIZE;
		if (off + namesize > size)
			return -EINVAL;

		const char *name = (const char *)(const void *)(base + off);
		if (name[namesize - 1] != '\0')
			return -EINVAL;
		off = _align4(off + namesize);
		if (off > size || off + filesize > size)
			return -EINVAL;

		if (strcmp(name, CPIO_TRAILER) == 0) {
			log_debug("cpio", "trailer after %zu entries", entries);
			if (entries_out)
				*entries_out = entries;
			return 0;
		}

		log_trace("cpio", "entry name=%s mode=0%o size=%u", name, mode,
				  filesize);

		char *path = NULL;
		r = _make_path(name, &path);
		if (r != 0)
			return r;

		r = _ensure_parent_dirs(path);
		if (r == 0 && VFS_S_ISDIR(mode)) {
			r = _extract_dir(path, mode, uid, gid);
		} else if (r == 0 && VFS_S_ISREG(mode)) {
			r = _extract_file(path, mode, uid, gid, base + off, filesize);
		}

		kfree(path);
		if (r != 0)
			return r;
		if (VFS_S_ISDIR(mode) || VFS_S_ISREG(mode))
			entries++;

		off = _align4(off + filesize);
	}

	return -EINVAL;
}
