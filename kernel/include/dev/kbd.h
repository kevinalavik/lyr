#ifndef _LYR_DEV_KBD_H
#define _LYR_DEV_KBD_H

#include <stddef.h>
#include <stdint.h>

#define LYR_KEY_RESERVED 0
#define LYR_KEY_ESC 1
#define LYR_KEY_1 2
#define LYR_KEY_2 3
#define LYR_KEY_3 4
#define LYR_KEY_4 5
#define LYR_KEY_5 6
#define LYR_KEY_6 7
#define LYR_KEY_7 8
#define LYR_KEY_8 9
#define LYR_KEY_9 10
#define LYR_KEY_0 11
#define LYR_KEY_MINUS 12
#define LYR_KEY_EQUAL 13
#define LYR_KEY_BACKSPACE 14
#define LYR_KEY_TAB 15
#define LYR_KEY_Q 16
#define LYR_KEY_W 17
#define LYR_KEY_E 18
#define LYR_KEY_R 19
#define LYR_KEY_T 20
#define LYR_KEY_Y 21
#define LYR_KEY_U 22
#define LYR_KEY_I 23
#define LYR_KEY_O 24
#define LYR_KEY_P 25
#define LYR_KEY_LEFTBRACE 26
#define LYR_KEY_RIGHTBRACE 27
#define LYR_KEY_ENTER 28
#define LYR_KEY_LEFTCTRL 29
#define LYR_KEY_A 30
#define LYR_KEY_S 31
#define LYR_KEY_D 32
#define LYR_KEY_F 33
#define LYR_KEY_G 34
#define LYR_KEY_H 35
#define LYR_KEY_J 36
#define LYR_KEY_K 37
#define LYR_KEY_L 38
#define LYR_KEY_SEMICOLON 39
#define LYR_KEY_APOSTROPHE 40
#define LYR_KEY_GRAVE 41
#define LYR_KEY_LEFTSHIFT 42
#define LYR_KEY_BACKSLASH 43
#define LYR_KEY_Z 44
#define LYR_KEY_X 45
#define LYR_KEY_C 46
#define LYR_KEY_V 47
#define LYR_KEY_B 48
#define LYR_KEY_N 49
#define LYR_KEY_M 50
#define LYR_KEY_COMMA 51
#define LYR_KEY_DOT 52
#define LYR_KEY_SLASH 53
#define LYR_KEY_RIGHTSHIFT 54
#define LYR_KEY_KPASTERISK 55
#define LYR_KEY_LEFTALT 56
#define LYR_KEY_SPACE 57
#define LYR_KEY_CAPSLOCK 58
#define LYR_KEY_F1 59
#define LYR_KEY_F2 60
#define LYR_KEY_F3 61
#define LYR_KEY_F4 62
#define LYR_KEY_F5 63
#define LYR_KEY_F6 64
#define LYR_KEY_F7 65
#define LYR_KEY_F8 66
#define LYR_KEY_F9 67
#define LYR_KEY_F10 68
#define LYR_KEY_NUMLOCK 69
#define LYR_KEY_SCROLLLOCK 70
#define LYR_KEY_KP7 71
#define LYR_KEY_KP8 72
#define LYR_KEY_KP9 73
#define LYR_KEY_KPMINUS 74
#define LYR_KEY_KP4 75
#define LYR_KEY_KP5 76
#define LYR_KEY_KP6 77
#define LYR_KEY_KPPLUS 78
#define LYR_KEY_KP1 79
#define LYR_KEY_KP2 80
#define LYR_KEY_KP3 81
#define LYR_KEY_KP0 82
#define LYR_KEY_KPDOT 83
#define LYR_KEY_F11 87
#define LYR_KEY_F12 88
#define LYR_KEY_102ND 86
#define LYR_KEY_KPENTER 96
#define LYR_KEY_RIGHTCTRL 97
#define LYR_KEY_KPSLASH 98
#define LYR_KEY_RIGHTALT 100
#define LYR_KEY_HOME 102
#define LYR_KEY_UP 103
#define LYR_KEY_PAGEUP 104
#define LYR_KEY_LEFT 105
#define LYR_KEY_RIGHT 106
#define LYR_KEY_END 107
#define LYR_KEY_DOWN 108
#define LYR_KEY_PAGEDOWN 109
#define LYR_KEY_INSERT 110
#define LYR_KEY_DELETE 111
#define LYR_KEY_MAX 128

#define LYR_KBD_MOD_SHIFT 0x0001u
#define LYR_KBD_MOD_CTRL 0x0002u
#define LYR_KBD_MOD_ALT 0x0004u
#define LYR_KBD_MOD_CAPS 0x0008u
#define LYR_KBD_MOD_NUM 0x0010u
#define LYR_KBD_MOD_SCROLL 0x0020u
#define LYR_KBD_MOD_EXT 0x8000u

typedef struct lyr_key_event {
	uint16_t keycode;
	uint16_t scancode;
	uint16_t mods;
	uint8_t down;
	uint8_t set;
} lyr_key_event_t;

#define LYR_KBDIOCSMAP 0x4b01UL
#define LYR_KBDIOCGMAP 0x4b02UL
#define LYR_KBDIOCFLUSH 0x4b03UL
#define LYR_KBD_MAP_PATH_MAX 256

int kbd_init(void);
void kbd_submit_event(const lyr_key_event_t *ev);

int kbd_read_event(lyr_key_event_t *ev);
int kbd_read_byte(uint8_t *ch);

int kbd_load_keymap_file(const char *path);
void kbd_load_default_keymap(void);

#endif // _LYR_DEV_KBD_H
