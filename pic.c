#include "io.h"
#include "pic.h"

static uint8_t shadow[2];       /* mask as the game last wrote it */
static uint8_t protect[2];      /* bits SBPRO keeps unmasked */
static int icw_left[2];         /* remaining init words after an ICW1 */

int pic_watching;               /* set once ports 20h-A1h are trapped */

static uint8_t ocw3_read[2] = { 0x0A, 0x0A };   /* game's IRR/ISR read select */

/* In-service register of the real controller. */
static uint8_t real_isr(int c)
{
    uint16_t port = c ? 0xA0 : 0x20;
    outb(port, 0x0B);
    uint8_t isr = inb(port);
    outb(port, ocw3_read[c]);
    return isr;
}

/* The emulated SB IRQ never passes through the real 8259, so SBPRO applies
   the 8259's priority rule itself: while a real interrupt of the same or
   higher priority is in service (the game's timer or keyboard handler hasn't
   sent its EOI yet), the SB IRQ waits. That way the SB handler never runs
   inside those handlers, and its EOI always finds the real controller idle
   (a no-op there), so it can't end someone else's interrupt. */
int pic_real_busy(int irq)
{
    if (irq < 8) return (real_isr(0) & ((2u << irq) - 1)) != 0;
    return (real_isr(0) & 0x07) || (real_isr(1) & ((2u << (irq - 8)) - 1));
}

void pic_init(int hda_irq)
{
    shadow[0] = inb(0x21);
    shadow[1] = inb(0xA1);
    protect[0] = protect[1] = 0;
    if (hda_irq < 8) protect[0] = 1 << hda_irq;
    else { protect[0] = 1 << 2; protect[1] = 1 << (hda_irq - 8); }
    icw_left[0] = icw_left[1] = 0;
}

int pic_owns(uint16_t port)
{
    return port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1;
}

uint8_t pic_in(uint16_t port)
{
    if (port == 0x21) return shadow[0];
    if (port == 0xA1) return shadow[1];
    return inb(port);                               /* IRR/ISR reads */
}

void pic_out(uint16_t port, uint8_t v)
{
    int c = (port & 0x80) ? 1 : 0;
    if ((port & 1) == 0) {                          /* 20h / A0h */
        if (v & 0x10)                               /* ICW1: init sequence follows */
            icw_left[c] = 1 + !(v & 0x02) + (v & 0x01);
        else if ((v & 0x08) && (v & 0x02))          /* OCW3: IRR/ISR read select */
            ocw3_read[c] = 0x08 | (v & 0x03);
        outb(port, v);
        return;
    }
    if (icw_left[c]) {                              /* ICW2-4 go through untouched */
        icw_left[c]--;
        outb(port, v);
        return;
    }
    shadow[c] = v;
    outb(port, v & ~protect[c]);
}

int pic_masked(int irq)
{
    if (irq < 8) return (shadow[0] >> irq) & 1;
    return ((shadow[1] >> (irq - 8)) & 1) || ((shadow[0] >> 2) & 1);
}

void pic_restore(void)
{
    outb(0x21, shadow[0]);
    outb(0xA1, shadow[1]);
}
