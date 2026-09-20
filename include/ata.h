#ifndef NUGGET_ATA_H
#define NUGGET_ATA_H
#include "types.h"

#define ATA_SECTOR_SIZE 512

bool ata_init(void);
bool ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);
bool ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);

#endif
