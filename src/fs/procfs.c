#include <fs/procfs.h>
#include <debug/log.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sched/sched.h>

extern int npf_snprintf_(char *buffer, size_t bufsz, const char *format, ...);

#define PROCFS_DEV 0x70726f63ULL

typedef enum procfs_kind {
	PROCFS_ROOT,
	PROCFS_PID_DIR,
	PROCFS_FILE_STATUS,
	PROCFS_FILE_STAT,
	PROCFS_FILE_CMDLINE,
	PROCFS_FILE_COMM,
	PROCFS_FILE_CWD,
} procfs_kind_t;

typedef struct procfs_node {
	vfs_node_t vnode;
	procfs_kind_t kind;
	pid_t pid;
} procfs_node_t;

static int procfs_lookup(vfs_node_t *dir, const char *name, size_t len,
						 vfs_node_t **out);
static int procfs_read(vfs_node_t *node, uint64_t off, void *buf, size_t len,
					   size_t *done);
static int procfs_readdir(vfs_node_t *dir, size_t index, vfs_dirent_t *out);
static void procfs_release(vfs_node_t *node);

static const vfs_ops_t procfs_ops = {
	.lookup = procfs_lookup,
	.read = procfs_read,
	.readdir = procfs_readdir,
	.release = procfs_release,
};

static procfs_node_t *procfs_to_node(vfs_node_t *node)
{
	return (procfs_node_t *)node;
}

static int name_eq(const char *name, size_t len, const char *lit)
{
	return strlen(lit) == len && memcmp(name, lit, len) == 0;
}

static int parse_pid(const char *name, size_t len, pid_t *out)
{
	if (!name || !len || !out)
		return 0;

	pid_t pid = 0;
	for (size_t i = 0; i < len; i++) {
		char c = name[i];
		if (c < '0' || c > '9')
			return 0;
		if (pid > (INT32_MAX - (c - '0')) / 10)
			return 0;
		pid = (pid_t)(pid * 10 + (c - '0'));
	}

	*out = pid;
	return 1;
}

