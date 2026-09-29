#ifndef OPL_H
#define OPL_H
#include <stdint.h>

/* OPL3 FM synthesis (DOSBox's dbopl). */

#define OPL_MAX_FRAMES 1024

#ifdef __cplusplus
extern "C" {
#endif
void opl_init(int rate);                    /* load time only (uses the FPU) */
void opl_write(uint32_t reg, uint8_t val);  /* reg 0x000-0x1FF */
void opl_mix(int32_t *out, int frames);     /* adds interleaved stereo */
extern int32_t opl_peak;                    /* debug: peak output level */
#ifdef __cplusplus
}
#endif

#endif
