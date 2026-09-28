#ifndef IO_H
#define IO_H
#include <stdint.h>

/* Port I/O. JLMs run at ring 0, so these go straight to the hardware. */

static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" : : "a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" : : "a"(v), "Nd"(p)); }
static inline uint8_t  inb(uint16_t p) { uint8_t v;  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static inline void io_delay(int n) { while (n--) inb(0x80); }   /* ~1 us each */

/* Linear memory in Jemm's flat address space (0-1 MB is the V86 address space). */
static inline uint32_t lin_peekl(uint32_t a)
{
    uint32_t v;
    __asm__ volatile("movl (%1),%0" : "=r"(v) : "r"(a) : "memory");
    return v;
}
static inline void lin_pokel(uint32_t a, uint32_t v)
{
    __asm__ volatile("movl %1,(%0)" : : "r"(a), "r"(v) : "memory");
}

#endif
