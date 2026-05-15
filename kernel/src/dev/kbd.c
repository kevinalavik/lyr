#include <dev/kbd.h>
#include <dev/console.h>
#include <fs/evdev.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <sync/spinlock.h>

#define KBD_MAP_BYTES_MAX 4u

typedef struct {
	uint8_t bytes[KBD_MAP_BYTES_MAX];
	uint8_t len;
} layout_symbol_t;

typedef struct {
	layout_symbol_t normal;
	layout_symbol_t shifted;
	layout_symbol_t altgr;
} layout_entry_t;

static layout_entry_t kbd_layout[LYR_KEY_MAX];

static uint16_t kbd_mods;
static char kbd_current_map[LYR_KBD_MAP_PATH_MAX];
static evdev_t *kbd_evdev;

#define KBD_EVENT_RING_SIZE 256u

typedef struct {
	lyr_key_event_t ring[KBD_EVENT_RING_SIZE];
	volatile uint32_t head;
	volatile uint32_t tail;
	sched_waitq_t waitq;
	spinlock_t lock;
} kbd_event_queue_t;

static kbd_event_queue_t kbd_queue;

static void kbd_queue_init(kbd_event_queue_t *q)
{
	memset(q, 0, sizeof(*q));
	sched_waitq_init(&q->waitq);
	spinlock_init(&q->lock);
}

static void kbd_queue_flush(kbd_event_queue_t *q)
{
	spinlock_acquire(&q->lock);
	q->tail = q->head;
	spinlock_release(&q->lock);
	sched_waitq_wake_all(&q->waitq);
	sched_io_wake_all();
}

static int kbd_queue_push(kbd_event_queue_t *q, const lyr_key_event_t *ev)
{
	if (!q || !ev)
		return -EINVAL;

	spinlock_acquire(&q->lock);
	if (q->head - q->tail >= KBD_EVENT_RING_SIZE) {
		spinlock_release(&q->lock);
		return -EAGAIN;
	}

	q->ring[q->head & (KBD_EVENT_RING_SIZE - 1)] = *ev;
	__asm__ volatile("" ::: "memory");
	q->head++;
	spinlock_release(&q->lock);

	sched_waitq_wake_all(&q->waitq);
	sched_io_wake_all();
	return 0;
}

static int kbd_queue_wait_for_event(kbd_event_queue_t *q)
{
	if (!q)
		return -EINVAL;

	for (;;) {
		unsigned seq = sched_waitq_prepare(&q->waitq);

		spinlock_acquire(&q->lock);
		if (q->head != q->tail) {
			spinlock_release(&q->lock);
			return 0;
		}
		spinlock_release(&q->lock);

		tcb_t *thread = sched_current();
		if (thread && sched_signal_is_pending(thread))
			return -EINTR;

		int r = sched_waitq_wait(&q->waitq, seq, NULL);
		if (r != 0)
			return r;
	}
}

static int kbd_queue_read(kbd_event_queue_t *q, lyr_key_event_t *ev)
{
	if (!q || !ev)
		return -EINVAL;

	for (;;) {
		spinlock_acquire(&q->lock);
		if (q->head != q->tail) {
			*ev = q->ring[q->tail & (KBD_EVENT_RING_SIZE - 1)];
			__asm__ volatile("" ::: "memory");
			q->tail++;
			spinlock_release(&q->lock);
			return 0;
		}
		spinlock_release(&q->lock);

		int r = kbd_queue_wait_for_event(q);
		if (r != 0)
			return r;
	}
}

static void layout_symbol_clear(layout_symbol_t *out)
{
	if (!out)
		return;
	out->len = 0;
	memset(out->bytes, 0, sizeof(out->bytes));
}

static int layout_symbol_set_byte(layout_symbol_t *out, uint8_t ch)
{
	layout_symbol_clear(out);
	if (ch == 0)
		return 0;
	out->bytes[0] = ch;
	out->len = 1;
	return 0;
}

