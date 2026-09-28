/* Intel HD Audio output for a resident DJGPP client.
 *
 * MMIO is reached through an LDT selector over a DPMI physical mapping.
 * CORB, RIRB, BDL and the sample ring live in one conventional-memory block:
 * JEMM maps the first 640K 1:1, so its linear address is the physical
 * address the controller DMAs from.
 *
 * The stream raises IOC at the end of every BDL entry; the handler refills
 * the ring a fixed distance ahead of the link position.
 */
#include <stdio.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <dos.h>
#include "pci.h"
#include "hda.h"

#define RING_BUFS    8
#define CHUNK_FRAMES 512
#define RING_FRAMES  (RING_BUFS * CHUNK_FRAMES)       /* power of two */
#define RING_MASK    (RING_FRAMES - 1)
#define TARGET_AHEAD (2 * CHUNK_FRAMES)                /* ~21 ms */
#define RENDER_MAX   256

static int hsel;                                       /* MMIO selector */
static unsigned long corb_lin, rirb_lin, bdl_lin, ring_lin;
static int dos_sel;
static uint32_t sd;                                    /* stream descriptor offset */
static int stream_index;
static uint16_t rirb_rp;
static uint32_t write_pos;
static int irq_line;
static hda_render_fn render;
static int16_t mixbuf[RENDER_MAX * 2];
static _go32_dpmi_seginfo old_isr, new_isr;
static int isr_vec;

static inline uint8_t  r8 (uint32_t o) { return _farpeekb(hsel, o); }
static inline uint16_t r16(uint32_t o) { return _farpeekw(hsel, o); }
static inline uint32_t r32(uint32_t o) { return _farpeekl(hsel, o); }
static inline void w8 (uint32_t o, uint8_t v)  { _farpokeb(hsel, o, v); }
static inline void w16(uint32_t o, uint16_t v) { _farpokew(hsel, o, v); }
static inline void w32(uint32_t o, uint32_t v) { _farpokel(hsel, o, v); }

static void io_delay(int n) { while (n--) inportb(0x80); }   /* ~1 us each */

static int wait_bits32(uint32_t off, uint32_t mask, uint32_t want, int us)
{
    while (us-- > 0) {
        if ((r32(off) & mask) == want) return 1;
        io_delay(1);
    }
    return 0;
}

/* ---------------------------------------------------------------- verbs */

static int hda_cmd(uint32_t v, uint32_t *resp)
{
    uint16_t wp = (r16(0x48) + 1) & 0xFF;
    _farpokel(_dos_ds, corb_lin + wp * 4, v);
    w16(0x48, wp);
    for (int i = 0; i < 20000; i++) {
        uint16_t rwp = r16(0x58) & 0xFF;
        if (rwp != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 0xFF;
            if (resp) *resp = _farpeekl(_dos_ds, rirb_lin + rirb_rp * 8);
            w8(0x5D, 0x05);
            return 1;
        }
        io_delay(1);
    }
    return 0;
}

static uint32_t verb(int cad, int nid, uint32_t v, uint32_t payload)    /* 12-bit verb */
{
    uint32_t r = 0;
    hda_cmd((uint32_t)cad << 28 | (uint32_t)nid << 20 | v << 8 | payload, &r);
    return r;
}

static uint32_t verb4(int cad, int nid, uint32_t v, uint32_t payload)   /* 4-bit verb */
{
    uint32_t r = 0;
    hda_cmd((uint32_t)cad << 28 | (uint32_t)nid << 20 | v << 16 | payload, &r);
    return r;
}

static uint32_t param(int cad, int nid, int p) { return verb(cad, nid, 0xF00, p); }

/* ---------------------------------------------------------------- codec */

typedef struct { uint8_t type, nconn; uint8_t conn[16]; uint32_t caps; } Widget;
static Widget widgets[128];

static void unmute_out(int cad, int nid)
{
    uint32_t gain = param(cad, nid, 0x12) & 0x7F;      /* offset = 0 dB */
    verb4(cad, nid, 0x3, 0xB000 | gain);
}

static void unmute_in(int cad, int nid, int index)
{
    uint32_t gain = param(cad, nid, 0x0D) & 0x7F;
    verb4(cad, nid, 0x3, 0x7000 | (index << 8) | gain);
}

static int route_to_dac(int cad, int nid, int depth)
{
    if (depth > 6 || nid >= 128) return -1;
    Widget *w = &widgets[nid];
    if (w->type == 0) return nid;                       /* audio output */
    for (int i = 0; i < w->nconn; i++) {
        int dac = route_to_dac(cad, w->conn[i], depth + 1);
        if (dac < 0) continue;
        if (w->type != 2 && w->nconn > 1) verb(cad, nid, 0x701, i);   /* mixers sum */
        if (w->caps & (1 << 1)) unmute_in(cad, nid, i);
        if (w->caps & (1 << 2)) unmute_out(cad, nid);
        verb(cad, nid, 0x705, 0);
        return dac;
    }
    return -1;
}

