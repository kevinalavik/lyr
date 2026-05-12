#include <cpu/instr.h>
#include <dev/console.h>
#include <dev/device.h>
#include <dev/kbd.h>
#include <drv/driver.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <ipc/ipc.h>
#include <sys/poll.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT 0x64

#define PS2_STATUS_OUT_BUF 0x01
#define PS2_STATUS_IN_BUF 0x02
#define PS2_STATUS_SYS_FLAG 0x04
#define PS2_STATUS_CMD_DATA 0x08
#define PS2_STATUS_AUX_DATA 0x20

#define PS2_CMD_READ_CONFIG 0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_DISABLE_PORT1 0xAD
#define PS2_CMD_ENABLE_PORT1 0xAE
#define PS2_CMD_DISABLE_PORT2 0xA7
#define PS2_CMD_ENABLE_PORT2 0xA8
#define PS2_CMD_TEST_CONTROLLER 0xAA
#define PS2_CMD_TEST_PORT1 0xAB
#define PS2_CMD_TEST_PORT2 0xA9
#define PS2_CMD_WRITE_PORT2 0xD4

#define PS2_CONFIG_PORT1_INT 0x01
#define PS2_CONFIG_PORT2_INT 0x02
#define PS2_CONFIG_TRANSLATE 0x40

#define PS2_DEVICE_RESET 0xFF
#define PS2_DEVICE_ENABLE_SCANS 0xF4
#define PS2_DEVICE_DISABLE_SCANS 0xF5
#define PS2_DEVICE_SET_SAMPLE_RATE 0xF3
#define PS2_DEVICE_IDENTIFY 0xF2
#define PS2_DEVICE_SET_DEFAULTS 0xF6

#define PS2_KBD_ACK 0xFA
#define PS2_KBD_RESEND 0xFE
#define PS2_KBD_ERROR 0x00

#define PS2_MOUSE_ACK 0xFA
#define PS2_MOUSE_NACK 0xFE

#define PS2_KBD_EXT 0xE0
#define PS2_KBD_BRK 0xF0

#define SC2_SIZE 128

