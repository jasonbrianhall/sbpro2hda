#ifndef SBOUT_H
#define SBOUT_H
#include <stdint.h>

/* The Sound Blaster's playback engine: pulls 8-bit samples from the
   game's DMA buffer, resamples to 48 kHz and raises the virtual IRQ. */

void sb_out_init(int irq, int dma);
int  sb_out_map_init(void);                 /* reserve address space for DMA buffers; 1 = ok */
void sb_out_start(int autoinit, uint32_t len_bytes, int silence);
void sb_out_stop(void);
void sb_out_exit_autoinit(void);
void sb_out_raise_irq(void);
void sb_render(int16_t *out, int frames);   /* HDA render callback */
int  sb_pending_vector(void);               /* called from the HDA IRQ; 0 = nothing to inject */

#endif
