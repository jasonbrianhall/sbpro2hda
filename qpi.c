#include <string.h>
#include <dpmi.h>
#include "qpi.h"

static uint16_t qpi_seg, qpi_off;

int qpi_detect(void)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0xD201;
    r.x.bx = 0x4849;            /* 'HI' */
    r.x.cx = 0x5241;            /* 'RA' */
    r.x.dx = 0x4D30;            /* 'M0' */
    __dpmi_simulate_real_mode_interrupt(0x2F, &r);
    if (r.x.bx != 0x4F4B)       /* 'OK' */
        return 0;
    qpi_seg = r.x.es;
    qpi_off = r.x.di;
    return 1;
}

static int qpi_call(__dpmi_regs *r)
{
    r->x.cs = qpi_seg;
    r->x.ip = qpi_off;
    r->x.ss = r->x.sp = 0;      /* host supplies a stack */
    r->x.flags = 0;
    __dpmi_simulate_real_mode_procedure_retf(r);
    return !(r->x.flags & 1);
}

int qpi_version(void)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.h.ah = 0x03;
    return qpi_call(&r) ? r.x.bx : 0;
}

int qpi_get_io_callback(uint16_t *seg, uint16_t *off)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x1A06;
    if (!qpi_call(&r)) return 0;
    *seg = r.x.es;
    *off = r.x.di;
    return 1;
}

int qpi_set_io_callback(uint16_t seg, uint16_t off)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x1A07;
    r.x.es = seg;
    r.x.di = off;
    return qpi_call(&r);
}

int qpi_trap_port(uint16_t port)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x1A09;
    r.x.dx = port;
    return qpi_call(&r);
}

int qpi_untrap_port(uint16_t port)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x1A0A;
    r.x.dx = port;
    return qpi_call(&r);
}
