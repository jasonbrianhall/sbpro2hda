#ifndef DSP_H
#define DSP_H
#include <stdint.h>

/* Sound Blaster Pro 2.0 DSP (v3.02) and mixer, port-level. */

typedef struct {
    uint8_t  time_constant;     /* 40h: rate = 1000000 / (256 - tc), per channel pair in stereo */
    uint16_t block_len;         /* 48h, bytes - 1 */
    uint16_t sc_len;            /* 14h single-cycle length, bytes - 1 */
    uint8_t  speaker;
    uint8_t  dac_value;         /* 10h direct output */
    uint8_t  pending;           /* DSP_START_* set by commands, consumed by the DMA engine */
    uint8_t  paused;
    uint8_t  mixer[256];
} DspState;

enum { DSP_START_NONE, DSP_START_SINGLE, DSP_START_AUTO, DSP_STOP_AUTO };

extern DspState dsp;

void    dsp_init(uint16_t base);
int     dsp_owns(uint16_t port);
uint8_t dsp_in(uint16_t port);
void    dsp_out(uint16_t port, uint8_t v);

/* Hook for F2h/F3h (IRQ request). Set by the IRQ injection layer. */
extern void (*dsp_irq_request)(void);

#endif
