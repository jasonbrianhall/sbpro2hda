/* Sound Blaster Pro 2.0 DSP and mixer.
 *
 *  base+4  mixer index      base+5  mixer data
 *  base+6  DSP reset        base+A  DSP read data
 *  base+C  DSP write / write-buffer status
 *  base+E  DSP read-buffer status (also acknowledges the 8-bit IRQ)
 */
#include <string.h>
#include "dsp.h"

DspState dsp;
void (*dsp_irq_request)(void);

static uint16_t base;
static uint8_t outq[16];
static uint8_t qhead, qtail;
static uint8_t cmd, nargs, argi, args[2];
static uint8_t reset_latch, test_reg, mix_index;

static void q_clear(void) { qhead = qtail = 0; }
static void q_push(uint8_t v)
{
    uint8_t next = (qtail + 1) & 15;
    if (next != qhead) { outq[qtail] = v; qtail = next; }
}
static int q_empty(void) { return qhead == qtail; }
static uint8_t q_pop(void)
{
    static uint8_t last = 0xFF;
    if (!q_empty()) { last = outq[qhead]; qhead = (qhead + 1) & 15; }
    return last;
}

static void mixer_reset(void)
{
    memset(dsp.mixer, 0, sizeof dsp.mixer);
    dsp.mixer[0x04] = 0x99;     /* voice */
    dsp.mixer[0x22] = 0x99;     /* master */
    dsp.mixer[0x26] = 0x99;     /* FM */
    dsp.mixer[0x0E] = 0x00;     /* mono, filter on */
}

static void dsp_reset(void)
{
    q_clear();
    nargs = argi = 0;
    dsp.pending = DSP_STOP_AUTO;
    dsp.paused = 0;
    dsp.speaker = 0;
    q_push(0xAA);
}

static int arg_count(uint8_t c)
{
    switch (c) {
    case 0x10: case 0x40: case 0xE0: case 0xE2: case 0xE4:
        return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x48:
    case 0x74: case 0x75: case 0x76: case 0x77: case 0x80:
        return 2;
    default:
        return 0;
    }
}

static void exec(void)
{
    uint16_t w = args[0] | (args[1] << 8);
    switch (cmd) {
    case 0x10: dsp.dac_value = args[0]; break;
    case 0x14: case 0x91: /* 91h: high-speed single-cycle, uses 48h length */
        dsp.sc_len = cmd == 0x14 ? w : dsp.block_len;
        dsp.pending = DSP_START_SINGLE;
        break;
    case 0x1C: case 0x90:
        dsp.pending = DSP_START_AUTO;
        break;
    case 0x40: dsp.time_constant = args[0]; break;
    case 0x48: dsp.block_len = w; break;
    case 0x80: break;           /* silence block: needs IRQ timing, step 3 */
    case 0xD0: dsp.paused = 1; break;
    case 0xD4: dsp.paused = 0; break;
    case 0xD1: dsp.speaker = 1; break;
    case 0xD3: dsp.speaker = 0; break;
    case 0xD8: q_push(dsp.speaker ? 0xFF : 0x00); break;
    case 0xDA: dsp.pending = DSP_STOP_AUTO; break;
    case 0xE0: q_push((uint8_t)~args[0]); break;
    case 0xE1: q_push(0x03); q_push(0x02); break;
    case 0xE4: test_reg = args[0]; break;
    case 0xE8: q_push(test_reg); break;
    case 0xF2: case 0xF3:
        if (dsp_irq_request) dsp_irq_request();
        break;
    case 0xF8: q_push(0x00); break;
    default: break;             /* ADPCM, MIDI etc.: accepted and ignored */
    }
}

void dsp_init(uint16_t b)
{
    base = b;
    mixer_reset();
    dsp.time_constant = 0xA6;   /* ~11 kHz */
    dsp.block_len = 0x7FF;
    dsp_reset();
    q_clear();
}

int dsp_owns(uint16_t port)
{
    switch (port - base) {
    case 0x4: case 0x5: case 0x6: case 0xA: case 0xC: case 0xE: return 1;
    default: return 0;
    }
}

uint8_t dsp_in(uint16_t port)
{
    switch (port - base) {
    case 0x5: return dsp.mixer[mix_index];
    case 0xA: return q_pop();
    case 0xC: return 0x7F;                              /* bit 7 clear: ready */
    case 0xE: return q_empty() ? 0x7F : 0xFF;           /* bit 7: data available */
    default:  return 0xFF;
    }
}

void dsp_out(uint16_t port, uint8_t v)
{
    switch (port - base) {
    case 0x4:
        mix_index = v;
        break;
    case 0x5:
        if (mix_index == 0x00) mixer_reset();
        else dsp.mixer[mix_index] = v;
        break;
    case 0x6:
        if (v & 1) reset_latch = 1;
        else if (reset_latch) { reset_latch = 0; dsp_reset(); }
        break;
    case 0xC:
        if (nargs) {
            args[argi++] = v;
            if (argi >= nargs) { exec(); nargs = 0; }
        } else {
            cmd = v;
            argi = 0;
            nargs = arg_count(v);
            if (!nargs) exec();
        }
        break;
    }
}
