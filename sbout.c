/* Sound Blaster Pro playback engine.
 *
 * DMA: Jemm passes the game's 8237 programming through to the (emulated or
 * real) DMA controller. Nothing ever requests DMA on the Sound Blaster's
 * channel, so its address/count registers keep what the game wrote, and we
 * read them back when a transfer starts. If Jemm had to redirect the
 * transfer to its own DMA buffer, the controller holds that buffer's
 * address instead, which is exactly where the data is.
 *
 * Playback runs inside the HDA interrupt: every rendered 48 kHz frame
 * advances the SB stream by rate/48000 samples. When a DSP block finishes,
 * the virtual IRQ is flagged and injected as the HDA interrupt returns.
 */
#include <string.h>
#include "io.h"
#include "jlm.h"
#include "dsp.h"
#include "hda.h"
#include "sbout.h"
#include "pic.h"
#include "opl.h"

#define WINDOW_PAGES 17                     /* 64 KB + one page of slack */

static int sb_irq = 5, sb_dma = 1;
static volatile int irq_pending;

static struct {
    int active, autoinit, silence;
    uint32_t dma_phys, dma_len, dma_pos;
    const volatile uint8_t *mem;
    uint32_t block_len, block_left;
    uint32_t step, frac;
    int stereo;
    int16_t prev_l, prev_r, cur_l, cur_r;
} s;

/* Emulated view of the SB channel's 8237 registers, for games that poll
   the DMA count or the status register instead of waiting for the IRQ. */
static int dma_valid, dma_tc, dma_done, soft_ff;

static uint32_t window_lin;                 /* for buffers outside conventional memory */
static int window_committed;

static const uint8_t page_port[4] = { 0x87, 0x83, 0x81, 0x82 };

void sb_out_init(int irq, int dma)
{
    sb_irq = irq;
    sb_dma = dma & 3;
    memset(&s, 0, sizeof s);
}

int sb_out_map_init(void)
{
    window_lin = jlm_page_reserve(PR_SYSTEM, WINDOW_PAGES, 0);
    if (window_lin == 0xFFFFFFFFu || window_lin == 0) { window_lin = 0; return 0; }
    return 1;
}

/* ---------------------------------------------------------------- DMA */

static void read_dma_controller(void)
{
    uint16_t aport = sb_dma * 2, cport = sb_dma * 2 + 1;
    uint32_t lo, hi;

    outb(0x0C, 0);                          /* clear flip-flop */
    lo = inb(aport); hi = inb(aport);
    uint32_t addr = hi << 8 | lo;
    outb(0x0C, 0);
    lo = inb(cport); hi = inb(cport);
    uint32_t count = hi << 8 | lo;
    outb(0x0C, 0);
    uint32_t page = inb(page_port[sb_dma]);

    s.dma_phys = page << 16 | addr;
    s.dma_len = count + 1;
    s.dma_pos = 0;
    dma_valid = 1;
    dma_done = 0;
    dma_tc = 0;
}

static int map_dma_buffer(void)
{
    if (s.dma_phys + s.dma_len <= 0xA0000) {             /* conventional memory: 1:1 */
        s.mem = (const volatile uint8_t *)s.dma_phys;
        return 1;
    }
    if (!window_lin) return 0;
    uint32_t first = s.dma_phys >> 12;
    uint32_t pages = ((s.dma_phys & 0xFFF) + s.dma_len + 4095) >> 12;
    if (pages > WINDOW_PAGES) pages = WINDOW_PAGES;
    if (window_committed) jlm_page_decommit(window_lin >> 12, WINDOW_PAGES, 0);
    window_committed = jlm_page_commit_phys(window_lin >> 12, pages, first, PC_INCR | PC_WRITEABLE) != 0;
    if (!window_committed) return 0;
    s.mem = (const volatile uint8_t *)(window_lin + (s.dma_phys & 0xFFF));
    return 1;
}

/* ---------------------------------------------------------------- control */

