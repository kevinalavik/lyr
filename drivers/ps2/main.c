#include <cpu/instr.h>
#include <dev/console.h>
#include <dev/device.h>
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

#define KEYMAP_SIZE 128

static const uint8_t keymap_set2_normal[KEYMAP_SIZE] = {
	[0x0d] = 9,
	[0x0e] = '`',
	[0x15] = 'q',
	[0x16] = '1',
	[0x1a] = 'z',
	[0x1b] = 's',
	[0x1c] = 'a',
	[0x1d] = 'w',
	[0x1e] = '2',
	[0x21] = 'c',
	[0x22] = 'x',
	[0x23] = 'd',
	[0x24] = 'e',
	[0x25] = '4',
	[0x26] = '3',
	[0x29] = ' ',
	[0x2a] = 'v',
	[0x2b] = 'f',
	[0x2c] = 't',
	[0x2d] = 'r',
	[0x2e] = '5',
	[0x31] = 'n',
	[0x32] = 'b',
	[0x33] = 'h',
	[0x34] = 'g',
	[0x35] = 'y',
	[0x36] = '6',
	[0x3a] = 'm',
	[0x3b] = 'j',
	[0x3c] = 'u',
	[0x3d] = '7',
	[0x3e] = '8',
	[0x41] = ',',
	[0x42] = 'k',
	[0x43] = 'i',
	[0x44] = 'o',
	[0x45] = '0',
	[0x46] = '9',
	[0x49] = '.',
	[0x4a] = '/',
	[0x4b] = 'l',
	[0x4c] = ';',
	[0x4d] = 'p',
	[0x4e] = '-',
	[0x52] = '\'',
	[0x54] = '[',
	[0x55] = '=',
	[0x5a] = 13,
	[0x5b] = ']',
	[0x5d] = '\\',
	[0x66] = 8,
	[0x76] = 27,
};

static const uint8_t keymap_set2_shift[KEYMAP_SIZE] = {
	[0x0d] = 9,
	[0x0e] = '~',
	[0x15] = 'Q',
	[0x16] = '!',
	[0x1a] = 'Z',
	[0x1b] = 'S',
	[0x1c] = 'A',
	[0x1d] = 'W',
	[0x1e] = '@',
	[0x21] = 'C',
	[0x22] = 'X',
	[0x23] = 'D',
	[0x24] = 'E',
	[0x25] = '$',
	[0x26] = '#',
	[0x29] = ' ',
	[0x2a] = 'V',
	[0x2b] = 'F',
	[0x2c] = 'T',
	[0x2d] = 'R',
	[0x2e] = '%',
	[0x31] = 'N',
	[0x32] = 'B',
	[0x33] = 'H',
	[0x34] = 'G',
	[0x35] = 'Y',
	[0x36] = '^',
	[0x3a] = 'M',
	[0x3b] = 'J',
	[0x3c] = 'U',
	[0x3d] = '&',
	[0x3e] = '*',
	[0x41] = '<',
	[0x42] = 'K',
	[0x43] = 'I',
	[0x44] = 'O',
	[0x45] = ')',
	[0x46] = '(',
	[0x49] = '>',
	[0x4a] = '?',
	[0x4b] = 'L',
	[0x4c] = ':',
	[0x4d] = 'P',
	[0x4e] = '_',
	[0x52] = '"',
	[0x54] = '{',
	[0x55] = '+',
	[0x5a] = 13,
	[0x5b] = '}',
	[0x5d] = '|',
	[0x66] = 8,
	[0x76] = 27,
};

#define KEYBUFFER_SIZE 256

typedef struct {
	volatile uint8_t head;
	volatile uint8_t tail;
	volatile uint16_t count;
	uint8_t buffer[KEYBUFFER_SIZE];
} keybuffer_t;

typedef struct {
	volatile uint8_t head;
	volatile uint8_t tail;
	volatile uint16_t count;
	uint8_t buffer[KEYBUFFER_SIZE];
} mousebuffer_t;

static keybuffer_t keybuf;
static mousebuffer_t mousebuf;
static int shift_pressed;
static int ctrl_pressed;
static int alt_pressed;
static int extended_code;
static int break_pending;

static void keybuffer_put(uint8_t scancode)
{
	if (keybuf.count >= KEYBUFFER_SIZE)
		return;
	keybuf.buffer[keybuf.head++] = scancode;
	keybuf.head %= KEYBUFFER_SIZE;
	keybuf.count++;
}

static int keybuffer_get(uint8_t *out)
{
	if (keybuf.count == 0)
		return 0;
	*out = keybuf.buffer[keybuf.tail++];
	keybuf.tail %= KEYBUFFER_SIZE;
	keybuf.count--;
	return 1;
}

static void mousebuffer_put(uint8_t b)
{
	if (mousebuf.count >= KEYBUFFER_SIZE)
		return;
	mousebuf.buffer[mousebuf.head++] = b;
	mousebuf.head %= KEYBUFFER_SIZE;
	mousebuf.count++;
}

static int mousebuffer_get(uint8_t *out)
{
	if (mousebuf.count == 0)
		return 0;
	*out = mousebuf.buffer[mousebuf.tail++];
	mousebuf.tail %= KEYBUFFER_SIZE;
	mousebuf.count--;
	return 1;
}

static int ps2_wait_input(void)
{
	for (int i = 0; i < 100000; i++) {
		uint8_t status = inb(PS2_STATUS_PORT);
		if (!(status & PS2_STATUS_IN_BUF))
			return 0;
	}
	return -1;
}

