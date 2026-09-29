#ifndef HDA_H
#define HDA_H
#include <stdint.h>

#define HDA_RATE 48000

/* Fills 'frames' interleaved stereo 16-bit frames. Called at interrupt time:
   no floating point, no DOS/BIOS calls, no printf. */
typedef void (*hda_render_fn)(int16_t *out, int frames);

int  hda_init(void);            /* finds controller, routes codec, sets up stream; 1 = ok */
int  hda_irq(void);             /* PCI interrupt line */
int  hda_start(hda_render_fn fn);  /* installs the IRQ handler and runs the stream */
void hda_stop(void);
extern uint32_t hda_underruns;

#endif
