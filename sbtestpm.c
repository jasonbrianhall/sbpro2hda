/* SBTESTPM - Sound Blaster detection steps from a protected-mode (DPMI)
 * program, the way DOS/4GW games do them. Run under SBPM:  SBPM SBTESTPM
 * Assumes A220 I5 D1 P330. Results go to the screen and COM2.
 */
#include "version.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <pc.h>
#include <dpmi.h>
#include <go32.h>
#include <crt0.h>
#include <sys/farptr.h>
#include <sys/movedata.h>

int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY;

#define BASE 0x220
#define IRQ  5

static volatile int irqs;
static _go32_dpmi_seginfo old_isr, new_isr;
static unsigned long dma_lin;

static void say(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    for (char *p = buf; *p; p++) {
        if (*p == '\n') { while (!(inportb(0x2FD) & 0x20)) ; outportb(0x2F8, '\r'); }
        while (!(inportb(0x2FD) & 0x20)) ;
        outportb(0x2F8, *p);
    }
}

static unsigned ticks(void) { return _farpeekl(_dos_ds, 0x46C); }

static void isr(void)
{
    inportb(BASE + 0x0E);
    irqs++;
    outportb(0x20, 0x20);
}

static void dsp_w(int v)
{
    for (int i = 0; i < 65535 && (inportb(BASE + 0x0C) & 0x80); i++) ;
    outportb(BASE + 0x0C, v);
}

static int dsp_r(void)
{
    for (int i = 0; i < 65535 && !(inportb(BASE + 0x0E) & 0x80); i++) ;
    return inportb(BASE + 0x0A);
}

static int wait_irqs(int want, int max_ticks)
{
    unsigned end = ticks() + max_ticks;
    while (irqs < want && ticks() != end) ;
    return irqs >= want;
}

static void setup_dma(int len, int autoinit)
{
    outportb(0x0A, 0x05);
    outportb(0x0C, 0);
    outportb(0x0B, autoinit ? 0x59 : 0x49);
    outportb(0x02, dma_lin & 0xFF);
    outportb(0x02, (dma_lin >> 8) & 0xFF);
    outportb(0x83, (dma_lin >> 16) & 0xFF);
    outportb(0x03, (len - 1) & 0xFF);
    outportb(0x03, ((len - 1) >> 8) & 0xFF);
    outportb(0x0A, 0x01);
}

static void fm(int reg, int val)
{
    outportb(0x388, reg);
    for (int i = 0; i < 6; i++) inportb(0x388);
    outportb(0x389, val);
    for (int i = 0; i < 35; i++) inportb(0x388);
}

static void mpu_cmd(int v)
{
    for (int i = 0; i < 65535 && (inportb(0x331) & 0x40); i++) ;
    outportb(0x331, v);
}

static int mpu_read(void)
{
    for (int i = 0; i < 65535; i++)
        if (!(inportb(0x331) & 0x80)) return inportb(0x330);
    return -1;
}

static void mpu_data(int v)
{
    for (int i = 0; i < 65535 && (inportb(0x331) & 0x40); i++) ;
    outportb(0x330, v);
}

