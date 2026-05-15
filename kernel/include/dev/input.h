#ifndef _LYR_DEV_INPUT_H
#define _LYR_DEV_INPUT_H

#include <stddef.h>
#include <stdint.h>

#define LYR_INPUT_PROP_POINTER 0x00u

#define LYR_INPUT_BUS_VIRTUAL 0x06u
#define LYR_INPUT_BUS_I8042 0x11u

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

#define LYR_MOUSE_BUTTON_LEFT 0x01u
#define LYR_MOUSE_BUTTON_RIGHT 0x02u
#define LYR_MOUSE_BUTTON_MIDDLE 0x04u

#define LYR_INPUT_KEYMAP_BYTES_MAX 32u
#define LYR_INPUT_BITSET_BYTES 96u

typedef struct lyr_mouse_event {
	int32_t dx;
	int32_t dy;
	int32_t dz;
	uint32_t buttons;
} lyr_mouse_event_t;

typedef struct lyr_input_id {
	uint16_t bustype;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
} lyr_input_id_t;

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
	uint8_t scancode[LYR_INPUT_KEYMAP_BYTES_MAX];
} lyr_input_keymap_entry_t;

typedef struct lyr_input_mask {
	uint32_t type;
	uint32_t codes_size;
	uint64_t codes_ptr;
} lyr_input_mask_t;

#define LYR_IOC_NRBITS 8u
#define LYR_IOC_TYPEBITS 8u
#define LYR_IOC_SIZEBITS 14u
#define LYR_IOC_DIRBITS 2u

#define LYR_IOC_NRMASK ((1u << LYR_IOC_NRBITS) - 1u)
#define LYR_IOC_TYPEMASK ((1u << LYR_IOC_TYPEBITS) - 1u)
#define LYR_IOC_SIZEMASK ((1u << LYR_IOC_SIZEBITS) - 1u)
#define LYR_IOC_DIRMASK ((1u << LYR_IOC_DIRBITS) - 1u)

#define LYR_IOC_NRSHIFT 0u
#define LYR_IOC_TYPESHIFT (LYR_IOC_NRSHIFT + LYR_IOC_NRBITS)
#define LYR_IOC_SIZESHIFT (LYR_IOC_TYPESHIFT + LYR_IOC_TYPEBITS)
#define LYR_IOC_DIRSHIFT (LYR_IOC_SIZESHIFT + LYR_IOC_SIZEBITS)

#define LYR_IOC_NONE 0u
#define LYR_IOC_WRITE 1u
#define LYR_IOC_READ 2u

#define LYR_IOC(dir, type, nr, size)                                           \
	(((unsigned long)(dir) << LYR_IOC_DIRSHIFT) |                               \
	 ((unsigned long)(type) << LYR_IOC_TYPESHIFT) |                             \
	 ((unsigned long)(nr) << LYR_IOC_NRSHIFT) |                                 \
	 ((unsigned long)(size) << LYR_IOC_SIZESHIFT))

#define LYR_IOR(type, nr, size) LYR_IOC(LYR_IOC_READ, (type), (nr), sizeof(size))
#define LYR_IOW(type, nr, size) LYR_IOC(LYR_IOC_WRITE, (type), (nr), sizeof(size))

#define LYR_EVIOCGVERSION LYR_IOR('E', 0x01, int)
#define LYR_EVIOCGID LYR_IOR('E', 0x02, lyr_input_id_t)
#define LYR_EVIOCGREP LYR_IOR('E', 0x03, unsigned int[2])
#define LYR_EVIOCSREP LYR_IOW('E', 0x03, unsigned int[2])
#define LYR_EVIOCGKEYCODE LYR_IOR('E', 0x04, unsigned int[2])
#define LYR_EVIOCSKEYCODE LYR_IOW('E', 0x04, unsigned int[2])
#define LYR_EVIOCGKEYCODE_V2 LYR_IOR('E', 0x04, lyr_input_keymap_entry_t)
#define LYR_EVIOCSKEYCODE_V2 LYR_IOW('E', 0x04, lyr_input_keymap_entry_t)
#define LYR_EVIOCGNAME(len) LYR_IOC(LYR_IOC_READ, 'E', 0x06, (len))
#define LYR_EVIOCGPHYS(len) LYR_IOC(LYR_IOC_READ, 'E', 0x07, (len))
#define LYR_EVIOCGUNIQ(len) LYR_IOC(LYR_IOC_READ, 'E', 0x08, (len))
#define LYR_EVIOCGPROP(len) LYR_IOC(LYR_IOC_READ, 'E', 0x09, (len))
#define LYR_EVIOCGBIT(ev, len) LYR_IOC(LYR_IOC_READ, 'E', 0x20 + (ev), (len))
#define LYR_EVIOCGABS(abs) LYR_IOR('E', 0x40 + (abs), lyr_input_absinfo_t)
#define LYR_EVIOCSABS(abs) LYR_IOW('E', 0xc0 + (abs), lyr_input_absinfo_t)
#define LYR_EVIOCSFF LYR_IOW('E', 0x80, int)
#define LYR_EVIOCRMFF LYR_IOW('E', 0x81, int)
#define LYR_EVIOCGEFFECTS LYR_IOR('E', 0x84, int)
#define LYR_EVIOCGRAB LYR_IOW('E', 0x90, int)
#define LYR_EVIOCREVOKE LYR_IOW('E', 0x91, int)
#define LYR_EVIOCGMASK LYR_IOR('E', 0x92, lyr_input_mask_t)
#define LYR_EVIOCSMASK LYR_IOW('E', 0x93, lyr_input_mask_t)
#define LYR_EVIOCSCLOCKID LYR_IOW('E', 0xa0, int)

#endif /* _LYR_DEV_INPUT_H */