static const uint8_t sc2_normal[SC2_SIZE] = {
	/* 0x00 */ 0,
	/* 0x01 */ LYR_KEY_F9,
	/* 0x02 */ 0,
	/* 0x03 */ LYR_KEY_F5,
	/* 0x04 */ LYR_KEY_F3,
	/* 0x05 */ LYR_KEY_F1,
	/* 0x06 */ LYR_KEY_F2,
	/* 0x07 */ LYR_KEY_F12,
	/* 0x08 */ 0,
	/* 0x09 */ LYR_KEY_F10,
	/* 0x0A */ LYR_KEY_F8,
	/* 0x0B */ LYR_KEY_F6,
	/* 0x0C */ LYR_KEY_F4,
	/* 0x0D */ LYR_KEY_TAB,
	/* 0x0E */ LYR_KEY_GRAVE,
	/* 0x0F */ 0,
	/* 0x10 */ 0,
	/* 0x11 */ LYR_KEY_LEFTALT,
	/* 0x12 */ LYR_KEY_LEFTSHIFT,
	/* 0x13 */ 0,
	/* 0x14 */ LYR_KEY_LEFTCTRL,
	/* 0x15 */ LYR_KEY_Q,
	/* 0x16 */ LYR_KEY_1,
	/* 0x17 */ 0,
	/* 0x18 */ 0,
	/* 0x19 */ 0,
	/* 0x1A */ LYR_KEY_Z,
	/* 0x1B */ LYR_KEY_S,
	/* 0x1C */ LYR_KEY_A,
	/* 0x1D */ LYR_KEY_W,
	/* 0x1E */ LYR_KEY_2,
	/* 0x1F */ 0,
	/* 0x20 */ 0,
	/* 0x21 */ LYR_KEY_C,
	/* 0x22 */ LYR_KEY_X,
	/* 0x23 */ LYR_KEY_D,
	/* 0x24 */ LYR_KEY_E,
	/* 0x25 */ LYR_KEY_4,
	/* 0x26 */ LYR_KEY_3,
	/* 0x27 */ 0,
	/* 0x28 */ 0,
	/* 0x29 */ LYR_KEY_SPACE,
	/* 0x2A */ LYR_KEY_V,
	/* 0x2B */ LYR_KEY_F,
	/* 0x2C */ LYR_KEY_T,
	/* 0x2D */ LYR_KEY_R,
	/* 0x2E */ LYR_KEY_5,
	/* 0x2F */ 0,
	/* 0x30 */ 0,
	/* 0x31 */ LYR_KEY_N,
	/* 0x32 */ LYR_KEY_B,
	/* 0x33 */ LYR_KEY_H,
	/* 0x34 */ LYR_KEY_G,
	/* 0x35 */ LYR_KEY_Y,
	/* 0x36 */ LYR_KEY_6,
	/* 0x37 */ 0,
	/* 0x38 */ 0,
	/* 0x39 */ 0,
	/* 0x3A */ LYR_KEY_M,
	/* 0x3B */ LYR_KEY_J,
	/* 0x3C */ LYR_KEY_U,
	/* 0x3D */ LYR_KEY_7,
	/* 0x3E */ LYR_KEY_8,
	/* 0x3F */ 0,
	/* 0x40 */ 0,
	/* 0x41 */ LYR_KEY_COMMA,
	/* 0x42 */ LYR_KEY_K,
	/* 0x43 */ LYR_KEY_I,
	/* 0x44 */ LYR_KEY_O,
	/* 0x45 */ LYR_KEY_0,
	/* 0x46 */ LYR_KEY_9,
	/* 0x47 */ 0,
	/* 0x48 */ 0,
	/* 0x49 */ LYR_KEY_DOT,
	/* 0x4A */ LYR_KEY_SLASH,
	/* 0x4B */ LYR_KEY_L,
	/* 0x4C */ LYR_KEY_SEMICOLON,
	/* 0x4D */ LYR_KEY_P,
	/* 0x4E */ LYR_KEY_MINUS,
	/* 0x4F */ 0,
	/* 0x50 */ 0,
	/* 0x51 */ 0,
	/* 0x52 */ LYR_KEY_APOSTROPHE,
	/* 0x53 */ 0,
	/* 0x54 */ LYR_KEY_LEFTBRACE,
	/* 0x55 */ LYR_KEY_EQUAL,
	/* 0x56 */ 0,
	/* 0x57 */ 0,
	/* 0x58 */ LYR_KEY_CAPSLOCK,
	/* 0x59 */ LYR_KEY_RIGHTSHIFT,
	/* 0x5A */ LYR_KEY_ENTER,
	/* 0x5B */ LYR_KEY_RIGHTBRACE,
	/* 0x5C */ 0,
	/* 0x5D */ LYR_KEY_BACKSLASH,
	/* 0x5E */ 0,
	/* 0x5F */ 0,
	/* 0x60 */ 0,
	/* 0x61 */ LYR_KEY_102ND,
	/* 0x62 */ 0,
	/* 0x63 */ 0,
	/* 0x64 */ 0,
	/* 0x65 */ 0,
	/* 0x66 */ LYR_KEY_BACKSPACE,
	/* 0x67 */ 0,
	/* 0x68 */ 0,
	/* 0x69 */ LYR_KEY_KP1,
	/* 0x6A */ 0,
	/* 0x6B */ LYR_KEY_KP4,
	/* 0x6C */ LYR_KEY_KP7,
	/* 0x6D */ 0,
	/* 0x6E */ 0,
	/* 0x6F */ 0,
	/* 0x70 */ LYR_KEY_KP0,
	/* 0x71 */ LYR_KEY_KPDOT,
	/* 0x72 */ LYR_KEY_KP2,
	/* 0x73 */ LYR_KEY_KP5,
	/* 0x74 */ LYR_KEY_KP6,
	/* 0x75 */ LYR_KEY_KP8,
	/* 0x76 */ LYR_KEY_ESC,
	/* 0x77 */ LYR_KEY_NUMLOCK,
	/* 0x78 */ LYR_KEY_F11,
	/* 0x79 */ LYR_KEY_KPPLUS,
	/* 0x7A */ LYR_KEY_KP3,
	/* 0x7B */ LYR_KEY_KPMINUS,
	/* 0x7C */ LYR_KEY_KPASTERISK,
	/* 0x7D */ LYR_KEY_KP9,
	/* 0x7E */ LYR_KEY_SCROLLLOCK,
	/* 0x7F */ 0,
};

