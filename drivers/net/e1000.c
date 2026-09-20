#include "e1000.h"
#include "pci.h"
#include "idt.h"
#include "serial.h"
#include "string.h"
#include "vga.h"

/* A handful of common e1000/e1000e device IDs - covers QEMU's default
   emulated NIC (82540EM), several other 8254x-family chips found on real
   PCI/PCIe network cards, and the 82574L which was extremely common on
   real desktop/server motherboards for years. This isn't every e1000e
   variant that exists (there are dozens), but it's a reasonable, honest
   subset rather than pretending to cover every Intel NIC ever made. */
#define E1000_VENDOR_ID 0x8086
static const uint16_t E1000_DEVICE_IDS[] = {
    0x100E, /* 82540EM - QEMU's "-device e1000" default */
    0x100F, /* 82545EM */
    0x1004, /* 82543GC */
    0x1010, /* 82546EB */
    0x1019, /* 82547EI */
    0x10D3, /* 82574L - very common on real motherboards */
};
#define E1000_DEVICE_ID_COUNT (sizeof(E1000_DEVICE_IDS) / sizeof(E1000_DEVICE_IDS[0]))

/* Register offsets (memory-mapped, unlike RTL8139's I/O-port ones) */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_RST      (1u << 26)
#define CTRL_SLU      (1u << 6)  /* set link up */
#define RCTL_EN       (1u << 1)
#define RCTL_BAM      (1u << 15) /* accept broadcast */
#define RCTL_BSIZE_2048 0        /* buffer size bits = 00 -> 2048 bytes */
#define RCTL_SECRC    (1u << 26) /* strip Ethernet CRC */
#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)

#define NUM_RX_DESC 32
#define NUM_TX_DESC 8
#define RX_BUF_SIZE 2048

