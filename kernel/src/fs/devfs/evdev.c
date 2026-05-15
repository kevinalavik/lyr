/*
 * Linux-style evdev interface: native keyboard and mouse records are
 * translated into Linux input_event packets for userspace consumers.
 */

#include <fs/evdev.h>

#include <debug/log.h>
#include <errno.h>
#include <stdbool.h>
#include <fs/devfs.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sched/sched.h>
#include <sync/spinlock.h>
#include <sys/poll.h>

#define EVDEV_RING_SIZE 256u

struct evdev {
	lyr_input_event_t ring[EVDEV_RING_SIZE];
	volatile uint32_t head;
	volatile uint32_t tail;
	sched_waitq_t waitq;
	spinlock_t lock;
	evdev_kind_t kind;
	lyr_input_id_t id;
	const char *name;
	const char *phys;
	const char *uniq;
	unsigned int repeat[2];
	uint8_t supported_events[LYR_INPUT_BITSET_BYTES];
	uint8_t supported_keys[LYR_INPUT_BITSET_BYTES];
	uint8_t current_keys[LYR_INPUT_BITSET_BYTES];
	uint8_t supported_rels[LYR_INPUT_BITSET_BYTES];
	uint8_t supported_abs[LYR_INPUT_BITSET_BYTES];
	uint8_t supported_props[LYR_INPUT_BITSET_BYTES];
	bool grabbed;
	evdev_ioctl_t ioctl;
	void *ioctl_ctx;
	uint32_t mouse_buttons;
};

static int evdev_read_op(void *ctx, uint64_t off, void *buf, size_t len,
						 size_t *done);
static int evdev_poll_op(void *ctx, int events);
static int evdev_ioctl_op(void *ctx, unsigned long request, void *arg);

static int evdev_close_op(void *ctx)
{
	(void)ctx;
	return 0;
}