static int layout_symbol_set_utf8_codepoint(layout_symbol_t *out, unsigned cp)
{
	layout_symbol_clear(out);

	if (cp == 0)
		return 0;
	if (cp <= 0x7fu) {
		out->bytes[0] = (uint8_t)cp;
		out->len = 1;
		return 0;
	}
	if (cp <= 0x7ffu) {
		out->bytes[0] = (uint8_t)(0xc0u | (cp >> 6));
		out->bytes[1] = (uint8_t)(0x80u | (cp & 0x3fu));
		out->len = 2;
		return 0;
	}
	if (cp <= 0xffffu) {
		out->bytes[0] = (uint8_t)(0xe0u | (cp >> 12));
		out->bytes[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
		out->bytes[2] = (uint8_t)(0x80u | (cp & 0x3fu));
		out->len = 3;
		return 0;
	}
	if (cp <= 0x10ffffu) {
		out->bytes[0] = (uint8_t)(0xf0u | (cp >> 18));
		out->bytes[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
		out->bytes[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
		out->bytes[3] = (uint8_t)(0x80u | (cp & 0x3fu));
		out->len = 4;
		return 0;
	}

	return -1;
}

static int layout_symbol_set_token(layout_symbol_t *out, const char *s,
								   int dash_is_none)
{
	if (!out || !s)
		return -1;

	layout_symbol_clear(out);

	if (s[0] == '\0')
		return 0;
	if (dash_is_none && strcmp(s, "-") == 0)
		return 0;

	struct {
		const char *name;
		uint8_t val;
	} syms[] = {
		{ "nul", 0 },	   { "backspace", 8 }, { "tab", 9 },
		{ "enter", '\r' }, { "escape", 27 },   { "space", ' ' },
		{ "delete", 127 }, { "minus", '-' },
	};
	for (unsigned i = 0; i < sizeof(syms) / sizeof(syms[0]); i++)
		if (strcmp(s, syms[i].name) == 0)
			return layout_symbol_set_byte(out, syms[i].val);

	if (strncmp(s, "escape", 6) == 0 && s[6] != '\0') {
		out->bytes[0] = 27;
		size_t rest_len = strlen(s + 6);
		if (rest_len >= KBD_MAP_BYTES_MAX - 1)
			rest_len = KBD_MAP_BYTES_MAX - 1;
		memcpy(out->bytes + 1, s + 6, rest_len);
		out->len = (uint8_t)(1 + rest_len);
		return 0;
	}

	/* Decimal Unicode codepoint (only for multi-digit numbers). */
	if (s[0] >= '0' && s[0] <= '9' && s[1] != '\0') {
		unsigned cp = 0;
		for (int i = 0; s[i]; i++) {
			if (s[i] < '0' || s[i] > '9')
				goto copy_utf8_token;
			cp = cp * 10 + (unsigned)(s[i] - '0');
		}
		return layout_symbol_set_utf8_codepoint(out, cp);
	}

copy_utf8_token:
	/* Copy one UTF-8 scalar from the token verbatim. */
	uint8_t b0 = (uint8_t)s[0];
	size_t len = 1;
	if ((b0 & 0x80u) == 0)
		len = 1;
	else if ((b0 & 0xe0u) == 0xc0u)
		len = 2;
	else if ((b0 & 0xf0u) == 0xe0u)
		len = 3;
	else if ((b0 & 0xf8u) == 0xf0u)
		len = 4;
	else
		return -1;

	for (size_t i = 0; i < len; i++) {
		if (s[i] == '\0')
			return -1;
		out->bytes[i] = (uint8_t)s[i];
	}
	out->len = (uint8_t)len;
	return 0;
}

static int layout_symbol_is_ascii_lower(const layout_symbol_t *sym)
{
	return sym && sym->len == 1 && sym->bytes[0] >= 'a' && sym->bytes[0] <= 'z';
}

static void console_input_put_symbol(const layout_symbol_t *sym)
{
	if (!sym)
		return;
	for (uint8_t i = 0; i < sym->len; i++)
		console_input_put(sym->bytes[i]);
}

static int split_line(char *line, char *toks[], int max_toks)
{
	int n = 0;
	char *p = line;

	while (*p && n < max_toks) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '\n' || *p == '\r')
			break;

		if (*p == '#' && n == 0)
			break;
		toks[n++] = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
			p++;
		if (*p)
			*p++ = '\0';
	}
	return n;
}

void kbd_load_default_keymap(void)
{
	kbd_load_keymap_file("/share/kbd/us");
}

int kbd_load_keymap_file(const char *path)
{
	vfs_file_t *file;
	int r = vfs_open(path, VFS_O_RDONLY, 0, &vfs_root_cred, &file);
	if (r != 0)
		return r;

		/* Read the whole file into a heap buffer. Layout files are tiny. */
#define KEYMAP_FILE_MAX (16 * 1024)
	char *buf = kzalloc(KEYMAP_FILE_MAX);
	if (!buf) {
		vfs_close(file);
		return -ENOMEM;
	}

	size_t done = 0;
	r = vfs_read(file, buf, KEYMAP_FILE_MAX - 1, &done);
	vfs_close(file);
	if (r != 0) {
		kfree(buf);
		return r;
	}
	buf[done] = '\0';

	memset(kbd_layout, 0, sizeof(kbd_layout));

	char *line = buf;
	while (*line) {
		/* Find end of line */
		char *end = line;
		while (*end && *end != '\n' && *end != '\r')
			end++;
		char saved = *end;
		*end = '\0';

		char *toks[4];
		int n = split_line(line, toks, 4);

		if (n >= 3) {
			unsigned sc = 0;
			for (int i = 0; toks[0][i]; i++)
				sc = sc * 10 + (unsigned)(toks[0][i] - '0');

			if (sc < LYR_KEY_MAX) {
				/* Keep '-' usable as a literal key in normal/shifted columns.
				 * In the optional AltGr column, '-' remains the conventional
				 * "no mapping" marker used by the existing keymap files.
				 */
				layout_symbol_set_token(&kbd_layout[sc].normal, toks[1], 0);
				layout_symbol_set_token(&kbd_layout[sc].shifted, toks[2], 0);
				if (n >= 4)
					layout_symbol_set_token(&kbd_layout[sc].altgr, toks[3], 1);
			}
		}

		*end = saved;
		line = end;
		while (*line == '\n' || *line == '\r')
			line++;
	}

	kfree(buf);

	size_t plen = strlen(path);
	if (plen >= LYR_KBD_MAP_PATH_MAX)
		plen = LYR_KBD_MAP_PATH_MAX - 1;
	memcpy(kbd_current_map, path, plen);
	kbd_current_map[plen] = '\0';

	return 0;
}

void kbd_submit_event(const lyr_key_event_t *ev)
{
	if (!ev)
		return;

	/* 1. Maintain modifier state */
	uint16_t mod_bit = 0;
	switch (ev->keycode) {
	case LYR_KEY_LEFTSHIFT:
	case LYR_KEY_RIGHTSHIFT:
		mod_bit = LYR_KBD_MOD_SHIFT;
		break;
	case LYR_KEY_LEFTCTRL:
	case LYR_KEY_RIGHTCTRL:
		mod_bit = LYR_KBD_MOD_CTRL;
		break;
	case LYR_KEY_LEFTALT:
	case LYR_KEY_RIGHTALT:
		mod_bit = LYR_KBD_MOD_ALT;
		break;
	case LYR_KEY_CAPSLOCK:
		if (ev->down)
			kbd_mods ^= LYR_KBD_MOD_CAPS;
		break;
	case LYR_KEY_NUMLOCK:
		if (ev->down)
			kbd_mods ^= LYR_KBD_MOD_NUM;
		break;
	case LYR_KEY_SCROLLLOCK:
		if (ev->down)
			kbd_mods ^= LYR_KBD_MOD_SCROLL;
		break;
	}
	if (mod_bit) {
		if (ev->down)
			kbd_mods |= mod_bit;
		else
			kbd_mods &= (uint16_t)~mod_bit;
	}

	/* 2. Build a copy with the current mod state and push to ring */
	lyr_key_event_t e = *ev;
	e.mods = kbd_mods;

	(void)kbd_queue_push(&kbd_queue, &e);
	if (kbd_evdev)
		(void)evdev_push(kbd_evdev, &e);

	if (ev->down && (kbd_mods & LYR_KBD_MOD_ALT)) {
		switch (ev->keycode) {
		case LYR_KEY_F1:
		case LYR_KEY_F2:
		case LYR_KEY_F3:
		case LYR_KEY_F4:
			console_switch_tty((unsigned)(ev->keycode - LYR_KEY_F1));
			return;
		default:
			break;
		}
	}

	/* 3. On key-down only: translate to a byte and push to /dev/stdin */
	if (!ev->down)
		return;
	if (ev->keycode == 0 || ev->keycode >= LYR_KEY_MAX)
		return;

	const layout_entry_t *le = &kbd_layout[ev->keycode];
	const layout_symbol_t *sym;
	layout_symbol_t ctrl_sym;

	int shift_active = (kbd_mods & LYR_KBD_MOD_SHIFT) != 0;
	/* Caps-lock flips shift for ASCII alphabetic keys. */
	if ((kbd_mods & LYR_KBD_MOD_CAPS) &&
		layout_symbol_is_ascii_lower(&le->normal))
		shift_active = !shift_active;

	if ((kbd_mods & LYR_KBD_MOD_ALT) && le->altgr.len)
		sym = &le->altgr;
	else if (shift_active)
		sym = &le->shifted;
	else
		sym = &le->normal;

	/* Ctrl-<ASCII> → control code (^A = 1 … ^Z = 26, ^[ ^\ ^] ^^ ^_). */
	if ((kbd_mods & LYR_KBD_MOD_CTRL) && sym->len == 1 &&
		sym->bytes[0] >= 0x40 && sym->bytes[0] <= 0x7f) {
		layout_symbol_set_byte(&ctrl_sym, (uint8_t)(sym->bytes[0] & 0x1f));
		sym = &ctrl_sym;
	}

	console_input_put_symbol(sym);
}

int kbd_read_event(lyr_key_event_t *ev)
{
	return kbd_queue_read(&kbd_queue, ev);
}

int kbd_read_byte(uint8_t *ch)
{
	static uint8_t pending[KBD_MAP_BYTES_MAX];
	static uint8_t pending_len;
	static uint8_t pending_pos;

	if (!ch)
		return -EINVAL;

	if (pending_pos < pending_len) {
		*ch = pending[pending_pos++];
		if (pending_pos >= pending_len) {
			pending_pos = 0;
			pending_len = 0;
		}
		return 0;
	}

	for (;;) {
		lyr_key_event_t ev;
		int r = kbd_read_event(&ev);
		if (r != 0)
			return r;
		if (!ev.down)
			continue;
		if (ev.keycode == 0 || ev.keycode >= LYR_KEY_MAX)
			continue;

		const layout_entry_t *le = &kbd_layout[ev.keycode];
		const layout_symbol_t *sym;
		layout_symbol_t ctrl_sym;

		int shift_active = (ev.mods & LYR_KBD_MOD_SHIFT) != 0;
		if ((ev.mods & LYR_KBD_MOD_CAPS) &&
			layout_symbol_is_ascii_lower(&le->normal))
			shift_active = !shift_active;

		if ((ev.mods & LYR_KBD_MOD_ALT) && le->altgr.len)
			sym = &le->altgr;
		else if (shift_active)
			sym = &le->shifted;
		else
			sym = &le->normal;

		if ((ev.mods & LYR_KBD_MOD_CTRL) && sym->len == 1 &&
			sym->bytes[0] >= 0x40 && sym->bytes[0] <= 0x7f) {
			layout_symbol_set_byte(&ctrl_sym, (uint8_t)(sym->bytes[0] & 0x1f));
			sym = &ctrl_sym;
		}

		if (sym->len == 0)
			continue;

		*ch = sym->bytes[0];
		if (sym->len > 1) {
			memcpy(pending, sym->bytes, sym->len);
			pending_len = sym->len;
			pending_pos = 1;
		}
		return 0;
	}
}

static int kbd_event_dev_ioctl(void *ctx, unsigned long request, void *arg)
{
	(void)ctx;
	tcb_t *thread = sched_current();
	pcb_t *process = thread ? thread->process : NULL;

	switch (request) {
	case LYR_KBDIOCSMAP:
		if (!arg)
			return -EINVAL;
		if (process && process->vas &&
			(uint64_t)(uintptr_t)arg < VAS_USER_END) {
			char path[LYR_KBD_MAP_PATH_MAX];
			if (vas_user_access_ok(process->vas, (uint64_t)(uintptr_t)arg,
								   sizeof(path), 0) != 0)
				return -EFAULT;
			memcpy(path, arg, sizeof(path));
			path[sizeof(path) - 1] = '\0';
			return kbd_load_keymap_file(path);
		}
		return kbd_load_keymap_file((const char *)arg);

	case LYR_KBDIOCGMAP:
		if (!arg)
			return -EINVAL;
		if (kbd_current_map[0] == '\0')
			return -EINVAL;
		if (process && process->vas &&
			(uint64_t)(uintptr_t)arg < VAS_USER_END &&
			vas_user_access_ok(process->vas, (uint64_t)(uintptr_t)arg,
							   LYR_KBD_MAP_PATH_MAX, 1) != 0)
			return -EFAULT;
		memcpy(arg, kbd_current_map, LYR_KBD_MAP_PATH_MAX);
		return 0;

	case LYR_KBDIOCFLUSH:
		kbd_queue_flush(&kbd_queue);
		if (kbd_evdev)
			evdev_flush(kbd_evdev);
		kbd_mods = 0;
		return 0;

	default:
		return -ENOTTY;
	}
}

int kbd_init(void)
{
	memset(kbd_layout, 0, sizeof(kbd_layout));
	kbd_mods = 0;
	kbd_current_map[0] = '\0';
	kbd_queue_init(&kbd_queue);

	int r = evdev_init();
	if (r != 0)
		return r;

	r = evdev_create(&kbd_evdev, EVDEV_KIND_KEYBOARD, kbd_event_dev_ioctl,
					 NULL);
	if (r != 0)
		return r;

	r = evdev_bind_path(kbd_evdev, "/dev/input/event0", 0444);
	if (r != 0)
		return r;

	r = evdev_bind_path(kbd_evdev, "/dev/input/kbd", 0444);
	if (r != 0)
		return r;

	kbd_load_default_keymap();

	return 0;
}
