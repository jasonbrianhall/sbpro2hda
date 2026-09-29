/* SBPM - protected-mode side of SBPRO, for DOS/4GW games (Doom, Warcraft 2).
 *
 * Needs HDPMI32i loaded resident (HDPMI32i -r) and SBPRO.DLL loaded.
 *
 *   SBPM [program [args]]      no program = start a DOS shell
 *
 * HDPMI32i runs protected-mode programs with IOPL 0 and lets a client trap
 * port ranges (HDPMI API function 6). SBPM traps the Sound Blaster, AdLib,
 * MPU-401, PIC and SB-DMA ports, then runs the game as a nested client.
 * Each trapped access is re-issued from a tiny real-mode stub, where JEMM
 * traps it again and hands it to SBPRO - so all emulation stays in SBPRO.
 * The chatty status polls (AdLib status, DSP busy) are answered here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <dpmi.h>
#include <go32.h>
#include <crt0.h>
#include <sys/movedata.h>
#include "sbpm.h"

int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY;

static uint16_t sb_base = 0x220, mpu_base = 0x330;
static int sb_dma = 1;
static uint16_t stub_seg;
static int dbg;

/* ------------------------------------------------------------ real mode */

/* Real-mode stubs (one paragraph):
     +0  EC           in  al, dx
     +1  CB           retf
     +2  EE           out dx, al
     +3  CB           retf
     +4  EE 42 88 E0  out dx, al / inc dx / mov al, ah
     +8  EE CB        out dx, al / retf               */
static const uint8_t stub_code[10] = { 0xEC, 0xCB, 0xEE, 0xCB, 0xEE, 0x42, 0x88, 0xE0, 0xEE, 0xCB };

static void rm_call(__dpmi_regs *r, uint16_t ip)
{
    r->x.cs = stub_seg;
    r->x.ip = ip;
    r->x.ss = r->x.sp = 0;
    r->x.flags = 0x0202;            /* IF set: SBPRO may deliver the SB IRQ right away */
    __dpmi_simulate_real_mode_procedure_retf(r);
}

static uint8_t rm_in(uint16_t port)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.dx = port;
    rm_call(&r, 0);
    return r.h.al;
}

static void rm_out(uint16_t port, uint8_t v)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.dx = port;
    r.h.al = v;
    rm_call(&r, 2);
}

static void rm_out2(uint16_t port, uint8_t a, uint8_t b)   /* port = a, port+1 = b */
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.dx = port;
    r.h.al = a;
    r.h.ah = b;
    rm_call(&r, 4);
}

/* ------------------------------------------------------------ local state */

static uint8_t fm_index[3];         /* 388/SB+0/SB+8 bank 0, 38A/SB+2 bank 1 */
static uint8_t fm_status, busy_count;
static int mpu_pending;             /* a command was sent; its ACK may be waiting */

static int fm_slot(uint16_t p)      /* index port -> slot, or -1 */
{
    if (p == 0x388 || p == sb_base || p == sb_base + 8) return 0;
    if (p == 0x38A || p == sb_base + 2) return 1;
    return -1;
}

static int fm_data_slot(uint16_t p)
{
    return fm_slot(p - 1);
}

static uint8_t pm_in(uint16_t port)
{
    if (fm_slot(port) >= 0) return fm_status;       /* OPL status, answered here */
    if (fm_data_slot(port) >= 0) return 0xFF;
    if (port == sb_base + 0x0C) return (++busy_count & 8) ? 0xFF : 0x7F;
    if (port == mpu_base + 1 && !mpu_pending) return 0xBF;  /* ready, nothing to read */
    uint8_t v = rm_in(port);
    if (port == mpu_base + 1 && (v & 0x80)) mpu_pending = 0;
    return v;
}

static void pm_out(uint16_t port, uint8_t v)
{
    int s = fm_slot(port);
    if (s >= 0) { fm_index[s] = v; return; }
    s = fm_data_slot(port);
    if (s >= 0) {
        rm_out2(port - 1, fm_index[s], v);          /* index + data in one trip */
        if (s == 0 && fm_index[0] == 0x04) {        /* timer flags, as SBPRO does */
            if (v & 0x80) fm_status = 0;
            else {
                if ((v & 0x01) && !(v & 0x40)) fm_status |= 0xC0;
                if ((v & 0x02) && !(v & 0x20)) fm_status |= 0xA0;
            }
        }
        return;
    }
    if (port == mpu_base + 1) mpu_pending = 1;
    rm_out(port, v);
}

/* Called from sbpmtrap.S on our own stack. err = HDPMI's I/O error code. */
uint32_t pm_trap(uint32_t out, uint32_t err, uint32_t edx, uint32_t eax)
{
    if (err & SI_STRING) return eax;                /* INS/OUTS: not supported */
    uint16_t port = (err & SI_CPORT) ? (uint16_t)((err >> 8) & 0xFF) : (uint16_t)edx;
    int size = (err >> 4) & 3;                      /* 0 byte, 1 word, 3 dword */

    if (out) {
        pm_out(port, (uint8_t)eax);
        if (size) pm_out(port + 1, (uint8_t)(eax >> 8));
        return eax;
    }
    uint8_t v = pm_in(port);
    if (size == 0) return (eax & 0xFFFFFF00u) | v;
    uint8_t h = pm_in(port + 1);
    if (size == 1) return (eax & 0xFFFF0000u) | (h << 8) | v;
    return 0xFFFF0000u | (h << 8) | v;
}

