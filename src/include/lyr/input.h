#ifndef _LYR_INPUT_H
#define _LYR_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* ioctl request codes; must match kernel <dev/kbd.h>. */
#define LYR_KBDIOCSMAP 0x4b01UL
#define LYR_KBDIOCGMAP 0x4b02UL
#define LYR_KBD_MAP_PATH_MAX 256

#ifndef LYR_KBD_DEVICE
#define LYR_KBD_DEVICE "/dev/input/event0"
#endif

#ifndef LYR_MOUSE_DEVICE
#define LYR_MOUSE_DEVICE "/dev/input/event1"
#endif

#define LYR_MOUSE_BUTTON_LEFT 0x01u
#define LYR_MOUSE_BUTTON_RIGHT 0x02u
#define LYR_MOUSE_BUTTON_MIDDLE 0x04u

typedef struct lyr_mouse_event {
	int32_t dx;
	int32_t dy;
	int32_t dz;
	uint32_t buttons;
} lyr_mouse_event_t;

#define LYR_INPUT_EV_SYN 0x00u
#define LYR_INPUT_EV_KEY 0x01u
#define LYR_INPUT_EV_REL 0x02u
#define LYR_INPUT_EV_ABS 0x03u
#define LYR_INPUT_EV_REP 0x14u

#define LYR_INPUT_SYN_REPORT 0x00u

#define LYR_INPUT_REL_X 0x00u
#define LYR_INPUT_REL_Y 0x01u
#define LYR_INPUT_REL_WHEEL 0x08u

#define LYR_INPUT_BTN_LEFT 0x110u
#define LYR_INPUT_BTN_RIGHT 0x111u
#define LYR_INPUT_BTN_MIDDLE 0x112u

typedef struct lyr_input_timeval {
	int64_t tv_sec;
	int64_t tv_usec;
} lyr_input_timeval_t;

typedef struct lyr_input_event {
	lyr_input_timeval_t time;
	uint16_t type;
	uint16_t code;
	int32_t value;
} lyr_input_event_t;

typedef struct lyr_input_id {
	uint16_t bustype;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
} lyr_input_id_t;

typedef struct lyr_input_absinfo {
	int32_t value;
	int32_t minimum;
	int32_t maximum;
	int32_t fuzz;
	int32_t flat;
	int32_t resolution;
} lyr_input_absinfo_t;

typedef struct lyr_input_keymap_entry {
	uint8_t flags;
	uint8_t len;
	uint16_t index;
	uint32_t keycode;
	uint8_t scancode[32];
} lyr_input_keymap_entry_t;

typedef struct lyr_input_mask {
	uint32_t type;
	uint32_t codes_size;
	uint64_t codes_ptr;
} lyr_input_mask_t;

typedef lyr_input_event_t input_event_t;
typedef lyr_input_id_t input_id_t;
typedef lyr_input_absinfo_t input_absinfo_t;
typedef lyr_input_keymap_entry_t input_keymap_entry_t;
typedef lyr_input_mask_t input_mask_t;

typedef struct lyr_kbd {
	int fd;
} lyr_kbd_t;

int lyr_kbd_open(lyr_kbd_t *kbd);
int lyr_kbd_close(lyr_kbd_t *kbd);
int lyr_kbd_set_layout(lyr_kbd_t *kbd, const char *path);
int lyr_kbd_get_layout(lyr_kbd_t *kbd, char *buf);

#ifdef __cplusplus
}
#endif

#endif /* _LYR_INPUT_H */
