/* SBPRO - resident Sound Blaster Pro 2.0 emulator over Intel HD Audio.
 *
 * Needs: JEMM386/JEMMEX with QPIEMU.DLL loaded (or QEMM), and HDPMI32i
 * loaded resident (HDPMI32i -r) so this client's memory survives exit.
 *
 * Usage: SBPRO [Axxx] [In] [Dn] [/T]
 *   /T  play a test tone instead of emulated output
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dpmi.h>
#include <go32.h>
#include <crt0.h>
#include <sys/farptr.h>
#include "hda.h"
#include "qpi.h"
#include "dsp.h"

int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY;     /* everything touched at IRQ time */

static uint16_t sb_base = 0x220;
static int sb_irq = 5, sb_dma = 1, test_tone;

/* ---------------------------------------------------------------- output */

static uint32_t tone_phase;

static void render_tone(int16_t *out, int frames)
{
    for (int i = 0; i < frames; i++) {
        tone_phase += (440u << 16) / HDA_RATE * 256;  /* 440 Hz, 24.8 fixed */
        int32_t p = (tone_phase >> 8) & 0xFFFF;       /* 0..65535 */
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

static _go32_dpmi_seginfo trap_cb;
static _go32_dpmi_registers trap_regs;

static void io_trap(_go32_dpmi_registers *r)
{
    uint16_t port = r->x.dx;
    if (r->h.cl & QPI_IO_OUT) {
        dsp_out(port, r->h.al);
    } else {
        uint8_t v = dsp_in(port);
        if (r->h.cl & QPI_IO_WORD) r->x.ax = v | 0xFF00;
        else r->h.al = v;
    }
}

static int install_traps(void)
{
    trap_cb.pm_offset = (unsigned long)io_trap;
    if (_go32_dpmi_allocate_real_mode_callback_retf(&trap_cb, &trap_regs) != 0) return 0;
    if (!qpi_set_io_callback(trap_cb.rm_segment, trap_cb.rm_offset)) return 0;
    for (uint16_t p = sb_base; p < sb_base + 0x10; p++)
        if (dsp_owns(p) && !qpi_trap_port(p)) return 0;
    return 1;
}

/* ---------------------------------------------------------------- resident */

static void go_resident(void)
{
    unsigned long psp = _go32_info_block.linear_address_of_original_psp;
    uint16_t paras = _farpeekw(_dos_ds, psp - 16 + 3);  /* size from the PSP's MCB */
    fflush(stdout);
    /* Issued from protected mode so HDPMI keeps this client alive. */
    __asm__ __volatile__("int $0x21" : : "a"(0x3100), "d"(paras));
}

/* ---------------------------------------------------------------- main */

static void parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        switch (toupper((unsigned char)a[0])) {
        case 'A': sb_base = (uint16_t)strtoul(a + 1, NULL, 16); break;
        case 'I': sb_irq = atoi(a + 1); break;
        case 'D': sb_dma = atoi(a + 1); break;
        case '/': case '-':
            if (toupper((unsigned char)a[1]) == 'T') test_tone = 1;
            break;
        }
    }
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);
    printf("SBPRO: Sound Blaster Pro 2.0 emulation over HD Audio\n");

    if (!qpi_detect()) {
        printf("QPI not found. Load JEMM386/JEMMEX and JLOAD QPIEMU.DLL.\n");
        return 1;
    }
    printf("QPI version %x.%02x\n", qpi_version() >> 8, qpi_version() & 0xFF);

    if (!hda_init()) return 1;
    dsp_init(sb_base);
    if (!install_traps()) {
        printf("Could not trap ports at %Xh\n", sb_base);
        return 1;
    }
    if (!hda_start(test_tone ? render_tone : render_sb)) {
        printf("Could not install HDA IRQ %d handler\n", hda_irq());
        return 1;
    }

    printf("HDA IRQ %d. Emulating A%X I%d D%d%s\n", hda_irq(), sb_base, sb_irq, sb_dma,
           test_tone ? " (test tone)" : "");
    printf("SET BLASTER=A%X I%d D%d T4\n", sb_base, sb_irq, sb_dma);
    go_resident();
    return 0;
}
