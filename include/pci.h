#ifndef NUGGET_PCI_H
#define NUGGET_PCI_H
#include "types.h"

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/* Scans all buses/slots/functions for a device matching vendor:device.
 * Returns true and fills bus/slot/func if found. */
bool pci_find_device(uint16_t vendor_id, uint16_t device_id, uint8_t *bus, uint8_t *slot, uint8_t *func);

#endif
