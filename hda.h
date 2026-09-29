#ifndef HDA_H
#define HDA_H
#include <stdint.h>

#define HDA_RATE 48000

/* Fills 'frames' interleaved stereo 16-bit frames. Called at interrupt time:
   no floating point, no DOS/BIOS calls, no printf. */
typedef void (*hda_render_fn)(int16_t *out, int frames);

int  hda_init(int want, int hdmi);  /* controller number (0 = auto), use HDMI; 1 = ok */
int  hda_irq(void);             /* PCI interrupt line */
typedef int (*hda_busy_fn)(void);  /* 1 = keep the stream running even if silent */
int  hda_start(hda_render_fn fn, hda_busy_fn busy_fn);  /* IRQ handler + stream */
void hda_wake(void);            /* restart a stream paused while idle */
void hda_stop(void);
extern uint32_t hda_underruns;

#endif
