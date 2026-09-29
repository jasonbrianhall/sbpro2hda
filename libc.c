/* The little bit of C runtime a JLM needs: memset/memcpy (gcc may emit
   calls to them) and a printf that writes through DOS during load. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "jlm.h"

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    while (n--) *p++ = *q++;
    return d;
}

/* Output sinks: DOS console (load time) and the serial port (/D). */
int dbg_on;
typedef void (*sink_fn)(char);

static Client *con_client;
static void con_putc(char ch)
{
    con_client->EAX = 0x0200;
    con_client->EDX = (uint8_t)ch;
    jlm_nest_int(con_client, 0x21);
}

/* Debug output: COM1 at 115200 8N1. */
#define COM 0x3F8

static void outp(uint16_t port, uint8_t v) { __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(port)); }
static uint8_t inp(uint16_t port) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }

void dbg_init(void)
{
    outp(COM + 1, 0x00);                /* no UART interrupts */
    outp(COM + 3, 0x80);                /* DLAB */
    outp(COM + 0, 0x01);                /* divisor 1 = 115200 baud */
    outp(COM + 1, 0x00);
    outp(COM + 3, 0x03);                /* 8N1 */
    outp(COM + 2, 0xC7);                /* FIFO on, cleared */
    outp(COM + 4, 0x03);                /* DTR + RTS */
    dbg_on = 1;
}

/* Never block the game: if the UART isn't draining, drop characters. */
static void serial_putc(char ch)
{
    for (int i = 0; i < 200; i++) {
        if (inp(COM + 5) & 0x20) { outp(COM, (uint8_t)ch); return; }
    }
}

static void putch(sink_fn out, char ch)
{
    if (ch == '\n') out('\r');
    out(ch);
}

static void putnum(sink_fn out, uint32_t v, int base, int width)
{
    char buf[12];
    int n = 0;
    do {
        int d = v % base;
        buf[n++] = d < 10 ? '0' + d : 'A' + d - 10;
        v /= base;
    } while (v);
    while (n < width) buf[n++] = '0';
    while (n) putch(out, buf[--n]);
}

static void vformat(sink_fn out, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { putch(out, *fmt); continue; }
        int width = 0;
        fmt++;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { putch(out, '-'); v = -v; }
            putnum(out, (uint32_t)v, 10, width);
            break;
        }
        case 'u': putnum(out, va_arg(ap, uint32_t), 10, width); break;
        case 'x': case 'X': putnum(out, va_arg(ap, uint32_t), 16, width); break;
        case 's': for (const char *s = va_arg(ap, const char *); *s; s++) putch(out, *s); break;
        case 'c': putch(out, (char)va_arg(ap, int)); break;
        case '%': putch(out, '%'); break;
        case 0: fmt--; break;
        }
    }
}

void jprintf(const char *fmt, ...)
{
    va_list ap;
    con_client = jlm_client();
    va_start(ap, fmt);
    vformat(con_putc, fmt, ap);
    va_end(ap);
}

void dbg(const char *fmt, ...)
{
    if (!dbg_on) return;
    va_list ap;
    va_start(ap, fmt);
    vformat(serial_putc, fmt, ap);
    va_end(ap);
}
