/* Intel HD Audio output for SBPRO.DLL (runs inside Jemm at ring 0).
 *
 * The controller registers are mapped with _PageReserve/_PageCommitPhys.
 * CORB, RIRB, BDL and the sample ring live in one locked XMS block, so the
 * controller gets its physical address and we write through a mapping.
 *
 * The stream raises IOC at the end of every BDL entry. The IRQ's real-mode
 * vector points at a V86 callback, so the handler runs here at ring 0
 * whether the interrupt arrived in V86 mode or was reflected down by a
 * DOS extender.
 */
#include <string.h>
#include "io.h"
#include "jlm.h"
#include "pci.h"
#include "hda.h"
#include "pic.h"
#include "sbout.h"

#define RING_BUFS    32
#define CHUNK_FRAMES 128                               /* IRQ every 2.7 ms */
#define RING_FRAMES  (RING_BUFS * CHUNK_FRAMES)       /* power of two */
#define RING_MASK    (RING_FRAMES - 1)
#define TARGET_AHEAD (8 * CHUNK_FRAMES)                /* ~21 ms */

/* DMA block layout, 4 KB aligned */
#define RIRB_OFF  0x0000        /* 2 KB */
#define CORB_OFF  0x0800        /* 1 KB */
#define BDL_OFF   0x0C00        /* 512 bytes */
#define RING_OFF  0x1000        /* 16 KB */
#define DMA_BYTES (RING_OFF + RING_FRAMES * 4)
#define DMA_PAGES (DMA_BYTES / 4096)

static volatile uint8_t *mmio;
static uint32_t mmio_lin;
static uint8_t *dma;
static uint32_t dma_lin, dma_phys;
static uint32_t xms_entry;
static uint16_t xms_handle;
static int xms_locked;

static uint32_t sd;
static int stream_index;
static uint16_t rirb_rp;
static uint32_t write_pos;
static int irq_line = -1;
static int running;
static hda_render_fn render;
static uint32_t irq_callback;
static int16_t *ring;

