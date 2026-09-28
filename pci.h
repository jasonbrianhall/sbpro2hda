#ifndef PCI_H
#define PCI_H
#include <stdint.h>

typedef struct { int bus, dev, fn; } PciDev;

int      pci_find_class(int cls, int sub, PciDev *out);
uint32_t pci_read(PciDev d, int off);
void     pci_write(PciDev d, int off, uint32_t v);

#endif