static int setup_codec(int cad, uint8_t tag, uint16_t fmt)
{
    uint32_t sub = param(cad, 0, 0x04);
    int fg0 = (sub >> 16) & 0xFF, fgn = sub & 0xFF, any = 0;

    for (int fg = fg0; fg < fg0 + fgn; fg++) {
        if ((param(cad, fg, 0x05) & 0xFF) != 0x01) continue;   /* audio FG only */
        verb(cad, fg, 0x705, 0);
        io_delay(10000);
        uint32_t ws = param(cad, fg, 0x04);
        int start = (ws >> 16) & 0xFF, count = ws & 0xFF;

        for (int n = start; n < start + count && n < 128; n++) {
            Widget *w = &widgets[n];
            w->caps = param(cad, n, 0x09);
            w->type = (w->caps >> 20) & 0xF;
            w->nconn = 0;
            uint32_t cl = param(cad, n, 0x0E);
            int len = cl & 0x7F;
            if (!(w->caps & (1 << 8)) || (cl & 0x80)) continue;
            for (int i = 0; i < len && w->nconn < 16; i += 4) {
                uint32_t e = verb(cad, n, 0xF02, i);
                for (int k = 0; k < 4 && i + k < len; k++)
                    w->conn[w->nconn++] = (e >> (8 * k)) & 0xFF;
            }
        }

        for (int n = start; n < start + count && n < 128; n++) {
            if (widgets[n].type != 4) continue;                /* pin complex */
            uint32_t pcaps = param(cad, n, 0x0C);
            if (!(pcaps & (1 << 4))) continue;                 /* output capable */
            uint32_t cfg = verb(cad, n, 0xF1C, 0);
            int conn = cfg >> 30, dev = (cfg >> 20) & 0xF;
            if (conn == 1) continue;
            if (dev > 0x2) continue;                           /* line out, speaker, HP */
            int dac = route_to_dac(cad, n, 0);
            if (dac < 0) continue;
            verb(cad, n, 0x707, dev == 0x2 ? 0xC0 : 0x40);
            if (pcaps & (1 << 16)) verb(cad, n, 0x70C, 0x02);  /* EAPD */
            verb4(cad, dac, 0x2, fmt);
            verb(cad, dac, 0x706, tag << 4);
            unmute_out(cad, dac);
            verb(cad, dac, 0x705, 0);
            printf("HDA: codec %d pin %d -> DAC %d\n", cad, n, dac);
            any = 1;
        }
    }
    return any;
}

/* ---------------------------------------------------------------- memory */

static int alloc_dma_memory(void)
{
    /* RIRB 2K (2K-aligned) | CORB 1K | BDL 128 | pad | ring 16K (4K offset) */
    const unsigned need = 4096 + RING_FRAMES * 4;
    int paras = (need + 2048 + 15) / 16;
    int seg = __dpmi_allocate_dos_memory(paras, &dos_sel);
    if (seg == -1) return 0;

    unsigned long base = (unsigned long)seg * 16;
    unsigned long a = (base + 2047) & ~2047ul;
    rirb_lin = a;
    corb_lin = a + 2048;
    bdl_lin  = a + 3072;
    ring_lin = a + 4096;

    static const uint8_t zero[512];
    for (unsigned long p = base; p < base + (unsigned long)paras * 16; p += sizeof zero)
        dosmemput(zero, sizeof zero, p);
    return 1;
}

/* ---------------------------------------------------------------- init */

int hda_irq(void) { return irq_line; }

