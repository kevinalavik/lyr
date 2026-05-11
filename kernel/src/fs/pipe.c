#include <fs/pipe.h>

#include <errno.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sched/sched.h>
#include <sync/spinlock.h>
#include <sys/poll.h>

#define PIPE_CAPACITY 4096
#define PIPE_READER 1
#define PIPE_WRITER 2

typedef struct pipe_shared {
	spinlock_t lock;
	size_t head;
	size_t tail;
	size_t count;
	unsigned readers;
	unsigned writers;
	unsigned nodes;
	uint8_t buffer[PIPE_CAPACITY];
} pipe_shared_t;

typedef struct pipe_endpoint {
	vfs_node_t node;
	pipe_shared_t *shared;
	int side;
} pipe_endpoint_t;

static int pipe_wait_interruptible(void)
{
	for (;;) {
		tcb_t *thread = sched_current();
		if (thread && sched_signal_is_pending(thread))
			return -EINTR;
		__asm__ volatile("sti; hlt; cli" ::: "memory");
	}
}

static pipe_endpoint_t *pipe_endpoint_from_node(vfs_node_t *node)
{
	return node ? (pipe_endpoint_t *)node : NULL;
}

static int pipe_close(vfs_file_t *file)
{
	pipe_endpoint_t *endpoint = pipe_endpoint_from_node(file ? file->node : NULL);
	if (!endpoint || !endpoint->shared)
		return -EBADF;

	pipe_shared_t *shared = endpoint->shared;
	spinlock_acquire(&shared->lock);
	if (endpoint->side == PIPE_READER) {
		if (shared->readers > 0)
			shared->readers--;
	} else {
		if (shared->writers > 0)
			shared->writers--;
	}
	spinlock_release(&shared->lock);
	return 0;
}

static int pipe_poll(vfs_file_t *file, int events)
{
	pipe_endpoint_t *endpoint = pipe_endpoint_from_node(file ? file->node : NULL);
	if (!endpoint || !endpoint->shared)
		return 0;

	int revents = 0;
	pipe_shared_t *shared = endpoint->shared;
	spinlock_acquire(&shared->lock);

	if (events & LYR_POLL_READ_MASK) {
		if (shared->count > 0 || shared->writers == 0)
			revents |= LYR_POLLIN | LYR_POLLRDNORM;
	}

	if (events & LYR_POLL_WRITE_MASK) {
		if (shared->readers == 0)
			revents |= LYR_POLLERR;
		else if (shared->count < PIPE_CAPACITY)
			revents |= LYR_POLLOUT | LYR_POLLWRNORM;
	}

	spinlock_release(&shared->lock);
	return revents;
}

static void pipe_release(vfs_node_t *node)
{
	pipe_endpoint_t *endpoint = pipe_endpoint_from_node(node);
	if (!endpoint)
		return;

	pipe_shared_t *shared = endpoint->shared;
	if (shared) {
		int free_shared = 0;
		spinlock_acquire(&shared->lock);
		if (shared->nodes > 0)
			shared->nodes--;
		free_shared = shared->nodes == 0;
		spinlock_release(&shared->lock);
		if (free_shared)
			kfree(shared);
	}

	kfree(endpoint);
}

static const vfs_ops_t pipe_ops = {
	.close = pipe_close,
	.poll = pipe_poll,
	.release = pipe_release,
};

static pipe_endpoint_t *pipe_endpoint_create(pipe_shared_t *shared, int side)
{
	pipe_endpoint_t *endpoint = kzalloc(sizeof(*endpoint));
	if (!endpoint)
		return NULL;

	vfs_node_init(&endpoint->node, &pipe_ops, VFS_S_IFIFO | 0666, 0, 0);
	endpoint->shared = shared;
	endpoint->side = side;
	return endpoint;
}