static inline uint8_t  r8 (uint32_t o) { return *(volatile uint8_t  *)(mmio + o); }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(mmio + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(mmio + o); }
static inline void w8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(mmio + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(mmio + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(mmio + o) = v; }

static int wait_bits32(uint32_t off, uint32_t mask, uint32_t want, int us)
{
    while (us-- > 0) {
        if ((r32(off) & mask) == want) return 1;
        io_delay(1);
    }
    return 0;
}

/* ---------------------------------------------------------------- XMS */

static int xms_call(Client *c, uint8_t fn, uint16_t dx)
{
    c->EAX = (uint32_t)fn << 8;
    c->EDX = dx;
    jlm_nest_far_call(c, xms_entry);
    return (c->EAX & 0xFFFF) == 1;
}

static int alloc_dma_memory(void)
{
    Client *c = jlm_client();

    c->EAX = 0x4300;
    jlm_nest_int(c, 0x2F);
    if ((c->EAX & 0xFF) != 0x80) { jprintf("HDA: no XMS driver\n"); return 0; }
    c->EAX = 0x4310;
    jlm_nest_int(c, 0x2F);
    xms_entry = (c->ES & 0xFFFF) << 16 | (c->EBX & 0xFFFF);

    if (!xms_call(c, 0x09, DMA_BYTES / 1024 + 4)) { jprintf("HDA: XMS allocation failed\n"); return 0; }
    xms_handle = c->EDX & 0xFFFF;
    c->EDX = xms_handle;
    if (!xms_call(c, 0x0C, xms_handle)) { jprintf("HDA: XMS lock failed\n"); return 0; }
    xms_locked = 1;
    dma_phys = ((c->EDX & 0xFFFF) << 16 | (c->EBX & 0xFFFF));
    dma_phys = (dma_phys + 4095) & ~4095u;

    dma_lin = jlm_page_reserve(PR_SYSTEM, DMA_PAGES, 0);
    if (dma_lin == 0xFFFFFFFFu || dma_lin == 0) { dma_lin = 0; return 0; }
    if (!jlm_page_commit_phys(dma_lin >> 12, DMA_PAGES, dma_phys >> 12, PC_INCR | PC_WRITEABLE))
        return 0;
    dma = (uint8_t *)dma_lin;
    ring = (int16_t *)(dma + RING_OFF);
    memset(dma, 0, DMA_BYTES);
    return 1;
}

static void free_dma_memory(void)
{
    Client *c = jlm_client();
    if (mmio) {                                         /* stop CORB/RIRB DMA first */
        w8(0x4C, 0);
        w8(0x5C, 0);
        if (sd) w8(sd, 0);
    }
    if (dma_lin) { jlm_page_free(dma_lin, 0); dma_lin = 0; dma = 0; }
    if (xms_handle) {
        if (xms_locked) xms_call(c, 0x0D, xms_handle);
        xms_call(c, 0x0A, xms_handle);
        xms_handle = 0;
        xms_locked = 0;
    }
    if (mmio_lin) { jlm_page_free(mmio_lin, 0); mmio_lin = 0; mmio = 0; }
}

/* ---------------------------------------------------------------- verbs */

static int hda_cmd(uint32_t v, uint32_t *resp)
{
    volatile uint32_t *corb = (volatile uint32_t *)(dma + CORB_OFF);
    volatile uint32_t *rirb = (volatile uint32_t *)(dma + RIRB_OFF);
    uint16_t wp = (r16(0x48) + 1) & 0xFF;
    corb[wp] = v;
    w16(0x48, wp);
    for (int i = 0; i < 20000; i++) {
        uint16_t rwp = r16(0x58) & 0xFF;
        if (rwp != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 0xFF;
            if (resp) *resp = rirb[rirb_rp * 2];
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

static int setup_codec(int cad, uint8_t tag, uint16_t fmt, int digital)
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

        if (dbg_on) {                                   /* /D: the codec's layout */
            dbg("HDA: codec %d vendor %08X rev %08X, function group %d, widgets %d-%d\n",
                cad, param(cad, 0, 0x00), param(cad, 0, 0x02), fg, start, start + count - 1);
            for (int n = start; n < start + count && n < 128; n++) {
                Widget *w = &widgets[n];
                dbg("  nid %02X type %X caps %08X", n, w->type, w->caps);
                if (w->type == 4)
                    dbg(" pincaps %08X config %08X", param(cad, n, 0x0C), verb(cad, n, 0xF1C, 0));
                if (w->nconn) {
                    dbg(" conn");
                    for (int i = 0; i < w->nconn; i++) dbg(" %02X", w->conn[i]);
                }
                dbg("\n");
            }
        }

        for (int n = start; n < start + count && n < 128; n++) {
            if (widgets[n].type != 4) continue;                /* pin complex */
            uint32_t pcaps = param(cad, n, 0x0C);
            if (!(pcaps & (1 << 4))) continue;                 /* output capable */
            uint32_t cfg = verb(cad, n, 0xF1C, 0);
            int conn = cfg >> 30, dev = (cfg >> 20) & 0xF;
            if (conn == 1) continue;                           /* nothing attached */
            if (digital) {
                if (dev != 0x4 && dev != 0x5) continue;        /* S/PDIF, HDMI/DP */
                if ((pcaps & (1 << 2)) && !(verb(cad, n, 0xF09, 0) & 0x80000000u))
                    continue;                                  /* no monitor on it */
            } else if (dev > 0x2) continue;                    /* line out, speaker, HP */
            int dac = route_to_dac(cad, n, 0);
            if (dac < 0) continue;
            verb(cad, n, 0x707, dev == 0x2 ? 0xC0 : 0x40);
            if (pcaps & (1 << 16)) verb(cad, n, 0x70C, 0x02);  /* EAPD */
            verb4(cad, dac, 0x2, fmt);
            verb(cad, dac, 0x706, tag << 4);
            if (digital) {
                verb(cad, dac, 0x72D, 1);                      /* 2 channels */
                verb(cad, dac, 0x70D, 0x01);                   /* digital converter on */
            }
            unmute_out(cad, dac);
            verb(cad, dac, 0x705, 0);
            jprintf("HDA: codec %d pin %d -> %s %d\n", cad, n, digital ? "digital out" : "DAC", dac);
            any = 1;
        }
    }
    return any;
}

/* ---------------------------------------------------------------- init */

int hda_irq(void) { return irq_line; }

static int fail(const char *msg)
{
    if (msg) jprintf("HDA: %s\n", msg);
    free_dma_memory();
    return 0;
}

/* Chipset settings Linux's HD Audio driver makes on every boot: without
   them some controllers read stale data from the CPU cache (noise or
   silence) or play static. */
static void pci_quirks(PciDev d)
{
    uint32_t id = pci_read(d, 0x00);
    uint16_t ven = id & 0xFFFF, dev = id >> 16;
    dbg("HDA: controller %04X:%04X at %02X:%02X.%X\n", ven, dev, d.bus, d.dev, d.fn);
    if (ven == 0x8086) {
        pci_update_byte(d, 0x44, 0x07, 0);              /* TCSEL: traffic class 0 */
        if (dev == 0x811B || dev == 0x080A || dev == 0x0F04 || dev == 0x2284) {
            uint32_t devc = pci_read(d, 0x78);          /* SCH: turn snooping on */
            if (devc & 0x800) pci_write(d, 0x78, devc & ~0x800u);
        }
    } else if (ven == 0x1002 || ven == 0x1022) {        /* ATI / AMD: enable snoop */
        pci_update_byte(d, 0x42, 0x07, 0x02);
    } else if (ven == 0x10DE) {                         /* NVIDIA: coherent DMA */
        pci_update_byte(d, 0x4E, 0x0F, 0x0F);
        pci_update_byte(d, 0x4D, 0x01, 0x01);
        pci_update_byte(d, 0x4C, 0x01, 0x01);
    }
}

static int try_controller(PciDev d, int digital)
{
    sd = 0;
    pci_quirks(d);
    uint32_t bar = pci_read(d, 0x10);
    if ((bar & 0x6) == 0x4 && pci_read(d, 0x14) != 0) return fail("BAR above 4 GB");
    irq_line = pci_read(d, 0x3C) & 0xFF;
    if (irq_line == 0 || irq_line > 15) return fail("no legacy IRQ");
    pci_write(d, 0x04, (pci_read(d, 0x04) | 0x06) & ~0x400u);   /* mem + master, INTx on */

    mmio_lin = jlm_page_reserve(PR_SYSTEM, 4, 0);
    if (mmio_lin == 0xFFFFFFFFu || mmio_lin == 0) { mmio_lin = 0; return fail("no address space"); }
    if (!jlm_page_commit_phys(mmio_lin >> 12, 4, (bar & 0xFFFFF000u) >> 12, PC_INCR | PC_WRITEABLE))
        return fail("cannot map BAR");
    mmio = (volatile uint8_t *)(mmio_lin + (bar & 0xFF0));

    if (!alloc_dma_memory()) return fail(0);

    /* controller reset */
    w32(0x08, r32(0x08) & ~1u);
    if (!wait_bits32(0x08, 1, 0, 100000)) return fail("reset timeout");
    w32(0x08, r32(0x08) | 1);
    if (!wait_bits32(0x08, 1, 1, 100000)) return fail("reset timeout");
    io_delay(2000);
    uint16_t codecs = r16(0x0E);
    if (!codecs) return fail("no codecs");

    /* CORB / RIRB, 256 entries */
    w8(0x4C, 0); w8(0x5C, 0);
    io_delay(1000);
    w32(0x40, dma_phys + CORB_OFF); w32(0x44, 0);
    w8(0x4E, 0x02);
    w16(0x48, 0);
    w16(0x4A, 0x8000);
    for (int i = 0; i < 1000 && !(r16(0x4A) & 0x8000); i++) io_delay(1);
    w16(0x4A, 0);
    for (int i = 0; i < 1000 && (r16(0x4A) & 0x8000); i++) io_delay(1);
    w32(0x50, dma_phys + RIRB_OFF); w32(0x54, 0);
    w8(0x5E, 0x02);
    w16(0x58, 0x8000);
    w16(0x5A, 0xFF);
    rirb_rp = 0;
    w8(0x4C, 0x02);
    w8(0x5C, 0x02);

    uint16_t gcap = r16(0x00);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) return fail("no output streams");
    stream_index = iss;
    sd = 0x80 + iss * 0x20;
    const uint8_t tag = 1;
    const uint16_t fmt = 0x0011;                        /* 48 kHz, 16-bit, stereo */

    int routed = 0;
    for (int cad = 0; cad < 15; cad++)
        if (codecs & (1 << cad)) routed |= setup_codec(cad, tag, fmt, digital);
    if (!routed) return fail(0);

    /* stream reset */
    w8(sd, r8(sd) | 1);
    for (int i = 0; i < 1000 && !(r8(sd) & 1); i++) io_delay(1);
    w8(sd, r8(sd) & ~1);
    for (int i = 0; i < 1000 && (r8(sd) & 1); i++) io_delay(1);

    volatile uint32_t *bdl = (volatile uint32_t *)(dma + BDL_OFF);
    for (int i = 0; i < RING_BUFS; i++) {
        bdl[i * 4 + 0] = dma_phys + RING_OFF + i * CHUNK_FRAMES * 4;
        bdl[i * 4 + 1] = 0;
        bdl[i * 4 + 2] = CHUNK_FRAMES * 4;
        bdl[i * 4 + 3] = 1;                             /* IOC */
    }
    w32(sd + 0x18, dma_phys + BDL_OFF);
    w32(sd + 0x1C, 0);
    w32(sd + 0x08, RING_FRAMES * 4);
    w16(sd + 0x0C, RING_BUFS - 1);
    w16(sd + 0x12, fmt);
    w8(sd + 0x02, tag << 4);
    return 1;
}

/* Several controllers are common (onboard + graphics card HDMI). Unless
   told otherwise (want = controller number from 1, hdmi = digital outputs),
   the first one with an analog output wins, else the first HDMI/DP output
   with a monitor on it. */
int hda_init(int want, int hdmi)
{
    PciDev d;
    int found = 0;
    for (int pass = hdmi ? 1 : 0; pass < 2; pass++) {
        for (int i = 0; pci_find_class(0x04, 0x03, i, &d); i++) {
            found = i + 1;
            if (want && want != i + 1) continue;
            if (pass == (hdmi ? 1 : 0))
                jprintf("HDA: controller %d at %02X:%02X.%X\n", i + 1, d.bus, d.dev, d.fn);
            if (try_controller(d, pass)) {
                if (pass) jprintf("HDA: using controller %d (HDMI/DisplayPort)\n", i + 1);
                else if (found > 1 || pci_find_class(0x04, 0x03, i + 1, &d))
                    jprintf("HDA: using controller %d\n", i + 1);
                return 1;
            }
        }
    }
    if (!found) jprintf("HDA: no HD Audio controller found\n");
    else if (want && want > found) jprintf("HDA: there is no controller %d\n", want);
    else if (hdmi) jprintf("HDA: no HDMI/DisplayPort output with a monitor on it\n");
    else jprintf("HDA: no usable output (speakers, line out, headphones or HDMI monitor)\n");
    return 0;
}

/* ---------------------------------------------------------------- runtime */

uint32_t hda_underruns;                 /* the ring ran dry (debug) */
static int primed;

/* Idle stop: at the DOS prompt nothing plays, but a running stream keeps
   the machine (and a VM's emulated sound card) busy with ~375 interrupts a
   second. After a second of silence with no SB transfer the stream is
   paused; any access to the emulated ports (hda_wake) starts it again. */
#define IDLE_IRQS 375
#define MAX_RENDER 256          /* frames per interrupt at most; catches up over the next ones */
static int idle_irqs, paused_idle, block_silent;
static hda_busy_fn busy;

static void refill(void)
{
    uint32_t play = (r32(sd + 0x04) / 4) & RING_MASK;
    uint32_t ahead = (write_pos - play) & RING_MASK;
    if (ahead > TARGET_AHEAD * 2) {                     /* underrun or first run */
        if (primed) hda_underruns++;
        write_pos = (play + TARGET_AHEAD / 2) & RING_MASK;
        ahead = TARGET_AHEAD / 2;
    }
    primed = 1;
    int budget = MAX_RENDER;            /* bound the time spent with interrupts off */
    while (ahead < TARGET_AHEAD && budget > 0) {
        int n = TARGET_AHEAD - ahead;
        if (n > budget) n = budget;
        budget -= n;
        if (n > (int)(RING_FRAMES - write_pos)) n = RING_FRAMES - write_pos;
        render(&ring[write_pos * 2], n);
        {
            const uint32_t *p = (const uint32_t *)&ring[write_pos * 2];
            for (int i = 0; i < n && block_silent; i++)
                if (p[i]) block_silent = 0;
        }
        write_pos = (write_pos + n) & RING_MASK;
        ahead += n;
    }
}

/* Called from irq_thunk. Returns 0 if the interrupt wasn't ours (shared line). */
static inline uint64_t tsc(void) { uint64_t t; __asm__ volatile("rdtsc" : "=A"(t)); return t; }
static uint64_t t_last, t_busy, t_span, t_max, t_maxper;
static int t_n;

int hda_irq_service(void)
{
    if (!running || !(r8(sd + 0x03) & 0x04)) return 0;
    uint64_t t0 = dbg_on ? tsc() : 0;
    w8(sd + 0x03, 0x04);                                /* ack BCIS */
    block_silent = 1;
    refill();
    if (block_silent && !(busy && busy())) {
        if (++idle_irqs >= IDLE_IRQS) {
            w8(sd, 0x04);                               /* RUN off, IOCE on */
            paused_idle = 1;
            idle_irqs = 0;
        }
    } else idle_irqs = 0;
    sb_tick();
    if (dbg_on) {                       /* /D: time spent here, % of the IRQ period */
        uint64_t t1 = tsc();
        if (t_last) {
            uint64_t per = t0 - t_last, b = t1 - t0;
            t_busy += b; t_span += per;
            if (b > t_max) { t_max = b; t_maxper = per; }
            if (++t_n >= 375 && t_span) {
                dbg("HDA: IRQ work avg %u%% of the time, longest %u%% of a period\n",
                    (uint32_t)(t_busy * 100 / t_span),
                    (uint32_t)(t_maxper ? t_max * 100 / t_maxper : 0));
                t_busy = t_span = t_max = 0; t_n = 0;
            }
        }
        t_last = t0;
    }
    if (irq_line >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    return 1;
}

static uint32_t irq_ivt_addr(void)
{
    int vec = irq_line < 8 ? 0x08 + irq_line : 0x70 + irq_line - 8;
    return (uint32_t)vec * 4;
}

/* From the port trap: the game is doing something, make sure we play. */
void hda_wake(void)
{
    if (!paused_idle || !running) return;
    paused_idle = 0;
    idle_irqs = 0;
    uint32_t play = (r32(sd + 0x04) / 4) & RING_MASK;
    memset(ring, 0, RING_FRAMES * 4);                   /* old data would replay */
    write_pos = play;
    primed = 0;
    refill();
    w8(sd, 0x02 | 0x04);                                /* RUN + IOCE */
}

int hda_start(hda_render_fn fn, hda_busy_fn busy_fn)
{
    render = fn;
    busy = busy_fn;
    write_pos = 0;

    irq_callback = jlm_alloc_v86_callback(irq_thunk, 0);
    if (!irq_callback) return 0;
    irq_chain_vector = lin_peekl(irq_ivt_addr());
    lin_pokel(irq_ivt_addr(), irq_callback);

    refill();
    running = 1;
    w32(0x20, 0x80000000u | (1u << stream_index));     /* GIE + this stream */
    w8(sd, 0x02 | 0x04);                                /* run + IOCE */
    if (irq_line >= 8) {
        outb(0xA1, inb(0xA1) & ~(1 << (irq_line - 8)));
        outb(0x21, inb(0x21) & ~(1 << 2));              /* cascade */
    } else {
        outb(0x21, inb(0x21) & ~(1 << irq_line));
    }
    return 1;
}

void hda_stop(void)
{
    if (running) {
        running = 0;
        w8(sd, 0);
        w32(0x20, 0);
        lin_pokel(irq_ivt_addr(), irq_chain_vector);
        jlm_free_v86_callback(irq_callback);
    }
    free_dma_memory();
}