int hda_init(void)
{
    PciDev d;
    if (!pci_find_class(0x04, 0x03, &d)) { printf("HDA: no controller\n"); return 0; }

    uint32_t bar = pci_read(d, 0x10);
    if ((bar & 0x6) == 0x4 && pci_read(d, 0x14) != 0) {
        printf("HDA: BAR above 4 GB\n");
        return 0;
    }
    irq_line = pci_read(d, 0x3C) & 0xFF;
    if (irq_line == 0 || irq_line > 15) { printf("HDA: no legacy IRQ\n"); return 0; }
    pci_write(d, 0x04, (pci_read(d, 0x04) | 0x06) & ~0x400u);   /* mem + master, INTx on */

    __dpmi_meminfo mi;
    mi.address = bar & 0xFFFFFFF0;
    mi.size = 0x4000;
    if (__dpmi_physical_address_mapping(&mi) != 0) { printf("HDA: cannot map BAR\n"); return 0; }
    hsel = __dpmi_allocate_ldt_descriptors(1);
    if (hsel < 0) return 0;
    __dpmi_set_segment_base_address(hsel, mi.address);
    __dpmi_set_segment_limit(hsel, 0x3FFF);

    if (!alloc_dma_memory()) { printf("HDA: out of conventional memory\n"); return 0; }

    /* controller reset */
    w32(0x08, r32(0x08) & ~1u);
    if (!wait_bits32(0x08, 1, 0, 100000)) return 0;
    w32(0x08, r32(0x08) | 1);
    if (!wait_bits32(0x08, 1, 1, 100000)) return 0;
    io_delay(2000);
    uint16_t codecs = r16(0x0E);
    if (!codecs) { printf("HDA: no codecs\n"); return 0; }

    /* CORB / RIRB, 256 entries */
    w8(0x4C, 0); w8(0x5C, 0);
    io_delay(1000);
    w32(0x40, corb_lin); w32(0x44, 0);
    w8(0x4E, 0x02);
    w16(0x48, 0);
    w16(0x4A, 0x8000);
    for (int i = 0; i < 1000 && !(r16(0x4A) & 0x8000); i++) io_delay(1);
    w16(0x4A, 0);
    for (int i = 0; i < 1000 && (r16(0x4A) & 0x8000); i++) io_delay(1);
    w32(0x50, rirb_lin); w32(0x54, 0);
    w8(0x5E, 0x02);
    w16(0x58, 0x8000);
    w16(0x5A, 0xFF);
    rirb_rp = 0;
    w8(0x4C, 0x02);
    w8(0x5C, 0x02);

    uint16_t gcap = r16(0x00);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) { printf("HDA: no output streams\n"); return 0; }
    stream_index = iss;
    sd = 0x80 + iss * 0x20;
    const uint8_t tag = 1;
    const uint16_t fmt = 0x0011;                        /* 48 kHz, 16-bit, stereo */

    int routed = 0;
    for (int cad = 0; cad < 15; cad++)
        if (codecs & (1 << cad)) routed |= setup_codec(cad, tag, fmt);
    if (!routed) { printf("HDA: no usable output pin\n"); return 0; }

    /* stream reset */
    w8(sd, r8(sd) | 1);
    for (int i = 0; i < 1000 && !(r8(sd) & 1); i++) io_delay(1);
    w8(sd, r8(sd) & ~1);
    for (int i = 0; i < 1000 && (r8(sd) & 1); i++) io_delay(1);

    for (int i = 0; i < RING_BUFS; i++) {
        unsigned long e = bdl_lin + i * 16;
        _farpokel(_dos_ds, e + 0, ring_lin + i * CHUNK_FRAMES * 4);
        _farpokel(_dos_ds, e + 4, 0);
        _farpokel(_dos_ds, e + 8, CHUNK_FRAMES * 4);
        _farpokel(_dos_ds, e + 12, 1);                  /* IOC */
    }
    w32(sd + 0x18, bdl_lin);
    w32(sd + 0x1C, 0);
    w32(sd + 0x08, RING_FRAMES * 4);
    w16(sd + 0x0C, RING_BUFS - 1);
    w16(sd + 0x12, fmt);
    w8(sd + 0x02, tag << 4);
    return 1;
}

/* ---------------------------------------------------------------- runtime */

static void refill(void)
{
    uint32_t play = (r32(sd + 0x04) / 4) & RING_MASK;
    uint32_t ahead = (write_pos - play) & RING_MASK;
    if (ahead > TARGET_AHEAD * 2) {                     /* underrun or first run */
        write_pos = (play + TARGET_AHEAD / 2) & RING_MASK;
        ahead = TARGET_AHEAD / 2;
    }
    while (ahead < TARGET_AHEAD) {
        int n = TARGET_AHEAD - ahead;
        if (n > RENDER_MAX) n = RENDER_MAX;
        if (n > (int)(RING_FRAMES - write_pos)) n = RING_FRAMES - write_pos;
        render(mixbuf, n);
        dosmemput(mixbuf, n * 4, ring_lin + write_pos * 4);
        write_pos = (write_pos + n) & RING_MASK;
        ahead += n;
    }
}

static void hda_isr(void)
{
    uint8_t sts = r8(sd + 0x03);
    if (sts & 0x04) {
        w8(sd + 0x03, 0x04);                            /* ack BCIS */
        refill();
    }
    if (irq_line >= 8) outportb(0xA0, 0x20);
    outportb(0x20, 0x20);
}

int hda_start(hda_render_fn fn)
{
    render = fn;
    write_pos = 0;

    isr_vec = irq_line < 8 ? 0x08 + irq_line : 0x70 + irq_line - 8;
    _go32_dpmi_get_protected_mode_interrupt_vector(isr_vec, &old_isr);
    new_isr.pm_offset = (unsigned long)hda_isr;
    new_isr.pm_selector = _go32_my_cs();
    if (_go32_dpmi_allocate_iret_wrapper(&new_isr) != 0) return 0;
    _go32_dpmi_set_protected_mode_interrupt_vector(isr_vec, &new_isr);

    disable();
    refill();
    w32(0x20, 0x80000000u | (1u << stream_index));     /* GIE + this stream */
    w8(sd, 0x02 | 0x04);                                /* run + IOCE */
    if (irq_line >= 8) {
        outportb(0xA1, inportb(0xA1) & ~(1 << (irq_line - 8)));
        outportb(0x21, inportb(0x21) & ~(1 << 2));      /* cascade */
    } else {
        outportb(0x21, inportb(0x21) & ~(1 << irq_line));
    }
    enable();
    return 1;
}

void hda_stop(void)
{
    w8(sd, 0);
    w32(0x20, 0);
    _go32_dpmi_set_protected_mode_interrupt_vector(isr_vec, &old_isr);
}
