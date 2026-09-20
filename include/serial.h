#ifndef NUGGET_SERIAL_H
#define NUGGET_SERIAL_H

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

#endif
