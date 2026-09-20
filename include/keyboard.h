#ifndef NUGGET_KEYBOARD_H
#define NUGGET_KEYBOARD_H
#include "types.h"

void keyboard_init(void);
/* Returns 0 if no key waiting, else the ASCII char (0 for non-printable) */
char keyboard_getchar(void);
bool keyboard_has_data(void);

#endif
