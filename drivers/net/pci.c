#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_make_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (uint32_t)((1u << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC));
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t data = pci_config_read32(bus, slot, func, offset);
    return (uint16_t)((data >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, uint8_t *bus, uint8_t *slot, uint8_t *func) {
    for (uint32_t b = 0; b < 256; b++) {
        for (uint32_t s = 0; s < 32; s++) {
            for (uint32_t f = 0; f < 8; f++) {
                uint16_t vendor = pci_config_read16((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x00);
                if (vendor == 0xFFFF) continue; /* no device */
                uint16_t device = pci_config_read16((uint8_t)b, (uint8_t)s, (uint8_t)f, 0x02);
                if (vendor == vendor_id && device == device_id) {
                    if (bus) *bus = (uint8_t)b;
                    if (slot) *slot = (uint8_t)s;
                    if (func) *func = (uint8_t)f;
                    return true;
                }
            }
        }
    }
    return false;
}