/* ------------------------------------------------------------ HDPMI API */

static struct { uint32_t off; uint16_t sel; } __attribute__((packed)) hdpmi_api;

static int hdpmi_find(void)
{
    static const char name[] = "HDPMI";
    uint32_t ok, off; uint16_t sel;
    __asm__ volatile(
        "pushl %%es\n\t"
        "int $0x2F\n\t"
        "movl %%edi, %1\n\t"
        "movw %%es, %2\n\t"
        "popl %%es"
        : "=a"(ok), "=m"(off), "=m"(sel)
        : "a"(0x168A), "S"(name)
        : "edi", "cc", "memory");
    if ((ok & 0xFF) != 0) return 0;
    hdpmi_api.off = off;
    hdpmi_api.sel = sel;
    return 1;
}

struct __attribute__((packed)) trapprocs {
    uint32_t in_off;  uint16_t in_sel;
    uint32_t out_off; uint16_t out_sel;
};

static int trap_range(uint16_t start, uint16_t count, uint32_t *handle)
{
    static struct trapprocs tp;
    uint32_t h, cf;
    tp.in_off = (uint32_t)trap_in;   tp.in_sel = _my_cs();
    tp.out_off = (uint32_t)trap_out; tp.out_sel = _my_cs();
    __asm__ volatile(
        "lcall *%3\n\t"
        "sbbl %1, %1"
        : "=a"(h), "=r"(cf)
        : "a"(6), "m"(hdpmi_api), "d"(start), "c"(count), "S"(&tp)
        : "cc", "memory");
    if (cf) return 0;
    *handle = h;
    return 1;
}

static void untrap(uint32_t handle)
{
    __asm__ volatile("lcall *%1" : : "a"(7), "m"(hdpmi_api), "d"(handle) : "cc", "memory");
}

/* ------------------------------------------------------------ main */

static void parse_blaster(void)
{
    const char *b = getenv("BLASTER");
    if (!b) return;
    for (const char *p = b; *p; p++) {
        switch (*p | 0x20) {
        case 'a': sb_base = (uint16_t)strtoul(p + 1, NULL, 16); break;
        case 'd': sb_dma = (int)strtoul(p + 1, NULL, 10); break;
        case 'p': mpu_base = (uint16_t)strtoul(p + 1, NULL, 16); break;
        }
    }
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && (!strcmp(argv[1], "/D") || !strcmp(argv[1], "/d"))) { dbg = 1; first = 2; }
    parse_blaster();

    if (!hdpmi_find()) {
        printf("SBPM: HDPMI not found. Load HDPMI32i -r first.\n");
        return 1;
    }

    int sel;
    int seg = __dpmi_allocate_dos_memory(1, &sel);
    if (seg == -1) { printf("SBPM: out of DOS memory\n"); return 1; }
    stub_seg = (uint16_t)seg;
    dosmemput(stub_code, sizeof stub_code, (unsigned long)seg * 16);
    pm_ds = (uint16_t)_my_ds();

    /* SBPRO must be answering in real mode: a DSP reset should give AAh. */
    rm_out(sb_base + 6, 1);
    for (volatile int i = 0; i < 1000; i++) ;
    rm_out(sb_base + 6, 0);
    uint8_t aa = 0;
    for (int i = 0; i < 1000 && aa != 0xAA; i++)
        if (rm_in(sb_base + 0x0E) & 0x80) aa = rm_in(sb_base + 0x0A);
    if (aa != 0xAA) {
        printf("SBPM: no Sound Blaster at %Xh in real mode. Load SBPRO.DLL first.\n", sb_base);
        return 1;
    }

    struct { uint16_t start, count; } ranges[] = {
        { sb_base, 16 }, { 0x388, 4 }, { mpu_base, 2 },
        { 0x20, 2 }, { 0xA0, 2 },
        { (uint16_t)(sb_dma * 2), 2 }, { 0x08, 1 },
    };
    const int n = sizeof ranges / sizeof ranges[0];
    uint32_t handles[8];
    int got = 0;
    for (; got < n; got++) {
        if (!trap_range(ranges[got].start, ranges[got].count, &handles[got])) {
            printf("SBPM: can't trap ports %Xh-%Xh. Is this HDPMI32i (the 'i' variant)?\n",
                   ranges[got].start, ranges[got].start + ranges[got].count - 1);
            while (got--) untrap(handles[got]);
            return 1;
        }
    }
    printf("SBPM: Sound Blaster Pro at %Xh, MPU-401 at %Xh for protected-mode programs\n",
           sb_base, mpu_base);

    int rc;
    if (argc > first) {
        rc = spawnvp(P_WAIT, argv[first], argv + first);
        if (rc == -1) printf("SBPM: can't run %s\n", argv[first]);
    } else {
        const char *shell = getenv("COMSPEC");
        printf("SBPM: type EXIT to leave\n");
        rc = spawnlp(P_WAIT, shell ? shell : "COMMAND.COM", shell ? shell : "COMMAND.COM", NULL);
    }

    for (int i = n - 1; i >= 0; i--) untrap(handles[i]);
    __dpmi_free_dos_memory(sel);
    return rc < 0 ? 1 : rc;
}