static const uint8_t sc2_extended[SC2_SIZE] = {
	[0x11] = LYR_KEY_RIGHTALT, [0x14] = LYR_KEY_RIGHTCTRL,
	[0x6B] = LYR_KEY_LEFT,	   [0x6C] = LYR_KEY_HOME,
	[0x70] = LYR_KEY_INSERT,   [0x71] = LYR_KEY_DELETE,
	[0x72] = LYR_KEY_DOWN,	   [0x74] = LYR_KEY_RIGHT,
	[0x75] = LYR_KEY_UP,	   [0x7A] = LYR_KEY_PAGEDOWN,
	[0x7D] = LYR_KEY_PAGEUP,
};

#define MOUSEBUFFER_SIZE 256

typedef struct {
	volatile uint8_t head;
	volatile uint8_t tail;
	volatile uint16_t count;
	uint8_t buffer[MOUSEBUFFER_SIZE];
} mousebuffer_t;

static mousebuffer_t mousebuf;

static void mousebuffer_put(uint8_t b)
{
	if (mousebuf.count >= MOUSEBUFFER_SIZE)
		return;
	mousebuf.buffer[mousebuf.head++] = b;
	mousebuf.head %= MOUSEBUFFER_SIZE;
	mousebuf.count++;
}

static int mousebuffer_get(uint8_t *out)
{
	if (mousebuf.count == 0)
		return 0;
	*out = mousebuf.buffer[mousebuf.tail++];
	mousebuf.tail %= MOUSEBUFFER_SIZE;
	mousebuf.count--;
	return 1;
}

static int ps2_wait_input(void)
{
	for (int i = 0; i < 100000; i++) {
		if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_IN_BUF))
			return 0;
	}
	return -1;
}

static int ps2_wait_output(void)
{
	for (int i = 0; i < 100000; i++) {
		if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_BUF)
			return 0;
	}
	return -1;
}

static void ps2_cmd_write(uint8_t cmd)
{
	ps2_wait_input();
	outb(PS2_CMD_PORT, cmd);
}

static uint8_t ps2_cmd_read(void)
{
	ps2_wait_output();
	return inb(PS2_DATA_PORT);
}

static void ps2_data_write(uint8_t data)
{
	ps2_wait_input();
	outb(PS2_DATA_PORT, data);
}

static void ps2_flush_output(void)
{
	for (int i = 0; i < 256; i++) {
		if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_BUF))
			break;
		(void)inb(PS2_DATA_PORT);
	}
}

static int ps2_write_device(uint8_t data)
{
	if (ps2_wait_input() != 0)
		return -1;
	outb(PS2_DATA_PORT, data);
	return 0;
}

static int ps2_write_mouse(uint8_t data)
{
	if (ps2_wait_input() != 0)
		return -1;
	outb(PS2_CMD_PORT, PS2_CMD_WRITE_PORT2);
	if (ps2_wait_input() != 0)
		return -1;
	outb(PS2_DATA_PORT, data);
	return 0;
}

static int ps2_read_byte(uint8_t *out)
{
	if (ps2_wait_output() != 0)
		return -1;
	*out = inb(PS2_DATA_PORT);
	return 0;
}

static int ps2_device_command(int mouse, uint8_t cmd, uint8_t *response)
{
	uint8_t ack = 0;
	for (int i = 0; i < 3; i++) {
		if (mouse) {
			if (ps2_write_mouse(cmd) != 0)
				return -1;
		} else if (ps2_write_device(cmd) != 0) {
			return -1;
		}
		if (ps2_read_byte(&ack) != 0)
			return -1;
		if (ack == PS2_KBD_ACK) {
			if (response)
				*response = ack;
			return 0;
		}
		if (ack != PS2_KBD_RESEND)
			break;
	}
	if (response)
		*response = ack;
	return -1;
}

