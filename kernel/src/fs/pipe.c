#include <fs/pipe.h>

#include <errno.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sched/sched.h>
#include <sync/spinlock.h>
#include <sys/poll.h>

#define PIPE_INITIAL_CAPACITY 4096
#define PIPE_READER 1
#define PIPE_WRITER 2

typedef struct pipe_shared {
	spinlock_t lock;
	sched_waitq_t waitq;
	uint8_t *buffer;
	size_t capacity;
	uint64_t write_offset;
	unsigned readers;
	unsigned writers;
	unsigned nodes;
} pipe_shared_t;

typedef struct pipe_endpoint {
	vfs_node_t node;
	pipe_shared_t *shared;
	int side;
} pipe_endpoint_t;

static int pipe_wait_interruptible(pipe_shared_t *shared, unsigned seq)
{
	return sched_waitq_wait(&shared->waitq, seq, NULL);
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
	sched_waitq_wake_all(&shared->waitq);
	sched_io_wake_all();
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
		if (file->offset < shared->write_offset || shared->writers == 0)
			revents |= LYR_POLLIN | LYR_POLLRDNORM;
		if (shared->writers == 0)
			revents |= LYR_POLLHUP;
	}

	if (events & LYR_POLL_WRITE_MASK) {
		if (shared->readers == 0)
			revents |= LYR_POLLERR;
		else
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
		if (free_shared) {
			kfree(shared->buffer);
			kfree(shared);
		}
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
	sched_waitq_init(&shared->waitq);
	shared->capacity = PIPE_INITIAL_CAPACITY;
	shared->buffer = kzalloc(shared->capacity + 1);
	if (!shared->buffer) {
		kfree(shared);
		return -ENOMEM;
	}
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
	rf->refs = 1;
	wf->node = &writer->node;
	wf->flags = VFS_O_WRONLY;
	wf->refs = 1;

	*read_end = rf;
	*write_end = wf;
	return 0;
}

int vfs_pipe_is(vfs_file_t *file)
{
	return file && file->node && file->node->ops == &pipe_ops;
}

int vfs_pipe_ref(vfs_file_t *file)
{
	pipe_endpoint_t *endpoint = pipe_endpoint_from_node(file ? file->node : NULL);
	if (!endpoint || !endpoint->shared)
		return -EBADF;

	pipe_shared_t *shared = endpoint->shared;
	spinlock_acquire(&shared->lock);
	if (endpoint->side == PIPE_READER)
		shared->readers++;
	else if (endpoint->side == PIPE_WRITER)
		shared->writers++;
	else {
		spinlock_release(&shared->lock);
		return -EBADF;
	}
	spinlock_release(&shared->lock);
	return 0;
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
		unsigned wait_seq = sched_waitq_prepare(&shared->waitq);
		spinlock_acquire(&shared->lock);

		int writers = (int)shared->writers;
		uint64_t write_offset = shared->write_offset;

		uint64_t available =
			file->offset < write_offset ? write_offset - file->offset : 0;
		if (available > 0) {
			size_t chunk = len - total;
			if ((uint64_t)chunk > available)
				chunk = (size_t)available;
			memcpy(out + total, shared->buffer + (size_t)file->offset, chunk);
			total += chunk;
			file->offset += chunk;
		}

		spinlock_release(&shared->lock);

		if (total > 0 || (writers == 0 && file->offset >= write_offset)) {
			if (done)
				*done = total;
			return 0;
		}

		if (file->flags & 0x800u)
			return -EAGAIN;

		int r = pipe_wait_interruptible(shared, wait_seq);
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
		size_t start = (size_t)file->offset;
		size_t need = start + (len - total);
		if (need > shared->capacity) {
			size_t new_cap = shared->capacity ? shared->capacity : PIPE_INITIAL_CAPACITY;
			while (new_cap < need)
				new_cap *= 2;
			uint8_t *new_buf = krealloc(shared->buffer, new_cap + 1);
			if (!new_buf) {
				spinlock_release(&shared->lock);
				if (total > 0 && done)
					*done = total;
				return total > 0 ? 0 : -ENOMEM;
			}
			if (new_cap > shared->capacity)
				memset(new_buf + shared->capacity, 0,
					   (new_cap + 1) - shared->capacity);
			shared->buffer = new_buf;
			shared->capacity = new_cap;
		}

		size_t chunk = len - total;
		if ((uint64_t)start > shared->write_offset) {
			memset(shared->buffer + (size_t)shared->write_offset, 0,
				   start - (size_t)shared->write_offset);
		}
		memcpy(shared->buffer + start, in + total, chunk);
		total += chunk;
		file->offset += chunk;
		if (file->offset > shared->write_offset)
			shared->write_offset = file->offset;
		shared->buffer[(size_t)shared->write_offset] = 0;

		spinlock_release(&shared->lock);
		sched_waitq_wake_all(&shared->waitq);
		sched_io_wake_all();

		if (done)
			*done = total;
		return 0;
	}
}

int vfs_pipe_seek(vfs_file_t *file, int whence, int64_t off, uint64_t *new_off)
{
	pipe_endpoint_t *endpoint = pipe_endpoint_from_node(file ? file->node : NULL);
	if (!endpoint || !endpoint->shared)
		return -EBADF;

	pipe_shared_t *shared = endpoint->shared;
	spinlock_acquire(&shared->lock);

	uint64_t base;
	switch (whence) {
	case VFS_SEEK_SET:
		base = 0;
		break;
	case VFS_SEEK_CUR:
		base = file->offset;
		break;
	case VFS_SEEK_END:
		base = shared->write_offset;
		break;
	default:
		spinlock_release(&shared->lock);
		return -EINVAL;
	}

	if (off < 0 && (uint64_t)(-off) > base) {
		spinlock_release(&shared->lock);
		return -EINVAL;
	}

	file->offset = off < 0 ? base - (uint64_t)(-off) : base + (uint64_t)off;
	if (new_off)
		*new_off = file->offset;

	spinlock_release(&shared->lock);
	return 0;
}
