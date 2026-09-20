#include "rtl8139.h"
#include "pci.h"
#include "idt.h"
#include "serial.h"
#include "string.h"
#include "vga.h"

#define RTL_VENDOR_ID 0x10EC
#define RTL_DEVICE_ID 0x8139

/* Register offsets (I/O port based access via BAR0) */
#define REG_MAC0      0x00
#define REG_TSAD0     0x20
#define REG_TSD0      0x10
#define REG_RBSTART   0x30
#define REG_CMD       0x37
#define REG_CAPR      0x38
#define REG_IMR       0x3C
#define REG_ISR       0x3E
#define REG_TCR       0x40
#define REG_RCR       0x44
#define REG_CONFIG1   0x52

#define CMD_RESET     0x10
#define CMD_RX_ENABLE 0x08
#define CMD_TX_ENABLE 0x04

#define ISR_ROK 0x01
#define ISR_TOK 0x04

#define RX_BUFFER_SIZE (8192 + 16 + 1500)
static uint8_t rx_buffer[RX_BUFFER_SIZE] __attribute__((aligned(4)));

#define TX_BUFFER_SIZE 1792 /* max Ethernet frame, rounded up */
static uint8_t tx_buffers[4][TX_BUFFER_SIZE] __attribute__((aligned(4)));
static int tx_cur = 0;

static uint16_t io_base = 0;
static uint8_t  mac[6];
static uint32_t rx_offset = 0;

static rtl8139_rx_callback_t rx_callback = NULL;
static bool nic_ready = false;

bool rtl8139_is_ready(void) {
    return nic_ready;
}

void rtl8139_set_rx_callback(rtl8139_rx_callback_t cb) {
    rx_callback = cb;
}

void rtl8139_get_mac(uint8_t out[6]) {
    memcpy(out, mac, 6);
}

static void rtl8139_irq_handler(registers_t *regs) {
    (void)regs;
    uint16_t status = inw(io_base + REG_ISR);

    if (status & ISR_ROK) {
        while ((inb(io_base + REG_CMD) & 0x01) == 0) { /* buffer not empty (bit0=0 means data present, per BUFE) */
            uint16_t *header = (uint16_t*)(rx_buffer + rx_offset);
            uint16_t rx_status = header[0];
            uint16_t rx_len = header[1]; /* includes 4-byte CRC */

            if (!(rx_status & 0x01)) break; /* not a valid "ROK" packet, stop */

            uint8_t *frame = rx_buffer + rx_offset + 4;
            uint16_t frame_len = rx_len > 4 ? rx_len - 4 : 0; /* strip CRC */

            if (rx_callback && frame_len > 0 && frame_len < 1600) {
                rx_callback(frame, frame_len);
            }

            rx_offset = (rx_offset + rx_len + 4 + 3) & ~3; /* round up to 4-byte boundary */
            if (rx_offset > RX_BUFFER_SIZE) rx_offset -= RX_BUFFER_SIZE;

            outw(io_base + REG_CAPR, (uint16_t)(rx_offset - 16));
        }
    }

    outw(io_base + REG_ISR, status); /* acknowledge handled interrupts */
}

bool rtl8139_init(void) {
    uint8_t bus, slot, func;
    if (!pci_find_device(RTL_VENDOR_ID, RTL_DEVICE_ID, &bus, &slot, &func)) {
        serial_write("[rtl8139] no card found on PCI bus\n");
        return false;
    }

    /* Enable bus mastering + I/O space access (command register bits 0 and 2) */
    uint32_t command = pci_config_read32(bus, slot, func, 0x04);
    command |= 0x05;
    pci_config_write32(bus, slot, func, 0x04, command);

    uint32_t bar0 = pci_config_read32(bus, slot, func, 0x10);
    io_base = (uint16_t)(bar0 & ~0x3);

    /* Power on */
    outb(io_base + REG_CONFIG1, 0x00);

    /* Software reset, wait for it to clear */
    outb(io_base + REG_CMD, CMD_RESET);
    int timeout = 100000;
    while ((inb(io_base + REG_CMD) & CMD_RESET) && timeout--);

    /* Read the burned-in MAC address */
    for (int i = 0; i < 6; i++) mac[i] = inb(io_base + REG_MAC0 + i);

    /* Set up the RX buffer (physical address - identity mapped, so == virtual) */
    outl(io_base + REG_RBSTART, (uint32_t)(uint64_t)rx_buffer);

    /* Enable ROK + TOK interrupts */
    outw(io_base + REG_IMR, ISR_ROK | ISR_TOK);

    /* Accept all packets (broadcast/multicast/physical match/promisc) + WRAP */
    outl(io_base + REG_RCR, 0x0F | (1 << 7));

    /* Enable RX and TX */
    outb(io_base + REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);

    /* Hook up the PCI interrupt line */
    uint8_t irq_line = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);
    serial_write("[rtl8139] pci device found, hooking up IRQ\n");
    register_interrupt_handler((uint8_t)(32 + irq_line), rtl8139_irq_handler);
    irq_clear_mask(irq_line);
    if (irq_line >= 8) irq_clear_mask(2); /* cascade */

    kprintf("[rtl8139] initialized, MAC %x:%x:%x:%x:%x:%x, IRQ %d\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], irq_line);
    serial_write("[rtl8139] card ready\n");
    nic_ready = true;
    return true;
}

bool rtl8139_send(const void *data, uint16_t len) {
    if (len > TX_BUFFER_SIZE) return false;

    memcpy(tx_buffers[tx_cur], data, len);
    outl(io_base + REG_TSAD0 + tx_cur * 4, (uint32_t)(uint64_t)tx_buffers[tx_cur]);
    outl(io_base + REG_TSD0 + tx_cur * 4, len); /* also clears OWN bit, starts TX */

    tx_cur = (tx_cur + 1) % 4;
    return true;
}
