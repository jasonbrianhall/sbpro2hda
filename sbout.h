#ifndef SBOUT_H
#define SBOUT_H
#include <stdint.h>
#include "jlm.h"

/* The Sound Blaster's playback engine: pulls 8-bit samples from the
   game's DMA buffer, resamples to 48 kHz and raises the virtual IRQ. */

void sb_out_init(int irq, int dma);
int  sb_out_map_init(void);                 /* reserve address space for DMA buffers; 1 = ok */
void sb_out_start(int autoinit, uint32_t len_bytes, int silence);
void sb_out_stop(void);
void sb_out_exit_autoinit(void);
void sb_out_raise_irq(void);
void sb_render(int16_t *out, int frames);   /* HDA render callback */
int  sb_pending_vector(void);
void sb_inject(Client *c, int vec);         /* deliver the SB IRQ into V86 */
void sb_isr_done(void);                     /* from sbret_thunk */
void sb_tick(void);                         /* from the HDA interrupt */
extern uint32_t sb_ret_callback;
extern uint32_t sb_hold_addr;               /* SBPM's CLI flag (linear), 0 = none */               /* called from the HDA IRQ; 0 = nothing to inject */

/* 8237 register emulation for the SB channel (address, count, status). */
int     sb_dma_owns(uint16_t port);
uint8_t sb_dma_in(uint16_t port);
void    sb_dma_out(uint16_t port, uint8_t v);

#endif