static int ps2_wait_output(void)
{
	for (int i = 0; i < 100000; i++) {
		uint8_t status = inb(PS2_STATUS_PORT);
		if (status & PS2_STATUS_OUT_BUF)
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
		uint8_t status = inb(PS2_STATUS_PORT);
		if (!(status & PS2_STATUS_OUT_BUF))
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

static void keyboard_handle(uint8_t scancode)
{
	if (scancode == 0xE0) {
		extended_code = 1;
		return;
	}

	if (scancode == 0xF0) {
		break_pending = 1;
		return;
	}

	int is_break = break_pending;
	break_pending = 0;
	uint8_t code = scancode;

	if (extended_code) {
		extended_code = 0;
		if (code == 0x14)
			ctrl_pressed = !is_break;
		else if (code == 0x11)
			alt_pressed = !is_break;
		return;
	}

	if (code == 0x12 || code == 0x59)
		shift_pressed = !is_break;
	else if (code == 0x14)
		ctrl_pressed = !is_break;
	else if (code == 0x11)
		alt_pressed = !is_break;

	if (!is_break && code < KEYMAP_SIZE) {
		const uint8_t *map = shift_pressed ? keymap_set2_shift : keymap_set2_normal;
		uint8_t ch = map[code];
		if (ch != 0) {
			keybuffer_put(ch);
			console_input_put(ch);
		}
	}
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

static void ps2_wait_for_data(volatile uint16_t *count, uint16_t needed)
{
	while (*count < needed)
		__asm__ volatile("sti; hlt; cli" ::: "memory");
}

static int kbd_read(void *ctx, uint64_t off, void *buf, size_t len,
					size_t *done)
{
	(void)ctx;
	(void)off;

	if (done)
		*done = 0;

	if (!buf)
		return VFS_ERR_INVAL;
	if (len == 0)
		return VFS_OK;

	ps2_wait_for_data(&keybuf.count, 1);

	uint8_t *p = buf;
	size_t count = 0;

	while (count < len) {
		uint8_t ch;
		if (!keybuffer_get(&ch))
			break;
		p[count++] = ch;
	}

	if (done)
		*done = count;
	return VFS_OK;
}

static int mouse_read(void *ctx, uint64_t off, void *buf, size_t len,
					  size_t *done)
{
	(void)ctx;
	(void)off;

	if (done)
		*done = 0;

	if (!buf || len == 0)
		return VFS_ERR_INVAL;

	uint8_t *p = buf;
	size_t count = 0;

	while (count + 3 <= len && mousebuf.count >= 3) {
		uint8_t packet[3];
		for (int i = 0; i < 3; i++)
			mousebuffer_get(&packet[i]);

		memcpy(p + count, packet, 3);
		count += 3;
	}

	*done = count;
	return VFS_OK;
}

static int kbd_poll(void *ctx, int events)
{
	(void)ctx;
	int revents = 0;
	if ((events & (LYR_POLLIN | LYR_POLLRDNORM | LYR_POLLRDBAND)) &&
		keybuf.count > 0)
		revents |= LYR_POLLIN | LYR_POLLRDNORM;
	return revents;
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

	while (1) {
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
	config &= (uint8_t)~(PS2_CONFIG_PORT1_INT | PS2_CONFIG_PORT2_INT |
					   PS2_CONFIG_TRANSLATE);
	ps2_cmd_write(PS2_CMD_WRITE_CONFIG);
	ps2_data_write(config);

	ps2_cmd_write(PS2_CMD_TEST_CONTROLLER);
	uint8_t resp = ps2_cmd_read();
	if (resp != 0x55) {
		driver_log(driver, "err", "controller self-test failed");
		return VFS_ERR_NOSYS;
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
		return VFS_ERR_NOSYS;
	}

	int r = devfs_mkdir("/dev/input", 0755);
	if (r != 0 && r != VFS_ERR_EXIST) {
		driver_log(driver, "err", "failed to create /dev/input");
		return r;
	}

	r = devfs_register_chr_poll("/dev/input/kbd", 0444, kbd_read, NULL, NULL,
								kbd_poll, NULL);
	if (r != 0) {
		driver_log(driver, "err", "failed to register keyboard device");
		return r;
	}

	r = devfs_register_chr_poll("/dev/input/mouse", 0444, mouse_read, NULL,
								NULL, mouse_poll, NULL);
	if (r != 0) {
		driver_log(driver, "err", "failed to register mouse device");
		return r;
	}

	r = devfs_register_chr_poll("/dev/psaux", 0444, mouse_read, NULL, NULL,
								mouse_poll, NULL);
	if (r != 0 && r != VFS_ERR_EXIST) {
		driver_log(driver, "err", "failed to register /dev/psaux");
		return r;
	}

	r = devfs_register_chr_poll("/dev/input/mice", 0444, mouse_read, NULL, NULL,
								mouse_poll, NULL);
	if (r != 0 && r != VFS_ERR_EXIST) {
		driver_log(driver, "err", "failed to register /dev/input/mice");
		return r;
	}

	r = driver_spawn_thread(driver, "ps2-poll", ps2_poll, NULL);
	if (r != VFS_OK) {
		driver_log(driver, "err", "failed to spawn polling thread");
		return r;
	}

	driver_log(driver, "info", "PS/2 driver ready");
	return VFS_OK;
}

static const char *const ps2_imports[] = {
	"console_input_put",
	"devfs_mkdir",
	"devfs_register_chr_poll",
	"driver_log",
	"driver_spawn_thread",
	"kzalloc",
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