static int sc2_extended_mode;
static int sc2_break_pending;

static void keyboard_handle(uint8_t byte)
{
	if (byte == PS2_KBD_EXT) {
		sc2_extended_mode = 1;
		return;
	}
	if (byte == PS2_KBD_BRK) {
		sc2_break_pending = 1;
		return;
	}

	int is_break = sc2_break_pending;
	int is_ext = sc2_extended_mode;
	sc2_break_pending = 0;
	sc2_extended_mode = 0;

	if (byte >= SC2_SIZE)
		return;

	uint8_t keycode = is_ext ? sc2_extended[byte] : sc2_normal[byte];
	if (keycode == 0)
		return;

	lyr_key_event_t ev = {
		.keycode = keycode,
		.scancode = (uint16_t)(is_ext ? (0xE000u | byte) : byte),
		.mods = 0,
		.down = is_break ? 0 : 1,
		.set = 2,
	};
	kbd_submit_event(&ev);
}

static uint8_t mouse_packet[3];
static int mouse_packet_index;
static int mouse_x;
static int mouse_y;
static uint8_t mouse_buttons;

static void mouse_emit_packet(const uint8_t packet[3])
{
	for (int i = 0; i < 3; i++)
		mousebuffer_put(packet[i]);

	int dx = (packet[0] & 0x10) ? ((int)packet[1] - 256) : packet[1];
	int dy = (packet[0] & 0x20) ? ((int)packet[2] - 256) : packet[2];
	mouse_x += dx;
	mouse_y -= dy;
	mouse_buttons = packet[0] & 0x07;
}

static void mouse_handle(uint8_t b)
{
	if (mouse_packet_index == 0 && !(b & 0x08))
		return;
	mouse_packet[mouse_packet_index++] = b;
	if (mouse_packet_index == 3) {
		mouse_emit_packet(mouse_packet);
		mouse_packet_index = 0;
	}
}

static int mouse_read(void *ctx, uint64_t off, void *buf, size_t len,
					  size_t *done)
{
	(void)ctx;
	(void)off;
	if (done)
		*done = 0;
	if (!buf || len == 0)
		return -EINVAL;

	uint8_t *p = buf;
	size_t count = 0;

	while (count + 3 <= len && mousebuf.count >= 3) {
		uint8_t packet[3];
		for (int i = 0; i < 3; i++)
			mousebuffer_get(&packet[i]);
		memcpy(p + count, packet, 3);
		count += 3;
	}

	if (done)
		*done = count;
	return 0;
}

static int mouse_poll(void *ctx, int events)
{
	(void)ctx;
	int revents = 0;
	if ((events & (LYR_POLLIN | LYR_POLLRDNORM | LYR_POLLRDBAND)) &&
		mousebuf.count >= 3)
		revents |= LYR_POLLIN | LYR_POLLRDNORM;
	return revents;
}

static void ps2_poll(void *ctx)
{
	(void)ctx;
	for (;;) {
		uint8_t status = inb(PS2_STATUS_PORT);
		if (status & PS2_STATUS_OUT_BUF) {
			uint8_t data = inb(PS2_DATA_PORT);
			if (status & PS2_STATUS_AUX_DATA)
				mouse_handle(data);
			else
				keyboard_handle(data);
		}
		for (volatile int i = 0; i < 10000; i++)
			;
	}
}