typedef struct PACKED {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct PACKED {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

#define TX_CMD_EOP  0x01
#define TX_CMD_IFCS 0x02
#define TX_CMD_RS   0x08
#define TX_STATUS_DD 0x01
#define RX_STATUS_DD 0x01

static volatile uint8_t *mmio_base = NULL;
static uint8_t mac[6];
static bool nic_ready = false;
static e1000_rx_callback_t rx_callback = NULL;

static e1000_rx_desc_t rx_descs[NUM_RX_DESC] __attribute__((aligned(16)));
static uint8_t rx_buffers[NUM_RX_DESC][RX_BUF_SIZE] __attribute__((aligned(16)));
static uint32_t rx_cur = 0;

static e1000_tx_desc_t tx_descs[NUM_TX_DESC] __attribute__((aligned(16)));
static uint8_t tx_buffers[NUM_TX_DESC][1792] __attribute__((aligned(16)));
static uint32_t tx_cur = 0;

static inline uint32_t reg_read(uint32_t offset) {
    return *(volatile uint32_t*)(mmio_base + offset);
}
static inline void reg_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(mmio_base + offset) = value;
}

bool e1000_is_ready(void) { return nic_ready; }
void e1000_set_rx_callback(e1000_rx_callback_t cb) { rx_callback = cb; }
void e1000_get_mac(uint8_t out[6]) { memcpy(out, mac, 6); }

static void e1000_irq_handler(registers_t *regs) {
    (void)regs;
    reg_read(REG_ICR); /* reading ICR also acknowledges it */

    while ((rx_descs[rx_cur].status & RX_STATUS_DD)) {
        uint16_t len = rx_descs[rx_cur].length;
        if (rx_callback && len > 0 && len < RX_BUF_SIZE) {
            rx_callback(rx_buffers[rx_cur], len);
        }
        rx_descs[rx_cur].status = 0;
        reg_write(REG_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    }
}

bool e1000_init(void) {
    uint8_t bus = 0, slot = 0, func = 0;
    bool found = false;
    for (uint32_t i = 0; i < E1000_DEVICE_ID_COUNT; i++) {
        if (pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_IDS[i], &bus, &slot, &func)) {
            found = true;
            break;
        }
    }
    if (!found) {
        serial_write("[e1000] no supported Intel NIC found on PCI bus\n");
        return false;
    }

    /* Enable bus mastering + memory space access */
    uint32_t command = pci_config_read32(bus, slot, func, 0x04);
    command |= 0x06; /* memory space (bit1) + bus master (bit2) */
    pci_config_write32(bus, slot, func, 0x04, command);

    uint32_t bar0 = pci_config_read32(bus, slot, func, 0x10);
    uint64_t mmio_phys = bar0 & ~0xFu;
    if (((bar0 >> 1) & 0x3) == 0x2) { /* 64-bit BAR: high dword lives in the next register */
        uint32_t bar1 = pci_config_read32(bus, slot, func, 0x14);
        mmio_phys |= ((uint64_t)bar1) << 32;
    }
    mmio_base = (volatile uint8_t*)mmio_phys;

    /* Full reset, then wait a bit for it to settle (no separate "reset done" bit to poll here) */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 1000000; i++) { }

    /* Bring the link up */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU);

    /* Mask all interrupts, then enable just RX-related ones */
    reg_write(REG_IMC, 0xFFFFFFFF);
    reg_write(REG_IMS, 0x1F6DC);

    /* MAC address - present in RAL0/RAH0 after reset on real hardware and QEMU alike */
    uint32_t ral = reg_read(REG_RAL0);
    uint32_t rah = reg_read(REG_RAH0);
    mac[0] = (uint8_t)(ral & 0xFF);
    mac[1] = (uint8_t)((ral >> 8) & 0xFF);
    mac[2] = (uint8_t)((ral >> 16) & 0xFF);
    mac[3] = (uint8_t)((ral >> 24) & 0xFF);
    mac[4] = (uint8_t)(rah & 0xFF);
    mac[5] = (uint8_t)((rah >> 8) & 0xFF);

    /* Re-write RAL0/RAH0 with the Address Valid bit (RAH bit 31) forced
       on. A power-on reset loads these from EEPROM with AV already set,
       but our software CTRL_RST doesn't reliably guarantee that - without
       it, the card only matches broadcast/multicast, not real unicast
       traffic addressed to us (which is exactly what broke ARP replies:
       DHCP's broadcast responses got through fine, but a unicast ARP
       reply didn't). */
    reg_write(REG_RAL0, ral);
    reg_write(REG_RAH0, rah | (1u << 31));

    /* Set up the RX descriptor ring */
    memset(rx_descs, 0, sizeof(rx_descs));
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_descs[i].addr = (uint64_t)(uint64_t)rx_buffers[i];
    }
    reg_write(REG_RDBAL, (uint32_t)(uint64_t)rx_descs);
    reg_write(REG_RDBAH, 0);
    reg_write(REG_RDLEN, NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, NUM_RX_DESC - 1);
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);

    /* Set up the TX descriptor ring */
    memset(tx_descs, 0, sizeof(tx_descs));
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_descs[i].addr = (uint64_t)(uint64_t)tx_buffers[i];
        tx_descs[i].status = TX_STATUS_DD; /* mark all as "already sent" so the ring starts free */
    }
    reg_write(REG_TDBAL, (uint32_t)(uint64_t)tx_descs);
    reg_write(REG_TDBAH, 0);
    reg_write(REG_TDLEN, NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));

    uint8_t irq_line = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);
    serial_write("[e1000] pci device found, hooking up IRQ\n");
    register_interrupt_handler((uint8_t)(32 + irq_line), e1000_irq_handler);
    irq_clear_mask(irq_line);
    if (irq_line >= 8) irq_clear_mask(2);

    kprintf("[e1000] initialized, MAC %x:%x:%x:%x:%x:%x, IRQ %d\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], irq_line);
    serial_write("[e1000] card ready\n");
    nic_ready = true;
    return true;
}

bool e1000_send(const void *data, uint16_t len) {
    if (len > sizeof(tx_buffers[0])) return false;

    int timeout = 100000;
    while (!(tx_descs[tx_cur].status & TX_STATUS_DD) && timeout--) { }
    if (!(tx_descs[tx_cur].status & TX_STATUS_DD)) return false;

    memcpy(tx_buffers[tx_cur], data, len);
    /* Ethernet's minimum frame size is 60 bytes (before the 4-byte CRC the
       hardware appends itself) - short frames like ARP requests (~42
       bytes) need padding, or some NICs will treat them as invalid "runt"
       frames and drop them rather than actually transmitting them. */
    uint16_t padded_len = len;
    if (padded_len < 60) {
        memset(tx_buffers[tx_cur] + len, 0, 60 - len);
        padded_len = 60;
    }
    tx_descs[tx_cur].length = padded_len;
    tx_descs[tx_cur].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    tx_descs[tx_cur].status = 0;

    uint32_t next = (tx_cur + 1) % NUM_TX_DESC;
    reg_write(REG_TDT, next);
    tx_cur = next;
    return true;
}