int vfs_pipe_create(vfs_file_t **read_end, vfs_file_t **write_end)
{
	if (!read_end || !write_end)
		return -EINVAL;

	*read_end = NULL;
	*write_end = NULL;

	pipe_shared_t *shared = kzalloc(sizeof(*shared));
	if (!shared)
		return -ENOMEM;

	spinlock_init(&shared->lock);
	shared->readers = 1;
	shared->writers = 1;
	shared->nodes = 2;

	pipe_endpoint_t *reader = pipe_endpoint_create(shared, PIPE_READER);
	if (!reader) {
		kfree(shared);
		return -ENOMEM;
	}

	pipe_endpoint_t *writer = pipe_endpoint_create(shared, PIPE_WRITER);
	if (!writer) {
		vfs_node_release(&reader->node);
		return -ENOMEM;
	}

	vfs_file_t *rf = kzalloc(sizeof(*rf));
	if (!rf) {
		vfs_node_release(&reader->node);
		vfs_node_release(&writer->node);
		return -ENOMEM;
	}

	vfs_file_t *wf = kzalloc(sizeof(*wf));
	if (!wf) {
		kfree(rf);
		vfs_node_release(&reader->node);
		vfs_node_release(&writer->node);
		return -ENOMEM;
	}

	rf->node = &reader->node;
	rf->flags = VFS_O_RDONLY;
	wf->node = &writer->node;
	wf->flags = VFS_O_WRONLY;

	*read_end = rf;
	*write_end = wf;
	return 0;
}

int vfs_pipe_is(vfs_file_t *file)
{
	return file && file->node && file->node->ops == &pipe_ops;
}

int vfs_pipe_read(vfs_file_t *file, void *buf, size_t len, size_t *done)
{
	pipe_endpoint_t *endpoint = pipe_endpoint_from_node(file ? file->node : NULL);
	if (done)
		*done = 0;
	if (!endpoint || !endpoint->shared || endpoint->side != PIPE_READER || !buf)
		return -EBADF;
	if (len == 0)
		return 0;

	pipe_shared_t *shared = endpoint->shared;
	uint8_t *out = (uint8_t *)buf;
	size_t total = 0;

	for (;;) {
		spinlock_acquire(&shared->lock);

		while (total < len && shared->count > 0) {
			out[total++] = shared->buffer[shared->tail];
			shared->tail = (shared->tail + 1) % PIPE_CAPACITY;
			shared->count--;
		}

		int writers = (int)shared->writers;
		spinlock_release(&shared->lock);

		if (total > 0 || writers == 0) {
			if (done)
				*done = total;
			return 0;
		}

		if (file->flags & 0x800u)
			return -EAGAIN;

		int r = pipe_wait_interruptible();
		if (r != 0)
			return r;
	}
}

int vfs_pipe_write(vfs_file_t *file, const void *buf, size_t len, size_t *done)
{
	pipe_endpoint_t *endpoint = pipe_endpoint_from_node(file ? file->node : NULL);
	if (done)
		*done = 0;
	if (!endpoint || !endpoint->shared || endpoint->side != PIPE_WRITER || !buf)
		return -EBADF;
	if (len == 0)
		return 0;

	pipe_shared_t *shared = endpoint->shared;
	const uint8_t *in = (const uint8_t *)buf;
	size_t total = 0;

	for (;;) {
		spinlock_acquire(&shared->lock);

		if (shared->readers == 0) {
			spinlock_release(&shared->lock);
			return -EPIPE;
		}

		while (total < len && shared->count < PIPE_CAPACITY) {
			shared->buffer[shared->head] = in[total++];
			shared->head = (shared->head + 1) % PIPE_CAPACITY;
			shared->count++;
		}

		spinlock_release(&shared->lock);

		if (total == len) {
			if (done)
				*done = total;
			return 0;
		}

		if (file->flags & 0x800u) {
			if (total > 0 && done)
				*done = total;
			return total > 0 ? 0 : -EAGAIN;
		}

		int r = pipe_wait_interruptible();
		if (r != 0) {
			if (total > 0 && done)
				*done = total;
			return total > 0 ? 0 : r;
		}
	}
}
