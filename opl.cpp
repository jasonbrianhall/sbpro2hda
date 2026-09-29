/* SBPRO <-> dbopl glue. Two chips: 0 = the game's FM (AdLib / SB Pro OPL3),
   1 = SBPRO's own General MIDI synth behind the MPU-401. Each is constructed
   with placement new, since a JLM has no C++ runtime to run constructors. */
#include <stdint.h>
#include <stddef.h>
#include "dbopl.h"
#include "opl.h"

inline void *operator new(size_t, void *p) noexcept { return p; }

alignas(8) static unsigned char handler_mem[2][sizeof(DBOPL::Handler)];
static DBOPL::Handler *chips[2];
static Bit32s buf[2 * OPL_MAX_FRAMES];
extern "C" { int32_t opl_peak; }            /* largest |sample| since last read (debug) */

extern "C" void gm_tables_init(int opl_rate_x100);   /* gmsynth tables, uses the FPU */

extern "C" void opl_init(int rate)
{
    /* Table setup uses the FPU once; keep whatever state the machine had. */
    alignas(4) unsigned char fpu_state[108];
    __asm__ volatile("fnsave %0\n\tfninit" : "=m"(fpu_state));
    for (int i = 0; i < 2; i++) {
        chips[i] = new (handler_mem[i]) DBOPL::Handler();
        chips[i]->Init((Bitu)rate);
    }
    gm_tables_init(4971591);                /* 14318180 / 288 Hz, x100 */
    __asm__ volatile("frstor %0" : : "m"(fpu_state));
}

extern "C" void opl_write(uint32_t reg, uint8_t val)
{
    if (chips[0]) chips[0]->WriteReg(reg, val);
}

extern "C" void midi_opl_write(uint32_t reg, uint8_t val)
{
    if (chips[1]) chips[1]->WriteReg(reg, val);
}

static void mix_chip(DBOPL::Handler *h, int32_t *out, int frames)
{
    h->Generate(buf, (Bitu)frames);
    int stereo = h->chip.opl3Active;
    int n = stereo ? frames * 2 : frames;
    for (int i = 0; i < n; i++) {
        int32_t v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > opl_peak) opl_peak = v;
    }
    if (stereo) {
        for (int i = 0; i < frames * 2; i++) out[i] += buf[i];
    } else {
        for (int i = 0; i < frames; i++) {
            out[i * 2] += buf[i];
            out[i * 2 + 1] += buf[i];
        }
    }
}

/* Adds both chips' output to 'out' (interleaved stereo). */
extern "C" void opl_mix(int32_t *out, int frames)
{
    if (frames > OPL_MAX_FRAMES) frames = OPL_MAX_FRAMES;
    for (int i = 0; i < 2; i++)
        if (chips[i]) mix_chip(chips[i], out, frames);
}
