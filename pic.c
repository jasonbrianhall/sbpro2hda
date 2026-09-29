#include "io.h"
#include "pic.h"

static uint8_t shadow[2];       /* mask as the game last wrote it */
static uint8_t protect[2];      /* bits SBPRO keeps unmasked */
static int icw_left[2];         /* remaining init words after an ICW1 */

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