void sb_out_start(int autoinit, uint32_t len_bytes, int silence)
{
    uint32_t tc = dsp.time_constant;
    uint32_t rate = 1000000u / (256 - tc);

    s.stereo = (dsp.mixer[0x0E] & 0x02) && !silence;
    if (s.stereo) rate /= 2;                /* TC was set for twice the frame rate */
    if (rate < 1000) rate = 1000;
    if (rate > 48000) rate = 48000;
    s.step = (rate << 16) / HDA_RATE;
    s.frac = 0;

    s.silence = silence;
    dsp.paused = 0;                         /* a new transfer ends a D0h pause */
    s.autoinit = autoinit;
    s.block_len = len_bytes ? len_bytes : 1;
    s.block_left = s.block_len;

    if (!silence) {
        read_dma_controller();
        if (!map_dma_buffer()) {
            dbg("SB: DMA buffer %X not mappable\n", s.dma_phys);
            s.active = 0;
            return;
        }
    }
    dbg("SB: start %s len=%u rate=%u%s dma=%X/%u", autoinit ? "auto" : "single",
        s.block_len, rate, s.stereo ? " stereo" : "", s.dma_phys, s.dma_len);
    if (!silence && s.dma_len >= 4)
        dbg(" data %2X %2X %2X %2X", s.mem[0], s.mem[1], s.mem[2], s.mem[3]);
    dbg("\n");

    /* Detection transfers (a few bytes) finish in microseconds on a real
       card; complete them now so the IRQ arrives before any timeout. */
    if (!autoinit && s.block_len <= 64) {
        if (!silence) {
            s.dma_pos = s.block_len % (s.dma_len ? s.dma_len : 1);
            if (s.dma_pos == 0) { dma_done = 1; dma_tc = 1; }
        }
        s.active = 0;
        irq_pending = 1;
        return;
    }
    s.active = 1;
}

void sb_out_stop(void)            { s.active = 0; irq_pending = 0; }
void sb_out_exit_autoinit(void)   { s.autoinit = 0; }
void sb_out_raise_irq(void)       { irq_pending = 1; dbg("SB: F2h IRQ request\n"); }

/* ---------------------------------------------------------------- render */

static uint8_t fetch(void)
{
    uint8_t b = 0x80;
    if (!s.silence) {
        b = s.mem[s.dma_pos];
        if (++s.dma_pos >= s.dma_len) {                  /* 8237 terminal count */
            s.dma_pos = 0;
            dma_tc = 1;
            if (!s.autoinit) dma_done = 1;
        }
    }
    if (--s.block_left == 0) {
        irq_pending = 1;
        if (s.autoinit) s.block_left = s.block_len;
        else s.active = 0;
    }
    return b;
}

static void next_frame(void)
{
    s.prev_l = s.cur_l;
    s.prev_r = s.cur_r;
    int16_t l = (int16_t)(((int)fetch() - 128) * 256);
    int16_t r = l;
    if (s.stereo && s.active) r = (int16_t)(((int)fetch() - 128) * 256);
    s.cur_l = l;
    s.cur_r = r;
}

static int32_t mix[2 * OPL_MAX_FRAMES];

/* Soft limiter: linear up to 3/4 of full scale, then 4:1 until the rail,
   so loud moments (music + effects together) squash instead of crackle. */
static inline int16_t limit(int32_t v)
{
    const int32_t knee = 24576;
    if (v > knee) { v = knee + (v - knee) / 4; if (v > 32767) v = 32767; }
    else if (v < -knee) { v = -knee + (v + knee) / 4; if (v < -32768) v = -32768; }
    return (int16_t)v;
}

static void render_chunk(int16_t *out, int frames)
{
    for (int i = 0; i < frames; i++) {
        int32_t l, r;
        if (s.active && !dsp.paused) {
            s.frac += s.step;
            while (s.frac >= 0x10000 && s.active) {
                s.frac -= 0x10000;
                next_frame();
            }
            int32_t f = s.frac;                          /* linear interpolation */
            l = s.prev_l + (((s.cur_l - s.prev_l) * f) >> 16);
            r = s.prev_r + (((s.cur_r - s.prev_r) * f) >> 16);
        } else {
            l = r = ((int32_t)dsp.dac_value - 128) * 256;  /* direct DAC (10h) */
        }
        if (!dsp.speaker) l = r = 0;
        mix[i * 2] = l;
        mix[i * 2 + 1] = r;
    }
    opl_mix(mix, frames);

    /* /D: report the FM level twice a second while it's playing */
    static uint32_t since, last_underruns;
    since += frames;
    if (since >= HDA_RATE / 2) {
        if (opl_peak) dbg("FM: peak %d\n", opl_peak);
        if (hda_underruns != last_underruns) {
            dbg("HDA: %u underruns\n", hda_underruns);
            last_underruns = hda_underruns;
        }
        opl_peak = 0;
        since = 0;
    }
    for (int i = 0; i < frames * 2; i++)
        out[i] = limit(mix[i]);
}

void sb_render(int16_t *out, int frames)
{
    while (frames > 0) {
        int n = frames > OPL_MAX_FRAMES ? OPL_MAX_FRAMES : frames;
        render_chunk(out, n);
        out += n * 2;
        frames -= n;
    }
}

