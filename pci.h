#ifndef PCI_H
#define PCI_H
#include <stdint.h>

typedef struct { int bus, dev, fn; } PciDev;

int      pci_find_class(int cls, int sub, int index, PciDev *out);   /* index-th match */
uint32_t pci_read(PciDev d, int off);
void     pci_write(PciDev d, int off, uint32_t v);
void     pci_update_byte(PciDev d, int off, uint8_t mask, uint8_t bits);

#endif
