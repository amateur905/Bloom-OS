#pragma once

#include <stdint.h>

#define KEY_UP     0x101
#define KEY_DOWN   0x102
#define KEY_LEFT   0x103
#define KEY_RIGHT  0x104
#define KEY_HOME   0x105
#define KEY_END    0x106
#define KEY_DELETE 0x107
#define KEY_PGUP   0x108
#define KEY_PGDN   0x109

void InitKeyboard(void);
char KeyboardGetChar(void);
int KeyboardGetKey(void);
int KeyboardCapsLock(void);
int KeyboardNumLock(void);
int KeyboardScrollLock(void);