/* ---------------------------------------------------------------- 8237 */

int sb_dma_owns(uint16_t port)
{
    return port == sb_dma * 2 || port == sb_dma * 2 + 1 || port == 0x08;
}

/* Writes go to the real controller unchanged. Reprogramming the address or
   count makes our emulated view stale until the next DSP start. */
void sb_dma_out(uint16_t port, uint8_t v)
{
    outb(port, v);
    if (port != 0x08) dma_valid = 0;
}

uint8_t sb_dma_in(uint16_t port)
{
    uint8_t hw = inb(port);                 /* also toggles the real flip-flop */
    if (!dma_valid) return hw;

    if (port == 0x08) {
        uint8_t v = hw & ~(0x11 << sb_dma);
        if (dma_tc) v |= 1 << sb_dma;
        if (s.active && !s.silence) v |= 0x10 << sb_dma;
        dma_tc = 0;
        return v;
    }

    /* The real registers still hold what the game programmed (nothing ever
       requests DMA on this channel). Comparing the byte we just read with
       that value tells which half the game's flip-flop selected. */
    uint32_t base, cur;
    if (port == sb_dma * 2) {
        base = s.dma_phys & 0xFFFF;
        cur = dma_done ? base + s.dma_len : base + s.dma_pos;
    } else {
        base = s.dma_len - 1;
        cur = dma_done ? 0xFFFF : s.dma_len - 1 - s.dma_pos;
    }
    uint8_t lo = base & 0xFF, hi = (base >> 8) & 0xFF;
    int high;
    if (lo != hi) high = (hw == hi);
    else high = soft_ff;
    soft_ff = !high;
    return high ? (uint8_t)(cur >> 8) : (uint8_t)cur;
}

/* ---------------------------------------------------------------- IRQ */

/* The SB interrupt is delivered with an extra IRET frame on the game's
   stack, so its handler returns through sbret_thunk first: SBPRO then knows
   exactly when the handler has finished and never starts it a second time
   while it is still running (handlers that EOI early and re-enable
   interrupts would otherwise be re-entered, which they don't expect). */
uint32_t sb_ret_callback;           /* seg:off of the V86 callback, 0 = none */
static int isr_active, isr_age;
#define ISR_TIMEOUT 200             /* HDA interrupts (~0.6 s): handler never returned */

void sb_inject(Client *c, int vec)
{
    jlm_simulate_int(c, vec);
    if (!sb_ret_callback) return;
    uint32_t sp = (c->ESP - 6) & 0xFFFF;
    uint32_t lin = ((c->SS & 0xFFFF) << 4) + sp;
    *(volatile uint16_t *)lin = (uint16_t)sb_ret_callback;
    *(volatile uint16_t *)(lin + 2) = (uint16_t)(sb_ret_callback >> 16);
    *(volatile uint16_t *)(lin + 4) = (uint16_t)c->EFlags;
    c->ESP = (c->ESP & 0xFFFF0000u) | sp;
    isr_active = 1;
    isr_age = 0;
}

void sb_isr_done(void)
{
    isr_active = 0;
}

void sb_tick(void)
{
    if (isr_active && ++isr_age > ISR_TIMEOUT) isr_active = 0;
}

/* Set by SBPM (protected mode): a byte in DOS memory that its CLI handler
   sets whenever the game opens a "pushf / cli ... popf" critical section.
   Under HDPMI32i such a CLI can't be honored (the POPF couldn't undo it),
   so SBPM leaves interrupts on; holding the SB IRQ back for one HD Audio
   period keeps the game's SB handler from running inside that section. */
uint32_t sb_hold_addr;
static int hold_streak;

int sb_pending_vector(void)
{
    if (!irq_pending) return 0;
    if (pic_masked(sb_irq)) return 0;                     /* masked: keep it pending */
    if (pic_virtual_busy(sb_irq)) return 0;               /* previous one not EOIed yet */
    if (isr_active) return 0;                             /* handler still running */
    if (sb_hold_addr && *(volatile uint8_t *)sb_hold_addr) {
        *(volatile uint8_t *)sb_hold_addr = 0;
        if (++hold_streak <= 4) return 0;   /* don't starve the game either */
    }
    hold_streak = 0;
    int vec = sb_irq < 8 ? 0x08 + sb_irq : 0x70 + sb_irq - 8;
    irq_pending = 0;
    pic_virtual_start(sb_irq);
    dbg("SB: IRQ %d -> int %X\n", sb_irq, vec);
    return vec;
}
