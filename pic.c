#include "io.h"
#include "pic.h"

static uint8_t shadow[2];       /* mask as the game last wrote it */
static uint8_t protect[2];      /* bits SBPRO keeps unmasked */
static int icw_left[2];         /* remaining init words after an ICW1 */

/* The SB IRQ SBPRO delivers never passes through the real 8259, so it has no
   in-service bit there. Without one, the game's EOI would land on the real
   controller and end whatever real interrupt is in service (the timer, say)
   too early. So the emulated IRQ gets a virtual in-service bit that the
   game's EOI clears, and a new SB IRQ waits for it like on real hardware. */
static int vis_irq = -1;        /* emulated IRQ in service, -1 = none */
static int vis_cascade;         /* slave IRQ: master EOI still expected */
static int vis_age;             /* HDA interrupts since delivery */
#define VIS_TIMEOUT 100         /* ~0.3 s: give up on a game that never EOIs */

int pic_watching;               /* set once ports 20h-A1h are trapped */

void pic_virtual_start(int irq)
{
    if (!pic_watching) return;                  /* can't see EOIs: don't wait for one */
    vis_irq = irq;
    vis_cascade = irq >= 8;
    vis_age = 0;
}

int pic_virtual_busy(int irq)
{
    return vis_irq == irq;
}

void pic_tick(void)
{
    if ((vis_irq >= 0 || vis_cascade) && ++vis_age > VIS_TIMEOUT)
        vis_irq = -1, vis_cascade = 0;
}

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

/* An EOI ends the highest-priority interrupt in service, as on a real 8259:
   if a real interrupt of higher priority than the emulated one is in service
   (a timer or keyboard handler), the EOI is its and goes to the controller.
   Otherwise it's for the emulated IRQ. Returns 1 if the EOI was ours. */
static int virtual_eoi(int c, uint8_t v)
{
    int cmd = v & 0xE0, level = v & 7;
    if (cmd != 0x20 && cmd != 0x60) return 0;               /* not an EOI */
    int want;                                               /* our level here */
    if (c == 1) {
        if (vis_irq < 8) return 0;
        want = vis_irq - 8;
    } else if (vis_irq >= 0 && vis_irq < 8) {
        want = vis_irq;
    } else if (vis_cascade && vis_irq < 0) {                /* slave EOI came first */
        want = 2;
    } else return 0;
    if (cmd == 0x60) {
        if (level != want) return 0;                        /* specific EOI, not ours */
    } else if (real_isr(c) & ((2u << want) - 1)) {
        return 0;                                           /* a real one ranks higher */
    }
    if (c == 1 || vis_irq < 8) vis_irq = -1;
    else vis_cascade = 0;
    if (c == 0) vis_cascade = 0;
    return 1;
}

void pic_init(int hda_irq)
{
    shadow[0] = inb(0x21);
    shadow[1] = inb(0xA1);
    protect[0] = protect[1] = 0;
    if (hda_irq < 8) protect[0] = 1 << hda_irq;
    else { protect[0] = 1 << 2; protect[1] = 1 << (hda_irq - 8); }
    icw_left[0] = icw_left[1] = 0;
    vis_irq = -1;
    vis_cascade = 0;
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
        else if (!(v & 0x08) && virtual_eoi(c, v))  /* OCW2: EOI for our IRQ */
            return;
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