int main(void)
{
    say("SBTESTPM " SBPRO_VERSION " (protected mode)\n");

    outportb(BASE + 6, 1);
    for (int i = 0; i < 100; i++) inportb(0x80);
    outportb(BASE + 6, 0);
    say("reset: %02X\n", dsp_r());
    dsp_w(0xE1);
    int hi = dsp_r(), lo = dsp_r();
    say("version: %02X%02X\n", hi, lo);

    int busy = 0;
    for (int i = 0; i < 1000 && !busy; i++) busy = inportb(BASE + 0x0C) & 0x80;
    say("busy bit: %s\n", busy ? "seen" : "never set");

    /* DMA buffer in conventional memory: a 1111 Hz square wave at 11111 Hz */
    int sel, seg = __dpmi_allocate_dos_memory(8192 / 16, &sel);
    dma_lin = (unsigned long)seg * 16;
    if ((dma_lin & 0xFFFF) + 4000 > 0x10000) dma_lin = (dma_lin + 0xFFFF) & ~0xFFFFul;
    for (int i = 0; i < 4000; i++) _farpokeb(_dos_ds, dma_lin + i, (i % 10) < 5 ? 0x40 : 0xC0);

    /* PM IRQ handler */
    _go32_dpmi_get_protected_mode_interrupt_vector(8 + IRQ, &old_isr);
    new_isr.pm_offset = (unsigned long)isr;
    new_isr.pm_selector = _go32_my_cs();
    _go32_dpmi_allocate_iret_wrapper(&new_isr);
    _go32_dpmi_set_protected_mode_interrupt_vector(8 + IRQ, &new_isr);
    int old_mask = inportb(0x21);
    outportb(0x21, old_mask & ~(1 << IRQ));

    irqs = 0;
    dsp_w(0xF2);
    say("F2h IRQ: %s\n", wait_irqs(1, 9) ? "yes" : "NO");

    dsp_w(0xD1);
    dsp_w(0x40); dsp_w(0xA6);

    setup_dma(4000, 0);
    irqs = 0;
    dsp_w(0x14); dsp_w(3999 & 0xFF); dsp_w(3999 >> 8);
    outportb(0x0C, 0);
    int c0 = inportb(0x03) | (inportb(0x03) << 8);
    say("4000-byte DMA IRQ: %s\n", wait_irqs(1, 55) ? "yes" : "NO");
    outportb(0x0C, 0);
    int c1 = inportb(0x03) | (inportb(0x03) << 8);
    say("DMA count: start %04X, end %04X (want FFFF)\n", c0, c1);

    setup_dma(4000, 1);
    irqs = 0;
    dsp_w(0x48); dsp_w(1999 & 0xFF); dsp_w(1999 >> 8);
    dsp_w(0x1C);
    wait_irqs(3, 55);
    dsp_w(0xDA);
    say("auto-init IRQs in 3 s (want 3): %d\n", irqs);

    /* AdLib timer detection and a note */
    fm(4, 0x60); fm(4, 0x80);
    int s1 = inportb(0x388);
    fm(2, 0xFF); fm(4, 0x21);
    for (int i = 0; i < 200; i++) inportb(0x80);
    int s2 = inportb(0x388);
    fm(4, 0x60); fm(4, 0x80);
    say("AdLib: %s\n", ((s1 & 0xE0) == 0 && (s2 & 0xE0) == 0xC0) ? "detected" : "NOT detected");
    static const unsigned char note[] = { 0x20,0x01, 0x40,0x10, 0x60,0xF0, 0x80,0x77, 0xA0,0x98,
                                          0x23,0x01, 0x43,0x00, 0x63,0xF0, 0x83,0x77, 0xB0,0x31 };
    unsigned t0 = ticks();
    for (unsigned i = 0; i < sizeof note; i += 2) fm(note[i], note[i + 1]);
    unsigned t1 = ticks();
    while (ticks() - t0 < 18) ;
    fm(0xB0, 0x11);
    say("AdLib note played; 10 register writes took %u ticks\n", t1 - t0);

    /* MPU-401 */
    mpu_cmd(0xFF);
    int a1 = mpu_read();
    mpu_cmd(0x3F);
    int a2 = mpu_read();
    say("MPU-401 ACKs (want FE FE): %02X %02X\n", a1 & 0xFF, a2 & 0xFF);
    mpu_data(0xC0); mpu_data(0x00); mpu_data(0x90); mpu_data(0x3C); mpu_data(0x7F);
    t0 = ticks();
    while (ticks() - t0 < 18) ;
    mpu_data(0x80); mpu_data(0x3C); mpu_data(0x00);
    say("MIDI middle C played\n");

    /* Absolute PIC mask: only IRQ 0, 1, 5 open */
    outportb(0x21, 0xDC);
    setup_dma(4000, 0);
    irqs = 0;
    dsp_w(0x14); dsp_w(3999 & 0xFF); dsp_w(3999 >> 8);
    say("IRQ with absolute PIC mask: %s\n", wait_irqs(1, 55) ? "yes" : "NO");

    outportb(0x21, old_mask & ~(1 << IRQ));

    /* Doom-like load: auto-init digital playback plus a MIDI note stream
       (status polled before every byte) for ~3 s. */
    setup_dma(4000, 1);
    irqs = 0;
    dsp_w(0x48); dsp_w(999 & 0xFF); dsp_w(999 >> 8);
    dsp_w(0x1C);
    unsigned start = ticks(), sent = 0;
    int n = 0;
    while (ticks() - start < 55) {
        int ch = n % 3, note = 48 + (n * 7) % 24;
        mpu_data(0x90 | ch); mpu_data(note); mpu_data(0x70);
        mpu_data(0x80 | ((n + 2) % 3)); mpu_data(48 + ((n - 2) * 7 + 48) % 24); mpu_data(0);
        mpu_data(0xB0 | ch); mpu_data(7); mpu_data(100);
        sent += 9;
        n++;
        unsigned t = ticks();
        while (ticks() == t) ;
    }
    dsp_w(0xDA);
    for (int c = 0; c < 3; c++) { mpu_data(0xB0 | c); mpu_data(123); mpu_data(0); }
    say("stress: %u MIDI bytes in 3 s alongside digital, %d SB IRQs (want ~33)\n", sent, irqs);

    outportb(0x21, old_mask);
    _go32_dpmi_set_protected_mode_interrupt_vector(8 + IRQ, &old_isr);
    say("done\n");
    return 0;
}
