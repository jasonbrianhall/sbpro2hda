#ifndef PIC_H
#define PIC_H
#include <stdint.h>

/* Virtualized 8259 interrupt masks. Games often write a whole new mask to
   port 21h/A1h, which would silence SBPRO's own HD Audio interrupt. Games
   see and set their own mask; SBPRO's IRQ (and the cascade) stay open. */

void    pic_init(int hda_irq);
int     pic_owns(uint16_t port);
uint8_t pic_in(uint16_t port);
void    pic_out(uint16_t port, uint8_t v);
int     pic_masked(int irq);        /* as the game sees it */
void    pic_restore(void);          /* on unload: put the game's mask back as is */
void    pic_virtual_start(int irq);  /* emulated IRQ delivered: now "in service" */
int     pic_virtual_busy(int irq);   /* still waiting for the game's EOI */
void    pic_tick(void);              /* from the HDA interrupt: EOI timeout */
extern int pic_watching;             /* EOIs are visible (ports trapped) */

#endif
