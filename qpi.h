#ifndef QPI_H
#define QPI_H
#include <stdint.h>

/* QEMM Programming Interface, as provided by QEMM or by JEMM with
   JLOAD QPIEMU.DLL. Used for real-mode (V86) I/O port trapping. */

/* Flags in CL when the I/O callback is entered. */
#define QPI_IO_OUT  0x04        /* set = OUT, clear = IN */
#define QPI_IO_WORD 0x08        /* word access */

int qpi_detect(void);
int qpi_version(void);                                  /* BCD, e.g. 0x0703 */
int qpi_set_io_callback(uint16_t seg, uint16_t off);
int qpi_get_io_callback(uint16_t *seg, uint16_t *off);
int qpi_trap_port(uint16_t port);
int qpi_untrap_port(uint16_t port);

#endif
