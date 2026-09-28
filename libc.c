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

static void putch(Client *c, char ch)
{
    if (ch == '\n') putch(c, '\r');
    c->EAX = 0x0200;
    c->EDX = (uint8_t)ch;
    jlm_nest_int(c, 0x21);
}

static void putnum(Client *c, uint32_t v, int base, int width)
{
    char buf[12];
    int n = 0;
    do {
        int d = v % base;
        buf[n++] = d < 10 ? '0' + d : 'A' + d - 10;
        v /= base;
    } while (v);
    while (n < width) buf[n++] = '0';
    while (n) putch(c, buf[--n]);
}

void jprintf(const char *fmt, ...)
{
    Client *c = jlm_client();
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { putch(c, *fmt); continue; }
        int width = 0;
        fmt++;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { putch(c, '-'); v = -v; }
            putnum(c, (uint32_t)v, 10, width);
            break;
        }
        case 'u': putnum(c, va_arg(ap, uint32_t), 10, width); break;
        case 'x': case 'X': putnum(c, va_arg(ap, uint32_t), 16, width); break;
        case 's': for (const char *s = va_arg(ap, const char *); *s; s++) putch(c, *s); break;
        case 'c': putch(c, (char)va_arg(ap, int)); break;
        case '%': putch(c, '%'); break;
        case 0: fmt--; break;
        }
    }
    va_end(ap);
}