static int ps2_main(driver_t *driver)
{
	driver_log(driver, "info", "PS/2 driver initializing");

	ps2_cmd_write(PS2_CMD_DISABLE_PORT1);
	ps2_cmd_write(PS2_CMD_DISABLE_PORT2);
	ps2_flush_output();

	ps2_cmd_write(PS2_CMD_READ_CONFIG);
	uint8_t config = ps2_cmd_read();
	config &= (uint8_t) ~(PS2_CONFIG_PORT1_INT | PS2_CONFIG_PORT2_INT |
						  PS2_CONFIG_TRANSLATE);
	ps2_cmd_write(PS2_CMD_WRITE_CONFIG);
	ps2_data_write(config);

	ps2_cmd_write(PS2_CMD_TEST_CONTROLLER);
	uint8_t resp = ps2_cmd_read();
	if (resp != 0x55) {
		driver_log(driver, "err", "controller self-test failed");
		return -ENOSYS;
	}

	int have_keyboard = 0;
	ps2_cmd_write(PS2_CMD_TEST_PORT1);
	resp = ps2_cmd_read();
	if (resp == 0x00) {
		ps2_cmd_write(PS2_CMD_ENABLE_PORT1);
		if (ps2_device_command(0, PS2_DEVICE_SET_DEFAULTS, &resp) == 0 &&
			ps2_device_command(0, PS2_DEVICE_ENABLE_SCANS, &resp) == 0) {
			have_keyboard = 1;
			driver_log(driver, "info", "keyboard detected");
		} else {
			driver_log(driver, "warn", "keyboard did not accept init commands");
		}
	}

	int have_mouse = 0;
	ps2_cmd_write(PS2_CMD_TEST_PORT2);
	resp = ps2_cmd_read();
	if (resp == 0x00) {
		ps2_cmd_write(PS2_CMD_ENABLE_PORT2);
		if (ps2_device_command(1, PS2_DEVICE_SET_DEFAULTS, &resp) == 0 &&
			ps2_device_command(1, PS2_DEVICE_ENABLE_SCANS, &resp) == 0) {
			have_mouse = 1;
			driver_log(driver, "info", "mouse detected");
		} else {
			driver_log(driver, "warn", "mouse did not accept init commands");
		}
	}

	config = 0;
	if (have_keyboard)
		config |= PS2_CONFIG_PORT1_INT;
	if (have_mouse)
		config |= PS2_CONFIG_PORT2_INT;
	ps2_cmd_write(PS2_CMD_WRITE_CONFIG);
	ps2_data_write(config);

	if (!have_keyboard && !have_mouse) {
		driver_log(driver, "err", "no usable PS/2 devices found");
		return -ENOSYS;
	}

	int r = devfs_mkdir("/dev/input", 0755);
	if (r != 0 && r != -EEXIST) {
		driver_log(driver, "err", "failed to create /dev/input");
		return r;
	}

	if (have_mouse) {
		r = devfs_register_chr_poll("/dev/input/mouse", 0444, mouse_read, NULL,
									NULL, mouse_poll, NULL);
		if (r != 0) {
			driver_log(driver, "err", "failed to register /dev/input/mouse");
			return r;
		}

		r = devfs_register_chr_poll("/dev/psaux", 0444, mouse_read, NULL, NULL,
									mouse_poll, NULL);
		if (r != 0 && r != -EEXIST) {
			driver_log(driver, "err", "failed to register /dev/psaux");
			return r;
		}

		r = devfs_register_chr_poll("/dev/input/mice", 0444, mouse_read, NULL,
									NULL, mouse_poll, NULL);
		if (r != 0 && r != -EEXIST) {
			driver_log(driver, "err", "failed to register /dev/input/mice");
			return r;
		}
	}

	r = driver_spawn_thread(driver, "ps2-poll", ps2_poll, NULL);
	if (r != 0) {
		driver_log(driver, "err", "failed to spawn polling thread");
		return r;
	}

	driver_log(driver, "info", "PS/2 driver ready");
	return 0;
}

static const char *const ps2_imports[] = {
	"kbd_submit_event", "devfs_mkdir",		   "devfs_register_chr_poll",
	"driver_log",		"driver_spawn_thread", "kzalloc",
	"memcpy",
};

const driver_metadata_t lyr_driver_metadata = {
	.magic = DRIVER_MAGIC,
	.abi_version = DRIVER_ABI_VERSION,
	.name = "ps2",
	.entry = ps2_main,
	.imports = ps2_imports,
	.import_count = sizeof(ps2_imports) / sizeof(ps2_imports[0]),
};
