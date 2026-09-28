/* SBPRO.DLL - Sound Blaster Pro 2.0 emulation over Intel HD Audio.
 *
 * A Jemm Loadable Module: runs inside JEMM386/JEMMEX at ring 0, traps the
 * Sound Blaster ports itself and stays resident. Nothing else is needed.
 *
 *   JLOAD SBPRO.DLL [Axxx] [In] [Dn] [/T]      load
 *   JLOAD -u SBPRO.DLL                          unload
 *
 *   /T  play a test tone instead of emulated output
 */
#include <stdint.h>
#include <string.h>
#include "jlm.h"
#include "hda.h"
#include "dsp.h"

#define SBPRO_DEVICE_ID 0x7B50

__attribute__((dllexport)) DDB ddb = {
    .Req_Device_Number = SBPRO_DEVICE_ID,
    .Dev_Major_Version = 0,
    .Dev_Minor_Version = 3,
    .Name = { 'S', 'B', 'P', 'R', 'O', ' ', ' ', ' ' },
    .Init_Order = 0x80000000u,
    .Size = sizeof(DDB),
};

static uint16_t sb_base = 0x220;
static int sb_irq = 5, sb_dma = 1, test_tone;
static int ports_trapped;

/* ---------------------------------------------------------------- output */

static uint32_t tone_phase;

static void render_tone(int16_t *out, int frames)
{
    for (int i = 0; i < frames; i++) {
        tone_phase += (440u << 16) / HDA_RATE * 256;  /* 440 Hz, 24.8 fixed */
        int32_t p = (tone_phase >> 8) & 0xFFFF;
        int32_t tri = p < 32768 ? p * 2 - 32768 : 98303 - p * 2;
        int16_t s = (int16_t)(tri / 8);
        out[i * 2] = out[i * 2 + 1] = s;
    }
}

static void render_sb(int16_t *out, int frames)
{
    /* Step 3 replaces this with the DMA-fed DSP output plus OPL3. */
    memset(out, 0, frames * 4);
}

/* ---------------------------------------------------------------- traps */

uint32_t sb_io(uint32_t data, uint32_t port, uint32_t type)
{
    if (type & IO_OUTPUT) {
        dsp_out((uint16_t)port, (uint8_t)data);
        return data;
    }
    uint8_t v = dsp_in((uint16_t)port);
    if (type & (IO_WORD | IO_DWORD)) return (data & 0xFFFF0000u) | 0xFF00u | v;
    return (data & 0xFFFFFF00u) | v;
}

static void untrap_ports(void)
{
    for (uint16_t p = sb_base; p < sb_base + 0x10; p++)
        if (ports_trapped && dsp_owns(p)) jlm_remove_io(p);
    ports_trapped = 0;
}

static int trap_ports(void)
{
    for (uint16_t p = sb_base; p < sb_base + 0x10; p++) {
        if (!dsp_owns(p)) continue;
        if (!jlm_install_io(p, io_thunk)) {
            jprintf("SBPRO: port %X is already trapped by another driver\n", p);
            for (uint16_t q = sb_base; q < p; q++)
                if (dsp_owns(q)) jlm_remove_io(q);
            return 0;
        }
    }
    ports_trapped = 1;
    return 1;
}

/* ---------------------------------------------------------------- load */

static uint32_t hex(const char **s)
{
    uint32_t v = 0;
    for (;; (*s)++) {
        char c = **s;
        if (c >= '0' && c <= '9') v = v * 16 + c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') v = v * 16 + (c | 0x20) - 'a' + 10;
        else return v;
    }
}

static uint32_t dec(const char **s)
{
    uint32_t v = 0;
    while (**s >= '0' && **s <= '9') v = v * 10 + *(*s)++ - '0';
    return v;
}

static void parse_args(const char *s)
{
    while (*s && *s != '\r' && *s != '\n') {
        char c = *s++;
        switch (c | 0x20) {
        case 'a': sb_base = (uint16_t)hex(&s); break;
        case 'i': sb_irq = dec(&s); break;
        case 'd': sb_dma = dec(&s); break;
        case '/': case '-':
            if ((*s | 0x20) == 't') { test_tone = 1; s++; }
            break;
        }
    }
}

static int load(JLCOMM *jc)
{
    parse_args((const char *)jc->lpCmdLine);
    jprintf("SBPRO: Sound Blaster Pro 2.0 emulation over HD Audio\n");

    if (!hda_init()) return 0;
    dsp_init(sb_base);
    if (!trap_ports()) { hda_stop(); return 0; }
    if (!hda_start(test_tone ? render_tone : render_sb)) {
        jprintf("SBPRO: no V86 callback left for the HDA IRQ\n");
        untrap_ports();
        hda_stop();
        return 0;
    }

    jprintf("SBPRO: HDA IRQ %d. Emulating A%X I%d D%d%s\n", hda_irq(), sb_base, sb_irq, sb_dma,
            test_tone ? " (test tone)" : "");
    jprintf("SET BLASTER=A%X I%d D%d T4\n", sb_base, sb_irq, sb_dma);
    return 1;
}

static int unload(void)
{
    untrap_ports();
    hda_stop();
    jprintf("SBPRO: unloaded\n");
    return 1;
}

int __stdcall DllMain(void *module, uint32_t reason, JLCOMM *jc)
{
    (void)module;
    Client *c = jlm_client();
    Client saved = *c;                  /* nested DOS/XMS calls change client regs */
    int ok = 0;

    if (reason == 1) ok = load(jc);
    else if (reason == 0) ok = unload();

    *c = saved;
    return ok;
}