static void pid_to_name(pid_t pid, char out[VFS_NAME_MAX + 1])
{
	char tmp[16];
	size_t n = 0;

	if (pid == 0) {
		out[0] = '0';
		out[1] = '\0';
		return;
	}

	while (pid > 0 && n < sizeof(tmp)) {
		tmp[n++] = (char)('0' + (pid % 10));
		pid /= 10;
	}

	for (size_t i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	out[n] = '\0';
}

static pid_t effective_pid(pid_t pid)
{
	if (pid >= 0)
		return pid;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return -1;

	return thread->process->pid;
}

static int proc_info_for(pid_t pid, sched_process_info_t *info)
{
	pid = effective_pid(pid);
	if (pid < 0)
		return 0;
	return sched_process_get_info(pid, info);
}

static procfs_node_t *alloc_node(procfs_kind_t kind, pid_t pid, vfs_mode_t mode)
{
	procfs_node_t *node = kzalloc(sizeof(*node));
	if (!node)
		return NULL;

	vfs_node_init(&node->vnode, &procfs_ops, mode, 0, 0);
	node->vnode.dev = PROCFS_DEV;
	node->vnode.private_data = node;
	node->kind = kind;
	node->pid = pid;

	if (VFS_S_ISDIR(mode))
		node->vnode.nlink = 2;
	else
		node->vnode.nlink = 1;

	return node;
}

vfs_node_t *procfs_create_root(void)
{
	procfs_node_t *root = alloc_node(PROCFS_ROOT, -1, VFS_S_IFDIR | 0555);
	if (!root)
		return NULL;
	log_debug("procfs", "created root");
	return &root->vnode;
}

int procfs_mount(const char *target)
{
	vfs_node_t *root = procfs_create_root();
	if (!root)
		return VFS_ERR_NOMEM;

	int r = vfs_mount(target, root, &vfs_root_cred);
	vfs_node_release(root);
	return r;
}

static int lookup_pid_root(const char *name, size_t len, vfs_node_t **out)
{
	pid_t pid;

	if (name_eq(name, len, "self")) {
		pid = effective_pid(-1);
		if (pid < 0)
			return VFS_ERR_NOENT;
	} else if (!parse_pid(name, len, &pid)) {
		return VFS_ERR_NOENT;
	}

	sched_process_info_t info;
	if (!sched_process_get_info(pid, &info))
		return VFS_ERR_NOENT;

	procfs_node_t *node = alloc_node(PROCFS_PID_DIR, pid, VFS_S_IFDIR | 0555);
	if (!node)
		return VFS_ERR_NOMEM;

	*out = &node->vnode;
	return VFS_OK;
}

static int lookup_pid_file(procfs_node_t *dir, const char *name, size_t len,
						   vfs_node_t **out)
{
	procfs_kind_t kind;

	if (name_eq(name, len, "status"))
		kind = PROCFS_FILE_STATUS;
	else if (name_eq(name, len, "stat"))
		kind = PROCFS_FILE_STAT;
	else if (name_eq(name, len, "cmdline"))
		kind = PROCFS_FILE_CMDLINE;
	else if (name_eq(name, len, "comm"))
		kind = PROCFS_FILE_COMM;
	else if (name_eq(name, len, "cwd"))
		kind = PROCFS_FILE_CWD;
	else
		return VFS_ERR_NOENT;

	sched_process_info_t info;
	if (!proc_info_for(dir->pid, &info))
		return VFS_ERR_NOENT;

	procfs_node_t *node = alloc_node(kind, dir->pid, VFS_S_IFREG | 0444);
	if (!node)
		return VFS_ERR_NOMEM;

	*out = &node->vnode;
	return VFS_OK;
}

static int procfs_lookup(vfs_node_t *dir_node, const char *name, size_t len,
						 vfs_node_t **out)
{
	if (!dir_node || !name || !out)
		return VFS_ERR_INVAL;
	if (!VFS_S_ISDIR(dir_node->mode))
		return VFS_ERR_NOTDIR;

	procfs_node_t *dir = procfs_to_node(dir_node);

	if (len == 1 && name[0] == '.') {
		vfs_node_ref(dir_node);
		*out = dir_node;
		return VFS_OK;
	}
	if (len == 2 && name[0] == '.' && name[1] == '.') {
		if (dir->kind == PROCFS_ROOT) {
			vfs_node_ref(dir_node);
			*out = dir_node;
			return VFS_OK;
		}
		procfs_node_t *root = alloc_node(PROCFS_ROOT, -1, VFS_S_IFDIR | 0555);
		if (!root)
			return VFS_ERR_NOMEM;
		*out = &root->vnode;
		return VFS_OK;
	}

	switch (dir->kind) {
	case PROCFS_ROOT:
		return lookup_pid_root(name, len, out);
	case PROCFS_PID_DIR:
		return lookup_pid_file(dir, name, len, out);
	default:
		return VFS_ERR_NOTDIR;
	}
}

static void dirent_fill(vfs_dirent_t *out, const char *name, vfs_mode_t mode)
{
	size_t len = strlen(name);
	if (len > VFS_NAME_MAX)
		len = VFS_NAME_MAX;
	memcpy(out->name, name, len);
	out->name[len] = '\0';
	out->mode = mode;
	out->uid = 0;
	out->gid = 0;
	out->size = 0;
	out->nlink = VFS_S_ISDIR(mode) ? 2 : 1;
}

static int procfs_root_readdir(size_t index, vfs_dirent_t *out)
{
	if (index == 0) {
		dirent_fill(out, ".", VFS_S_IFDIR | 0555);
		return VFS_OK;
	}
	if (index == 1) {
		dirent_fill(out, "..", VFS_S_IFDIR | 0555);
		return VFS_OK;
	}
	if (index == 2) {
		dirent_fill(out, "self", VFS_S_IFDIR | 0555);
		return VFS_OK;
	}

	sched_process_info_t info;
	if (!sched_process_get_nth(index - 3, &info))
		return VFS_ERR_NOENT;

	char name[VFS_NAME_MAX + 1];
	pid_to_name(info.pid, name);
	dirent_fill(out, name, VFS_S_IFDIR | 0555);
	return VFS_OK;
}

static int procfs_pid_readdir(procfs_node_t *dir, size_t index,
						  vfs_dirent_t *out)
{
	sched_process_info_t info;
	if (!proc_info_for(dir->pid, &info))
		return VFS_ERR_NOENT;

	switch (index) {
	case 0:
		dirent_fill(out, ".", VFS_S_IFDIR | 0555);
		return VFS_OK;
	case 1:
		dirent_fill(out, "..", VFS_S_IFDIR | 0555);
		return VFS_OK;
	case 2:
		dirent_fill(out, "status", VFS_S_IFREG | 0444);
		return VFS_OK;
	case 3:
		dirent_fill(out, "stat", VFS_S_IFREG | 0444);
		return VFS_OK;
	case 4:
		dirent_fill(out, "cmdline", VFS_S_IFREG | 0444);
		return VFS_OK;
	case 5:
		dirent_fill(out, "comm", VFS_S_IFREG | 0444);
		return VFS_OK;
	case 6:
		dirent_fill(out, "cwd", VFS_S_IFREG | 0444);
		return VFS_OK;
	default:
		return VFS_ERR_NOENT;
	}
}

static int procfs_readdir(vfs_node_t *dir_node, size_t index, vfs_dirent_t *out)
{
	if (!dir_node || !out)
		return VFS_ERR_INVAL;
	if (!VFS_S_ISDIR(dir_node->mode))
		return VFS_ERR_NOTDIR;

	procfs_node_t *dir = procfs_to_node(dir_node);
	if (dir->kind == PROCFS_ROOT)
		return procfs_root_readdir(index, out);
	if (dir->kind == PROCFS_PID_DIR)
		return procfs_pid_readdir(dir, index, out);
	return VFS_ERR_NOTDIR;
}

static char proc_state(const sched_process_info_t *info)
{
	if (info->zombie)
		return 'Z';
	if (info->dying)
		return 'X';
	return 'R';
}

static size_t build_status(const sched_process_info_t *info, char *buf,
					   size_t size)
{
	int n = npf_snprintf(
		buf, size,
		"Name:\t%s\nState:\t%c\nPid:\t%d\nPPid:\t%d\nThreads:\t%u\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nCwd:\t%s\n",
		info->name, proc_state(info), info->pid, info->ppid,
		info->thread_count, info->ruid, info->euid, info->ruid, info->euid,
		info->rgid, info->egid, info->rgid, info->egid, info->cwd);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

static size_t build_stat(const sched_process_info_t *info, char *buf,
					 size_t size)
{
	int n = npf_snprintf(
		buf, size,
		"%d (%s) %c %d 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %u 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %d\n",
		info->pid, info->name, proc_state(info), info->ppid,
		info->thread_count, info->exit_status);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

static size_t build_content(procfs_node_t *node, char *buf, size_t size)
{
	sched_process_info_t info;
	if (!proc_info_for(node->pid, &info))
		return 0;

	switch (node->kind) {
	case PROCFS_FILE_STATUS:
		return build_status(&info, buf, size);
	case PROCFS_FILE_STAT:
		return build_stat(&info, buf, size);
	case PROCFS_FILE_CMDLINE: {
		size_t len = strlen(info.name);
		if (len + 1 > size)
			len = size ? size - 1 : 0;
		if (len)
			memcpy(buf, info.name, len);
		if (size)
			buf[len++] = '\0';
		return len;
	}
	case PROCFS_FILE_COMM: {
		int n = npf_snprintf(buf, size, "%s\n", info.name);
		if (n < 0)
			return 0;
		if ((size_t)n >= size)
			return size ? size - 1 : 0;
		return (size_t)n;
	}
	case PROCFS_FILE_CWD: {
		int n = npf_snprintf(buf, size, "%s\n", info.cwd);
		if (n < 0)
			return 0;
		if ((size_t)n >= size)
			return size ? size - 1 : 0;
		return (size_t)n;
	}
	default:
		return 0;
	}
}

static int procfs_read(vfs_node_t *node, uint64_t off, void *buf, size_t len,
					   size_t *done)
{
	if (done)
		*done = 0;
	if (!node || !buf)
		return VFS_ERR_INVAL;
	if (VFS_S_ISDIR(node->mode))
		return VFS_ERR_ISDIR;

	char tmp[1024];
	procfs_node_t *pn = procfs_to_node(node);
	size_t total = build_content(pn, tmp, sizeof(tmp));

	if (!total && (pn->kind == PROCFS_FILE_STATUS || pn->kind == PROCFS_FILE_STAT ||
				 pn->kind == PROCFS_FILE_CMDLINE || pn->kind == PROCFS_FILE_COMM ||
				 pn->kind == PROCFS_FILE_CWD))
		return VFS_ERR_NOENT;

	if (off >= total || len == 0)
		return VFS_OK;

	size_t n = total - (size_t)off;
	if (n > len)
		n = len;
	memcpy(buf, tmp + off, n);
	if (done)
		*done = n;
	return VFS_OK;
}

static void procfs_release(vfs_node_t *node)
{
	kfree(node);
}
