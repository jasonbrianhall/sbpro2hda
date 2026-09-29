/* PCI config space via mechanism #1 (0xCF8/0xCFC). */
#include "io.h"
#include "pci.h"

static uint32_t cfg_addr(PciDev d, int off)
{
    return 0x80000000u | ((uint32_t)d.bus << 16) | ((uint32_t)d.dev << 11) |
           ((uint32_t)d.fn << 8) | (uint32_t)(off & 0xFC);
}

uint32_t pci_read(PciDev d, int off)
{
    outl(0xCF8, cfg_addr(d, off));
    return inl(0xCFC);
}

void pci_write(PciDev d, int off, uint32_t v)
{
    outl(0xCF8, cfg_addr(d, off));
    outl(0xCFC, v);
}

int pci_find_class(int cls, int sub, int index, PciDev *out)
{
    PciDev d;
    for (d.bus = 0; d.bus < 256; d.bus++)
        for (d.dev = 0; d.dev < 32; d.dev++)
            for (d.fn = 0; d.fn < 8; d.fn++) {
                uint32_t id = pci_read(d, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (d.fn == 0) break;
                    continue;
                }
                uint32_t cc = pci_read(d, 0x08);
                if ((int)(cc >> 24) == cls && (int)((cc >> 16) & 0xFF) == sub && index-- == 0) {
                    *out = d;
                    return 1;
                }
                if (d.fn == 0 && !(pci_read(d, 0x0C) & 0x00800000)) break;  /* single function */
            }
    return 0;
}

/* Read-modify-write of one config byte: clear 'mask', then set 'bits'. */
void pci_update_byte(PciDev d, int off, uint8_t mask, uint8_t bits)
{
    uint32_t v = pci_read(d, off & ~3);
    int sh = (off & 3) * 8;
    v = (v & ~((uint32_t)mask << sh)) | ((uint32_t)bits << sh);
    pci_write(d, off & ~3, v);
}
