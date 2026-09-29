/* SBPRO.DLL - Sound Blaster Pro 2.0 emulation over Intel HD Audio.
 *
 * A Jemm Loadable Module: runs inside JEMM386/JEMMEX at ring 0, traps the
 * Sound Blaster ports itself and stays resident. Nothing else is needed.
 *
 *   JLOAD SBPRO.DLL [Axxx] [In] [Dn] [/T]      load
 *   JLOAD -u SBPRO.DLL                          unload
 *
 *   /T  play a test tone instead of emulated output
 *   /D  log port traffic to COM1, 115200 8N1 (QEMU: -serial file:sbpro.log)
 */
#include <stdint.h>
#include <string.h>
#include "jlm.h"
#include "hda.h"
#include "dsp.h"
#include "sbout.h"
#include "pic.h"

#define SBPRO_DEVICE_ID 0x7B50

__attribute__((dllexport)) DDB ddb = {
    .Req_Device_Number = SBPRO_DEVICE_ID,
    .Dev_Major_Version = 0,
    .Dev_Minor_Version = 9,
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

/* ---------------------------------------------------------------- traps */

/* Log a port access; identical repeats (status polling) are counted. */
static uint32_t last_port = 0xFFFFFFFF, last_val, last_out, repeats;

static void log_io(uint32_t port, uint32_t v, int out)
{
    if (!dbg_on) return;
    if (port == last_port && v == last_val && out == last_out) {
        if (++repeats == 10000) { dbg("  (x10000 so far)\n"); repeats = 0; }
        return;
    }
    if (repeats) dbg("  (x%u)\n", repeats);
    repeats = 0;
    last_port = port; last_val = v; last_out = out;
    dbg(out ? "out %X,%2X\n" : "in  %X=%2X\n", port, v);
}

uint32_t sb_io(uint32_t data, uint32_t port, uint32_t type)
{
    uint16_t p = (uint16_t)port;
    if (type & IO_OUTPUT) {
        if (!pic_owns(p) || (p & 1)) log_io(port, data & 0xFF, 1);   /* skip EOIs */
        if (pic_owns(p)) pic_out(p, (uint8_t)data);
        else if (sb_dma_owns(p)) sb_dma_out(p, (uint8_t)data);
        else dsp_out(p, (uint8_t)data);
        return data;
    }
    uint8_t v = pic_owns(p) ? pic_in(p) : sb_dma_owns(p) ? sb_dma_in(p) : dsp_in(p);
    if (!pic_owns(p)) log_io(port, v, 0);
    if (type & (IO_WORD | IO_DWORD)) return (data & 0xFFFF0000u) | 0xFF00u | v;
    return (data & 0xFFFFFF00u) | v;
}

/* Every port we emulate: the SB block plus the AdLib ports. */
static int port_list(uint16_t *out)
{
    int n = 0;
    for (uint16_t p = sb_base; p < sb_base + 0x10; p++)
        if (dsp_owns(p)) out[n++] = p;
    for (uint16_t p = 0x388; p < 0x38C; p++)
        if (dsp_owns(p)) out[n++] = p;
    return n;
}

/* 8237 ports for the SB channel. Optional: JEMM before 5.84 keeps the DMA
   ports to itself, and then games that poll the DMA count won't work. */
static const uint16_t dma_ports_template[3] = { 0, 1, 0x08 };
static uint16_t dma_ports[3];
static int dma_trapped;

static const uint16_t pic_ports[4] = { 0x20, 0x21, 0xA0, 0xA1 };
static int pic_trapped;

static void untrap_ports(void)
{
    if (pic_trapped) {
        for (int i = 0; i < 4; i++) jlm_remove_io(pic_ports[i]);
        pic_restore();
        pic_trapped = 0;
    }
    uint16_t ports[24];
    int n = port_list(ports);
    if (ports_trapped)
        for (int i = 0; i < n; i++) jlm_remove_io(ports[i]);
    ports_trapped = 0;
    for (int i = 0; i < dma_trapped; i++) jlm_remove_io(dma_ports[i]);
    dma_trapped = 0;
}

static int trap_ports(void)
{
    uint16_t ports[24];
    int n = port_list(ports);
    for (int i = 0; i < n; i++) {
        if (!jlm_install_io(ports[i], io_thunk)) {
            jprintf("SBPRO: port %X is already trapped by another driver\n", ports[i]);
            while (i--) jlm_remove_io(ports[i]);
            return 0;
        }
    }
    ports_trapped = 1;

    pic_init(hda_irq());
    for (pic_trapped = 0; pic_trapped < 4; pic_trapped++)
        if (!jlm_install_io(pic_ports[pic_trapped], io_thunk)) break;
    if (pic_trapped < 4) {
        for (int i = 0; i < pic_trapped; i++) jlm_remove_io(pic_ports[i]);
        pic_trapped = 0;
        jprintf("SBPRO: warning, can't watch the interrupt controller;\n"
                "       games that rewrite the IRQ mask will stop the sound\n");
    }

    /* Before 5.84, JLOAD keeps Jemm's own DMA handlers in its trap table
       and removing a handler there can leave a dangling entry: don't try. */
    uint32_t ver = jlm_version();
    uint32_t major = ver & 0xFFFF, minor = ver >> 16;
    if (major < 5 || (major == 5 && minor < 84)) {
        jprintf("SBPRO: JEMM %u.%u is older than 5.84; DMA counter emulation is off.\n"
                "       Games that poll the DMA counter may hang.\n", major, minor);
        return 1;
    }

    dma_ports[0] = sb_dma * 2 + dma_ports_template[0];
    dma_ports[1] = sb_dma * 2 + dma_ports_template[1];
    dma_ports[2] = dma_ports_template[2];
    for (dma_trapped = 0; dma_trapped < 3; dma_trapped++)
        if (!jlm_install_io(dma_ports[dma_trapped], io_thunk)) break;
    if (dma_trapped < 3) {
        for (int i = 0; i < dma_trapped; i++) jlm_remove_io(dma_ports[i]);
        dma_trapped = 0;
        jprintf("SBPRO: warning, JEMM won't share the DMA ports;\n"
                "       games that poll the DMA counter may hang\n");
    }
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
            else if ((*s | 0x20) == 'd') { dbg_init(); s++; }
            break;
        }
    }
}

static int load(JLCOMM *jc)
{
    parse_args((const char *)jc->lpCmdLine);
    jprintf("SBPRO 0.9: Sound Blaster Pro 2.0 emulation over HD Audio (JEMM %u.%u)\n",
            jlm_version() & 0xFFFF, jlm_version() >> 16);

    if (!hda_init()) return 0;
    if (hda_irq() == sb_irq) {
        jprintf("SBPRO: HD Audio uses IRQ %d; pick another SB IRQ (e.g. I7)\n", sb_irq);
        hda_stop();
        return 0;
    }
    sb_out_init(sb_irq, sb_dma);
    if (!sb_out_map_init()) jprintf("SBPRO: warning, DMA buffers above 640K won't play\n");
    dsp_init(sb_base);
    if (!trap_ports()) { hda_stop(); return 0; }
    if (!hda_start(test_tone ? render_tone : sb_render)) {
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
