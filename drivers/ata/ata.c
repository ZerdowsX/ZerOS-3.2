#include "ata.h"
#include "serial.h"
#include "idt.h"
#include "timer.h"

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6

#define ATA_REG_DATA        (ATA_PRIMARY_IO + 0)
#define ATA_REG_ERROR       (ATA_PRIMARY_IO + 1)
#define ATA_REG_SECCOUNT    (ATA_PRIMARY_IO + 2)
#define ATA_REG_LBA0        (ATA_PRIMARY_IO + 3)
#define ATA_REG_LBA1        (ATA_PRIMARY_IO + 4)
#define ATA_REG_LBA2        (ATA_PRIMARY_IO + 5)
#define ATA_REG_DRIVE       (ATA_PRIMARY_IO + 6)
#define ATA_REG_STATUS      (ATA_PRIMARY_IO + 7)
#define ATA_REG_COMMAND     (ATA_PRIMARY_IO + 7)

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

static void ata_delay(void) {
    /* Reading the alternate status register 4 times gives ~400ns delay */
    for (int i = 0; i < 4; i++) inb(ATA_PRIMARY_CTRL);
}

static bool ata_wait_ready(void) {
    uint64_t start = timer_ticks();
    uint64_t timeout_ticks = 100; /* 1 real second at our 100 Hz timer, regardless of per-instruction emulation cost */
    while (timer_ticks() - start < timeout_ticks) {
        uint8_t status = inb(ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return false;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return true;
    }
    return false;
}

bool ata_init(void) {
    /* We use polled PIO mode (see ata_wait_ready), not interrupt-driven I/O,
       so mask the ATA IRQ lines - otherwise the drive's INTRQ line keeps
       firing after every command and floods the PIC with unhandled IRQs. */
    irq_set_mask(14);
    irq_set_mask(15);

    /* Select master drive */
    outb(ATA_REG_DRIVE, 0xA0);
    ata_delay();
    outb(ATA_REG_SECCOUNT, 0);
    outb(ATA_REG_LBA0, 0);
    outb(ATA_REG_LBA1, 0);
    outb(ATA_REG_LBA2, 0);
    outb(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_REG_STATUS);
    if (status == 0) {
        serial_write("[ata] no drive detected on primary master\n");
        return false;
    }

    uint64_t start = timer_ticks();
    while ((inb(ATA_REG_STATUS) & ATA_SR_BSY) && timer_ticks() - start < 100) { }

    if (inb(ATA_REG_LBA1) || inb(ATA_REG_LBA2)) {
        serial_write("[ata] non-ATA device detected, unsupported\n");
        return false;
    }

    start = timer_ticks();
    while (timer_ticks() - start < 100) {
        status = inb(ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            serial_write("[ata] IDENTIFY error\n");
            return false;
        }
        if (status & ATA_SR_DRQ) break;
    }

    /* Drain the 256-word identify data, we don't need it yet */
    for (int i = 0; i < 256; i++) inw(ATA_REG_DATA);

    serial_write("[ata] primary master drive ready (PIO mode)\n");
    return true;
}

bool ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    uint16_t *buf = (uint16_t*)buffer;

    outb(ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_REG_SECCOUNT, count);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for (uint8_t s = 0; s < count; s++) {
        if (!ata_wait_ready()) {
            serial_write("[ata] read timeout/error\n");
            return false;
        }
        for (int i = 0; i < 256; i++) {
            buf[s * 256 + i] = inw(ATA_REG_DATA);
        }
    }
    return true;
}

bool ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    const uint16_t *buf = (const uint16_t*)buffer;

    outb(ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_REG_SECCOUNT, count);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (uint8_t s = 0; s < count; s++) {
        if (!ata_wait_ready()) {
            serial_write("[ata] write timeout/error\n");
            return false;
        }
        for (int i = 0; i < 256; i++) {
            outw(ATA_REG_DATA, buf[s * 256 + i]);
        }
    }
    /* Flush cache once after all sectors, instead of after every single one -
       cuts the number of wait cycles roughly in half for multi-sector writes. */
    outb(ATA_REG_COMMAND, 0xE7);
    ata_wait_ready();
    return true;
}
