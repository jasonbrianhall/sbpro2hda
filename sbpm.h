#ifndef SBPM_H
#define SBPM_H
#include <stdint.h>

/* HDPMI I/O trap error-code bits (see HDPMIAPI.TXT) */
#define SI_STRING 0x08
#define SI_CPORT  0x40

extern uint16_t pm_ds;              /* our data selector, for the trap handler */
void trap_in(void);                 /* sbpmtrap.S: HDPMI calls these */
void trap_out(void);
uint32_t pm_trap(uint32_t out, uint32_t err, uint32_t edx, uint32_t eax);

#endif
