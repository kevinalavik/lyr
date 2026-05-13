#include <dev/kbd.h>
#include <dev/console.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <sys/poll.h>

#define KBD_EVENT_RING_SIZE 256u /* must be power of two */

static lyr_key_event_t kbd_event_ring[KBD_EVENT_RING_SIZE];
static volatile uint32_t kbd_event_head;
static volatile uint32_t kbd_event_tail;

static inline uint32_t ring_count(void)
{
	return kbd_event_head - kbd_event_tail;
}

static inline int ring_empty(void)
{
	return ring_count() == 0;
}
static inline int ring_full(void)
{
	return ring_count() >= KBD_EVENT_RING_SIZE;
}

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

	if (!ring_full()) {
		kbd_event_ring[kbd_event_head & (KBD_EVENT_RING_SIZE - 1)] = e;
		/* barrier: ensure write completes before incrementing head */
		__asm__ volatile("" ::: "memory");
		kbd_event_head++;
	}

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
	if (!ev)
		return -EINVAL;

	while (ring_empty())
		__asm__ volatile("sti; hlt; cli" ::: "memory");

	*ev = kbd_event_ring[kbd_event_tail & (KBD_EVENT_RING_SIZE - 1)];
	__asm__ volatile("" ::: "memory");
	kbd_event_tail++;
	return 0;
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

static int kbd_event_dev_read(void *ctx, uint64_t off, void *buf, size_t len,
							  size_t *done)
{
	(void)ctx;
	(void)off;

	if (done)
		*done = 0;
	if (!buf || len < sizeof(lyr_key_event_t))
		return -EINVAL;

	while (ring_empty())
		__asm__ volatile("sti; hlt; cli" ::: "memory");

	lyr_key_event_t *out = buf;
	size_t n = 0;

	while ((n + 1) * sizeof(lyr_key_event_t) <= len && !ring_empty()) {
		out[n] = kbd_event_ring[kbd_event_tail & (KBD_EVENT_RING_SIZE - 1)];
		__asm__ volatile("" ::: "memory");
		kbd_event_tail++;
		n++;
	}

	if (done)
		*done = n * sizeof(lyr_key_event_t);
	return 0;
}

static int kbd_event_dev_poll(void *ctx, int events)
{
	(void)ctx;
	int revents = 0;
	if ((events & (LYR_POLLIN | LYR_POLLRDNORM)) && !ring_empty())
		revents |= LYR_POLLIN | LYR_POLLRDNORM;
	return revents;
}

static inline void ring_flush(void)
{
	__asm__ volatile("" ::: "memory");
	kbd_event_tail = kbd_event_head;
	kbd_mods = 0;
	__asm__ volatile("" ::: "memory");
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
		ring_flush();
		return 0;

	default:
		return -ENOTTY;
	}
}

int kbd_init(void)
{
	memset(kbd_layout, 0, sizeof(kbd_layout));
	kbd_event_head = 0;
	kbd_event_tail = 0;
	kbd_mods = 0;
	kbd_current_map[0] = '\0';

	kbd_load_default_keymap();

	int r = devfs_mkdir("/dev/input", 0755);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr_poll("/dev/input/event0", 0444, kbd_event_dev_read,
								NULL, kbd_event_dev_ioctl, kbd_event_dev_poll,
								NULL);
	if (r != 0 && r != -EEXIST)
		return r;

	return 0;
}