static inline void evdev_bit_set(uint8_t *bits, unsigned bit)
{
	bits[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

static void evdev_bit_copy(void *dst, size_t dst_len, const uint8_t *src,
						   size_t src_len)
{
	size_t n = dst_len < src_len ? dst_len : src_len;
	memset(dst, 0, dst_len);
	memcpy(dst, src, n);
}

static inline bool evdev_bit_test(const uint8_t *bits, unsigned bit)
{
	return (bits[bit >> 3] & (uint8_t)(1u << (bit & 7u))) != 0;
}

static void evdev_init_keyboard_caps(evdev_t *dev)
{
	dev->name = "Lyr PS/2 keyboard";
	dev->phys = "i8042/serio0/input0";
	dev->uniq = "";
	dev->id.bustype = LYR_INPUT_BUS_I8042;
	dev->id.vendor = 0;
	dev->id.product = 0;
	dev->id.version = 1;
	dev->repeat[0] = 250;
	dev->repeat[1] = 33;

	evdev_bit_set(dev->supported_events, LYR_INPUT_EV_SYN);
	evdev_bit_set(dev->supported_events, LYR_INPUT_EV_KEY);
	evdev_bit_set(dev->supported_events, LYR_INPUT_EV_REP);

	static const uint16_t keys[] = {
		LYR_KEY_ESC,	 LYR_KEY_1,		  LYR_KEY_2,		   LYR_KEY_3,
		LYR_KEY_4,		 LYR_KEY_5,		  LYR_KEY_6,		   LYR_KEY_7,
		LYR_KEY_8,		 LYR_KEY_9,		  LYR_KEY_0,		   LYR_KEY_MINUS,
		LYR_KEY_EQUAL,	 LYR_KEY_BACKSPACE, LYR_KEY_TAB,		  LYR_KEY_Q,
		LYR_KEY_W,		 LYR_KEY_E,		  LYR_KEY_R,		   LYR_KEY_T,
		LYR_KEY_Y,		 LYR_KEY_U,		  LYR_KEY_I,		   LYR_KEY_O,
		LYR_KEY_P,		 LYR_KEY_LEFTBRACE, LYR_KEY_RIGHTBRACE,  LYR_KEY_ENTER,
		LYR_KEY_LEFTCTRL, LYR_KEY_A,		  LYR_KEY_S,		   LYR_KEY_D,
		LYR_KEY_F,		 LYR_KEY_G,		  LYR_KEY_H,		   LYR_KEY_J,
		LYR_KEY_K,		 LYR_KEY_L,		  LYR_KEY_SEMICOLON,   LYR_KEY_APOSTROPHE,
		LYR_KEY_GRAVE,	 LYR_KEY_LEFTSHIFT, LYR_KEY_BACKSLASH,   LYR_KEY_Z,
		LYR_KEY_X,		 LYR_KEY_C,		  LYR_KEY_V,		   LYR_KEY_B,
		LYR_KEY_N,		 LYR_KEY_M,		  LYR_KEY_COMMA,	   LYR_KEY_DOT,
		LYR_KEY_SLASH,	 LYR_KEY_RIGHTSHIFT, LYR_KEY_KPASTERISK,  LYR_KEY_LEFTALT,
		LYR_KEY_SPACE,	 LYR_KEY_CAPSLOCK, LYR_KEY_F1,		   LYR_KEY_F2,
		LYR_KEY_F3,		 LYR_KEY_F4,		  LYR_KEY_F5,		   LYR_KEY_F6,
		LYR_KEY_F7,		 LYR_KEY_F8,		  LYR_KEY_F9,		   LYR_KEY_F10,
		LYR_KEY_NUMLOCK, LYR_KEY_SCROLLLOCK, LYR_KEY_KP7,		   LYR_KEY_KP8,
		LYR_KEY_KP9,	 LYR_KEY_KPMINUS,  LYR_KEY_KP4,		   LYR_KEY_KP5,
		LYR_KEY_KP6,	 LYR_KEY_KPPLUS,   LYR_KEY_KP1,		   LYR_KEY_KP2,
		LYR_KEY_KP3,	 LYR_KEY_KP0,	  LYR_KEY_KPDOT,	   LYR_KEY_F11,
		LYR_KEY_F12,	 LYR_KEY_102ND,	  LYR_KEY_KPENTER,	   LYR_KEY_RIGHTCTRL,
		LYR_KEY_KPSLASH, LYR_KEY_RIGHTALT, LYR_KEY_HOME,		   LYR_KEY_UP,
		LYR_KEY_PAGEUP,	 LYR_KEY_LEFT,	  LYR_KEY_RIGHT,	   LYR_KEY_END,
		LYR_KEY_DOWN,	 LYR_KEY_PAGEDOWN, LYR_KEY_INSERT,	   LYR_KEY_DELETE,
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		evdev_bit_set(dev->supported_keys, keys[i]);
}

static void evdev_init_mouse_caps(evdev_t *dev)
{
	dev->name = "Lyr PS/2 mouse";
	dev->phys = "i8042/serio1/input0";
	dev->uniq = "";
	dev->id.bustype = LYR_INPUT_BUS_I8042;
	dev->id.vendor = 0;
	dev->id.product = 1;
	dev->id.version = 1;

	evdev_bit_set(dev->supported_events, LYR_INPUT_EV_SYN);
	evdev_bit_set(dev->supported_events, LYR_INPUT_EV_KEY);
	evdev_bit_set(dev->supported_events, LYR_INPUT_EV_REL);
	evdev_bit_set(dev->supported_keys, LYR_INPUT_BTN_LEFT);
	evdev_bit_set(dev->supported_keys, LYR_INPUT_BTN_RIGHT);
	evdev_bit_set(dev->supported_keys, LYR_INPUT_BTN_MIDDLE);
	evdev_bit_set(dev->supported_rels, LYR_INPUT_REL_X);
	evdev_bit_set(dev->supported_rels, LYR_INPUT_REL_Y);
	evdev_bit_set(dev->supported_rels, LYR_INPUT_REL_WHEEL);
	evdev_bit_set(dev->supported_props, LYR_INPUT_PROP_POINTER);
}

static void evdev_init_caps(evdev_t *dev)
{
	memset(dev->supported_events, 0, sizeof(dev->supported_events));
	memset(dev->supported_keys, 0, sizeof(dev->supported_keys));
	memset(dev->current_keys, 0, sizeof(dev->current_keys));
	memset(dev->supported_rels, 0, sizeof(dev->supported_rels));
	memset(dev->supported_abs, 0, sizeof(dev->supported_abs));
	memset(dev->supported_props, 0, sizeof(dev->supported_props));
	dev->grabbed = false;
	dev->mouse_buttons = 0;

	if (dev->kind == EVDEV_KIND_KEYBOARD)
		evdev_init_keyboard_caps(dev);
	else
		evdev_init_mouse_caps(dev);
}

static int evdev_wait_for_event(evdev_t *dev)
{
	if (!dev)
		return -EINVAL;

	for (;;) {
		unsigned seq = sched_waitq_prepare(&dev->waitq);

		spinlock_acquire(&dev->lock);
		if (dev->head != dev->tail) {
			spinlock_release(&dev->lock);
			return 0;
		}
		spinlock_release(&dev->lock);

		tcb_t *thread = sched_current();
		if (thread && sched_signal_is_pending(thread))
			return -EINTR;

		int r = sched_waitq_wait(&dev->waitq, seq, NULL);
		if (r != 0)
			return r;
	}
}

int evdev_init(void)
{
	int r = devfs_mkdir("/dev/input", 0755);
	if (r != 0 && r != -EEXIST)
		return r;
	return 0;
}

int evdev_create(evdev_t **out, evdev_kind_t kind, evdev_ioctl_t ioctl,
				 void *ctx)
{
	if (!out)
		return -EINVAL;
	if (kind != EVDEV_KIND_KEYBOARD && kind != EVDEV_KIND_MOUSE)
		return -EINVAL;

	evdev_t *dev = kzalloc(sizeof(*dev));
	if (!dev)
		return -ENOMEM;

	sched_waitq_init(&dev->waitq);
	spinlock_init(&dev->lock);
	dev->kind = kind;
	evdev_init_caps(dev);
	dev->ioctl = ioctl;
	dev->ioctl_ctx = ctx;

	*out = dev;
	return 0;
}

int evdev_bind_path(evdev_t *dev, const char *path, vfs_mode_t mode)
{
	if (!dev || !path)
		return -EINVAL;

	int r = devfs_register_chr_poll_close(path, mode, evdev_read_op, NULL,
										  evdev_ioctl_op, evdev_poll_op,
										  evdev_close_op, dev);
	if (r != 0 && r != -EEXIST)
		return r;

	return 0;
}

void evdev_flush(evdev_t *dev)
{
	if (!dev)
		return;

	spinlock_acquire(&dev->lock);
	dev->tail = dev->head;
	dev->mouse_buttons = 0;
	spinlock_release(&dev->lock);
	sched_waitq_wake_all(&dev->waitq);
	sched_io_wake_all();
}

static size_t evdev_free_slots(const evdev_t *dev)
{
	return EVDEV_RING_SIZE - (size_t)(dev->head - dev->tail);
}

static int evdev_queue_many(evdev_t *dev, const lyr_input_event_t *events,
							size_t count)
{
	if (!dev || !events)
		return -EINVAL;
	if (count == 0)
		return 0;
	if (evdev_free_slots(dev) < count)
		return -EAGAIN;

	for (size_t i = 0; i < count; i++) {
		lyr_input_event_t *out =
			&dev->ring[dev->head & (EVDEV_RING_SIZE - 1)];
		*out = events[i];
		out->time.tv_sec = 0;
		out->time.tv_usec = 0;
		dev->head++;
	}
	return 0;
}

static int evdev_emit_keyboard(evdev_t *dev, const lyr_key_event_t *ev)
{
	if (!dev || !ev)
		return -EINVAL;
	if (ev->keycode == 0 || ev->keycode >= LYR_KEY_MAX)
		return 0;

	lyr_input_event_t out[2] = {
		{ .type = LYR_INPUT_EV_KEY, .code = ev->keycode,
		  .value = ev->down ? 1 : 0 },
		{ .type = LYR_INPUT_EV_SYN, .code = LYR_INPUT_SYN_REPORT, .value = 0 },
	};
	int r = evdev_queue_many(dev, out, 2);
	if (r == 0) {
		if (ev->down)
			evdev_bit_set(dev->current_keys, ev->keycode);
		else
			dev->current_keys[ev->keycode >> 3] &=
				(uint8_t)~(1u << (ev->keycode & 7u));
	}
	return r;
}

static uint16_t evdev_mouse_button_code(uint32_t bit)
{
	switch (bit) {
	case LYR_MOUSE_BUTTON_LEFT:
		return LYR_INPUT_BTN_LEFT;
	case LYR_MOUSE_BUTTON_RIGHT:
		return LYR_INPUT_BTN_RIGHT;
	case LYR_MOUSE_BUTTON_MIDDLE:
		return LYR_INPUT_BTN_MIDDLE;
	default:
		return 0;
	}
}

static int evdev_emit_mouse(evdev_t *dev, const lyr_mouse_event_t *ev)
{
	if (!dev || !ev)
		return -EINVAL;

	lyr_input_event_t out[8];
	size_t count = 0;
	uint32_t changed = dev->mouse_buttons ^ ev->buttons;
	uint32_t pressed = ev->buttons;

	for (uint32_t bit = 1u; bit <= LYR_MOUSE_BUTTON_MIDDLE; bit <<= 1u) {
		if (!(changed & bit))
			continue;
		uint16_t code = evdev_mouse_button_code(bit);
		if (code == 0)
			continue;
		if (count >= sizeof(out) / sizeof(out[0]))
			return -EOVERFLOW;
		out[count++] = (lyr_input_event_t){
			.type = LYR_INPUT_EV_KEY,
			.code = code,
			.value = (pressed & bit) ? 1 : 0,
		};
	}

	if (ev->dx != 0) {
		if (count >= sizeof(out) / sizeof(out[0]))
			return -EOVERFLOW;
		out[count++] = (lyr_input_event_t){
			.type = LYR_INPUT_EV_REL,
			.code = LYR_INPUT_REL_X,
			.value = ev->dx,
		};
	}
	if (ev->dy != 0) {
		if (count >= sizeof(out) / sizeof(out[0]))
			return -EOVERFLOW;
		out[count++] = (lyr_input_event_t){
			.type = LYR_INPUT_EV_REL,
			.code = LYR_INPUT_REL_Y,
			.value = ev->dy,
		};
	}
	if (ev->dz != 0) {
		if (count >= sizeof(out) / sizeof(out[0]))
			return -EOVERFLOW;
		out[count++] = (lyr_input_event_t){
			.type = LYR_INPUT_EV_REL,
			.code = LYR_INPUT_REL_WHEEL,
			.value = ev->dz,
		};
	}

	if (count >= sizeof(out) / sizeof(out[0]))
		return -EOVERFLOW;
	out[count++] = (lyr_input_event_t){
		.type = LYR_INPUT_EV_SYN,
		.code = LYR_INPUT_SYN_REPORT,
		.value = 0,
	};

	int r = evdev_queue_many(dev, out, count);
	if (r == 0) {
		dev->mouse_buttons = ev->buttons;
		dev->current_keys[LYR_INPUT_BTN_LEFT >> 3] &=
			(uint8_t)~(1u << (LYR_INPUT_BTN_LEFT & 7u));
		dev->current_keys[LYR_INPUT_BTN_RIGHT >> 3] &=
			(uint8_t)~(1u << (LYR_INPUT_BTN_RIGHT & 7u));
		dev->current_keys[LYR_INPUT_BTN_MIDDLE >> 3] &=
			(uint8_t)~(1u << (LYR_INPUT_BTN_MIDDLE & 7u));
		if (ev->buttons & LYR_MOUSE_BUTTON_LEFT)
			evdev_bit_set(dev->current_keys, LYR_INPUT_BTN_LEFT);
		if (ev->buttons & LYR_MOUSE_BUTTON_RIGHT)
			evdev_bit_set(dev->current_keys, LYR_INPUT_BTN_RIGHT);
		if (ev->buttons & LYR_MOUSE_BUTTON_MIDDLE)
			evdev_bit_set(dev->current_keys, LYR_INPUT_BTN_MIDDLE);
	}
	return r;
}

int evdev_push(evdev_t *dev, const void *record)
{
	if (!dev || !record)
		return -EINVAL;

	spinlock_acquire(&dev->lock);
	int r;
	if (dev->kind == EVDEV_KIND_KEYBOARD)
		r = evdev_emit_keyboard(dev, (const lyr_key_event_t *)record);
	else
		r = evdev_emit_mouse(dev, (const lyr_mouse_event_t *)record);
	spinlock_release(&dev->lock);

	if (r == 0) {
		sched_waitq_wake_all(&dev->waitq);
		sched_io_wake_all();
	}
	return r;
}

int evdev_read_record(evdev_t *dev, void *record)
{
	if (!dev || !record)
		return -EINVAL;

	for (;;) {
		spinlock_acquire(&dev->lock);
		if (dev->head != dev->tail) {
			*(lyr_input_event_t *)record =
				dev->ring[dev->tail & (EVDEV_RING_SIZE - 1)];
			__asm__ volatile("" ::: "memory");
			dev->tail++;
			spinlock_release(&dev->lock);
			return 0;
		}
		spinlock_release(&dev->lock);

		int r = evdev_wait_for_event(dev);
		if (r != 0)
			return r;
	}
}

int evdev_read_bytes(evdev_t *dev, void *buf, size_t len, size_t *done)
{
	if (done)
		*done = 0;
	if (!dev || !buf)
		return -EINVAL;
	if (len == 0)
		return 0;
	if (len < sizeof(lyr_input_event_t))
		return -EINVAL;

	for (;;) {
		spinlock_acquire(&dev->lock);
		if (dev->head != dev->tail) {
			size_t n = 0;
			uint8_t *out = buf;
			while ((n + 1) * sizeof(lyr_input_event_t) <= len &&
				   dev->head != dev->tail) {
				memcpy(out + n * sizeof(lyr_input_event_t),
					   &dev->ring[dev->tail & (EVDEV_RING_SIZE - 1)],
					   sizeof(lyr_input_event_t));
				__asm__ volatile("" ::: "memory");
				dev->tail++;
				n++;
			}
			spinlock_release(&dev->lock);
			if (done)
				*done = n * sizeof(lyr_input_event_t);
			return 0;
		}
		spinlock_release(&dev->lock);

		int r = evdev_wait_for_event(dev);
		if (r != 0)
			return r;
	}
}

static unsigned long evdev_request_type(unsigned long request)
{
	return (request >> LYR_IOC_TYPESHIFT) & LYR_IOC_TYPEMASK;
}

static unsigned long evdev_request_nr(unsigned long request)
{
	return (request >> LYR_IOC_NRSHIFT) & LYR_IOC_NRMASK;
}

static unsigned long evdev_request_size(unsigned long request)
{
	return (request >> LYR_IOC_SIZESHIFT) & LYR_IOC_SIZEMASK;
}

static unsigned long evdev_request_dir(unsigned long request)
{
	return (request >> LYR_IOC_DIRSHIFT) & LYR_IOC_DIRMASK;
}

static int evdev_fill_name(void *arg, size_t len, const char *name)
{
	if (!arg || len == 0)
		return -EINVAL;
	memset(arg, 0, len);
	size_t n = strlen(name);
	if (n >= len)
		n = len - 1;
	memcpy(arg, name, n);
	return 0;
}

static int evdev_fill_bitset(void *arg, size_t len, const uint8_t *bits,
							 size_t bits_len)
{
	if (!arg || len == 0)
		return -EINVAL;
	evdev_bit_copy(arg, len, bits, bits_len);
	return 0;
}

static int evdev_copy_mask(void *arg, const uint8_t *bits, size_t bits_len)
{
	if (!arg)
		return -EINVAL;
	lyr_input_mask_t *mask = arg;
	if (mask->codes_ptr == 0 || mask->codes_size == 0)
		return -EINVAL;
	return evdev_fill_bitset((void *)(uintptr_t)mask->codes_ptr,
							 mask->codes_size, bits, bits_len);
}

static int evdev_read_op(void *ctx, uint64_t off, void *buf, size_t len,
						 size_t *done)
{
	(void)off;
	return evdev_read_bytes((evdev_t *)ctx, buf, len, done);
}

static int evdev_poll_op(void *ctx, int events)
{
	evdev_t *dev = ctx;

	if (!dev)
		return LYR_POLLERR;

	int revents = 0;

	if ((events & (LYR_POLLIN | LYR_POLLRDNORM)) && dev->head != dev->tail)
		revents |= LYR_POLLIN | LYR_POLLRDNORM;

	return revents;
}

static int evdev_ioctl_op(void *ctx, unsigned long request, void *arg)
{
	evdev_t *dev = ctx;

	if (!dev)
		return -EBADF;
	if (evdev_request_type(request) != 'E') {
		if (!dev->ioctl)
			return -ENOTTY;
		return dev->ioctl(dev->ioctl_ctx, request, arg);
	}

	unsigned long nr = evdev_request_nr(request);
	size_t len = evdev_request_size(request);
	unsigned long dir = evdev_request_dir(request);

	switch (nr) {
	case 0x01:
		if (len != sizeof(int))
			return -EINVAL;
		if (!arg)
			return -EINVAL;
		*(int *)arg = 0x010001;
		return 0;

	case 0x02:
		if (len != sizeof(lyr_input_id_t))
			return -EINVAL;
		if (!arg)
			return -EINVAL;
		*(lyr_input_id_t *)arg = dev->id;
		return 0;

	case 0x03:
		if (len != sizeof(unsigned int[2]))
			return -EINVAL;
		if (!arg)
			return -EINVAL;
		if (dir == LYR_IOC_READ) {
			unsigned int *rep = arg;
			rep[0] = dev->repeat[0];
			rep[1] = dev->repeat[1];
			return 0;
		}
		if (dir == LYR_IOC_WRITE) {
			unsigned int *rep = arg;
			dev->repeat[0] = rep[0];
			dev->repeat[1] = rep[1];
			return 0;
		}
		return -EINVAL;

	case 0x04:
		if (len == sizeof(unsigned int[2])) {
			if (!arg)
				return -EINVAL;
			unsigned int *pair = arg;
			if (dir == LYR_IOC_READ) {
				if (pair[0] >= LYR_KEY_MAX)
					return -EINVAL;
				pair[1] = pair[0];
				return 0;
			}
			if (dir == LYR_IOC_WRITE) {
				if (pair[1] >= LYR_KEY_MAX)
					return -EINVAL;
				return 0;
			}
		}
		if (len == sizeof(lyr_input_keymap_entry_t)) {
			if (!arg)
				return -EINVAL;
			lyr_input_keymap_entry_t *ent = arg;
			if (dir == LYR_IOC_READ) {
				if (ent->index >= LYR_KEY_MAX)
					return -EINVAL;
				ent->keycode = ent->index;
				return 0;
			}
			if (dir == LYR_IOC_WRITE) {
				if (ent->keycode >= LYR_KEY_MAX)
					return -EINVAL;
				return 0;
			}
		}
		return -ENOTTY;

	case 0x06:
		return evdev_fill_name(arg, len, dev->name);

	case 0x07:
		return evdev_fill_name(arg, len, dev->phys);

	case 0x08:
		return evdev_fill_name(arg, len, dev->uniq);

	case 0x09:
		return evdev_fill_bitset(arg, len, dev->supported_props,
								 sizeof(dev->supported_props));

	default:
		break;
	}

	if (nr >= 0x20 && nr < 0x20 + 0x80) {
		unsigned ev = nr - 0x20;
		switch (ev) {
		case 0x00:
			return evdev_fill_bitset(arg, len, dev->supported_events,
									 sizeof(dev->supported_events));
		case 0x01:
			return evdev_fill_bitset(arg, len, dev->supported_keys,
									 sizeof(dev->supported_keys));
		case 0x02:
			return evdev_fill_bitset(arg, len, dev->supported_rels,
									 sizeof(dev->supported_rels));
		case 0x03:
			return evdev_fill_bitset(arg, len, dev->supported_abs,
									 sizeof(dev->supported_abs));
		case 0x04:
		case 0x05:
		case 0x11:
		case 0x12:
		case 0x15:
		case 0x16:
		case 0x17:
			return evdev_fill_bitset(arg, len, (uint8_t[LYR_INPUT_BITSET_BYTES]){0},
									 LYR_INPUT_BITSET_BYTES);
		case 0x14:
			if (dev->kind != EVDEV_KIND_KEYBOARD)
				return evdev_fill_bitset(
					arg, len, (uint8_t[LYR_INPUT_BITSET_BYTES]){0},
					LYR_INPUT_BITSET_BYTES);
			{
				uint8_t rep_bits[LYR_INPUT_BITSET_BYTES] = { 0 };
				evdev_bit_set(rep_bits, 0);
				evdev_bit_set(rep_bits, 1);
				return evdev_fill_bitset(arg, len, rep_bits, sizeof(rep_bits));
			}
		default:
			return evdev_fill_bitset(arg, len, (uint8_t[LYR_INPUT_BITSET_BYTES]){0},
									 LYR_INPUT_BITSET_BYTES);
		}
	}

	if (nr >= 0x40 && nr < 0x40 + 0x80) {
		return -EINVAL;
	}

	switch (nr) {
	case 0x18:
		return evdev_fill_bitset(arg, len, dev->current_keys,
								 sizeof(dev->current_keys));

	case 0x19:
	case 0x1a:
	case 0x1b:
		return evdev_fill_bitset(arg, len, (uint8_t[LYR_INPUT_BITSET_BYTES]){0},
								 LYR_INPUT_BITSET_BYTES);

	case 0x40 ... 0x7f:
		if (len != sizeof(lyr_input_absinfo_t))
			return -EINVAL;
		if (!arg)
			return -EINVAL;
		if (!evdev_bit_test(dev->supported_abs, (unsigned)(nr - 0x40)))
			return -EINVAL;
		memset(arg, 0, sizeof(lyr_input_absinfo_t));
		return 0;

	case 0x80:
	case 0x81:
	case 0x84:
		return -ENOTTY;

	case 0x90:
		if (len != sizeof(int))
			return -EINVAL;
		if (!arg)
			return -EINVAL;
		dev->grabbed = arg && (*(int *)arg != 0);
		return 0;

	case 0x91:
		dev->grabbed = false;
		return 0;

	case 0x92:
		if (len != sizeof(lyr_input_mask_t))
			return -EINVAL;
		if (!arg)
			return -EINVAL;
	{
		lyr_input_mask_t *mask = arg;
		switch (mask->type) {
		case LYR_INPUT_EV_SYN:
			return evdev_copy_mask(arg, dev->supported_events,
								   sizeof(dev->supported_events));
		case LYR_INPUT_EV_KEY:
			return evdev_copy_mask(arg, dev->supported_keys,
								   sizeof(dev->supported_keys));
		case LYR_INPUT_EV_REL:
			return evdev_copy_mask(arg, dev->supported_rels,
								   sizeof(dev->supported_rels));
		case LYR_INPUT_EV_ABS:
			return evdev_copy_mask(arg, dev->supported_abs,
								   sizeof(dev->supported_abs));
		case LYR_INPUT_EV_REP:
			return evdev_copy_mask(arg, (uint8_t[LYR_INPUT_BITSET_BYTES]){ 0 },
								   LYR_INPUT_BITSET_BYTES);
		default:
			return evdev_copy_mask(arg, (uint8_t[LYR_INPUT_BITSET_BYTES]){0},
								   LYR_INPUT_BITSET_BYTES);
		}
	}

	case 0x93:
		return 0;

	case 0xa0:
		return 0;

	case 0xc0 ... 0xff:
		if (len != sizeof(lyr_input_absinfo_t))
			return -EINVAL;
		if (!arg)
			return -EINVAL;
		if (!evdev_bit_test(dev->supported_abs, (unsigned)(nr - 0xc0)))
			return -EINVAL;
		return 0;
	}

	return -ENOTTY;
}

void evdev_release(evdev_t *dev)
{
	if (!dev)
		return;
	kfree(dev);
}
