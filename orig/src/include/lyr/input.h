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
