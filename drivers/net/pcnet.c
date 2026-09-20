#include "pcnet.h"
#include "pci.h"
#include "idt.h"
#include "timer.h"
#include "serial.h"
#include "string.h"
#include "vga.h"

#define PCNET_VENDOR_ID 0x1022
#define PCNET_DEVICE_ID 0x2000

/* Register offsets in DWIO (32-bit access) mode */
#define REG_RDP    0x10
#define REG_RAP    0x14
#define REG_RESET  0x18
#define REG_BDP    0x1C

#define RX_COUNT 8   /* RLEN = log2(8) = 3 */
#define TX_COUNT 4   /* TLEN = log2(4) = 2 */
#define RX_RLEN  3
#define TX_TLEN  2
#define BUFFER_SIZE 1548
#define DESC_SIZE 16

typedef struct PACKED {
    uint32_t buf_addr;
    uint16_t bcnt;      /* 0xf000 | (12-bit two's complement of length) */
    uint8_t  reserved;
    uint8_t  flags;     /* bit7 = OWN, bit1 = STP (tx), bit0 = ENP (tx) */
    uint16_t mcnt;      /* actual received length goes here (rx) */
    uint16_t status2;   /* RCC / additional status - unused by this driver */
    uint32_t reserved2; /* pads the descriptor to the required 16 bytes */
} pcnet_desc_t;

typedef struct PACKED {
    uint16_t mode;
    uint8_t  rlen;       /* RLEN << 4 in high nibble */
    uint8_t  tlen;       /* TLEN << 4 in high nibble */
    uint8_t  mac[6];
    uint16_t reserved;
    uint8_t  ladr[8];
    uint32_t rx_desc_addr;
    uint32_t tx_desc_addr;
} pcnet_init_block_t;

static uint16_t io_base = 0;
static uint8_t  mac[6];
static bool nic_ready = false;

static pcnet_desc_t rx_desc[RX_COUNT] __attribute__((aligned(16)));
static pcnet_desc_t tx_desc[TX_COUNT] __attribute__((aligned(16)));
static uint8_t rx_buffers[RX_COUNT][BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[TX_COUNT][BUFFER_SIZE] __attribute__((aligned(16)));
static pcnet_init_block_t init_block __attribute__((aligned(16)));

static int rx_ptr = 0;
static int tx_ptr = 0;

static pcnet_rx_callback_t rx_callback = NULL;

void pcnet_set_rx_callback(pcnet_rx_callback_t cb) { rx_callback = cb; }
bool pcnet_is_ready(void) { return nic_ready; }
void pcnet_get_mac(uint8_t out[6]) { memcpy(out, mac, 6); }

static void write_rap(uint32_t val)              { outl(io_base + REG_RAP, val); }
static uint32_t read_rdp(void)                    { return inl(io_base + REG_RDP); }
static void write_rdp(uint32_t val)               { outl(io_base + REG_RDP, val); }
static uint32_t read_bdp(void)                    { return inl(io_base + REG_BDP); }
static void write_bdp(uint32_t val)               { outl(io_base + REG_BDP, val); }

static uint32_t read_csr(uint32_t csr) { write_rap(csr); return read_rdp(); }
static void write_csr(uint32_t csr, uint32_t val) { write_rap(csr); write_rdp(val); }
static uint32_t read_bcr(uint32_t bcr) { write_rap(bcr); return read_bdp(); }
static void write_bcr(uint32_t bcr, uint32_t val) { write_rap(bcr); write_bdp(val); }

static bool driver_owns(const pcnet_desc_t *d) { return (d->flags & 0x80) == 0; }

static void pcnet_irq_handler(registers_t *regs) {
    (void)regs;
    uint32_t csr0 = read_csr(0);

    if (csr0 & 0x0400) { /* RINT - packet(s) received */
        while (driver_owns(&rx_desc[rx_ptr])) {
            uint16_t plen = rx_desc[rx_ptr].mcnt;
            if (rx_callback && plen > 0 && plen < BUFFER_SIZE) {
                rx_callback(rx_buffers[rx_ptr], plen);
            }
            rx_desc[rx_ptr].bcnt = (uint16_t)(0xF000 | ((uint16_t)(-(int)BUFFER_SIZE) & 0x0FFF));
            rx_desc[rx_ptr].flags = 0x80; /* hand back to the card */
            rx_ptr = (rx_ptr + 1) % RX_COUNT;
        }
    }

    /* Acknowledge all pending status bits */
    write_csr(0, csr0 | 0x7F00);
}

bool pcnet_init(void) {
    uint8_t bus, slot, func;
    if (!pci_find_device(PCNET_VENDOR_ID, PCNET_DEVICE_ID, &bus, &slot, &func)) {
        serial_write("[pcnet] no card found on PCI bus\n");
        return false;
    }

    uint32_t command = pci_config_read32(bus, slot, func, 0x04);
    command |= 0x05; /* I/O space + bus mastering */
    pci_config_write32(bus, slot, func, 0x04, command);

    uint32_t bar0 = pci_config_read32(bus, slot, func, 0x10);
    io_base = (uint16_t)(bar0 & ~0x3);

    /* Read the burned-in MAC from the first 6 bytes of the card's I/O space */
    for (int i = 0; i < 6; i++) mac[i] = inb(io_base + i);

    /* Reset the card - read both the 32-bit and 16-bit reset register offsets
       since we don't know whether it's currently in DWIO 16 or 32-bit mode;
       whichever one is "real" triggers the reset, the other just reads garbage. */
    inl(io_base + 0x18);
    inw(io_base + 0x14);
    timer_sleep_ms(2);

    /* A 32-bit write to RDP switches the card into DWIO (32-bit) mode */
    write_rdp(0);

    /* Set SWSTYLE = 2 (32-bit descriptors/buffers) in CSR58 */
    uint32_t csr58 = read_csr(58);
    csr58 = (csr58 & 0xFFF0) | 2;
    write_csr(58, csr58);

    /* Ensure auto-select of the network media (BCR2 ASEL bit) */
    uint32_t bcr2 = read_bcr(2);
    bcr2 |= 0x2;
    write_bcr(2, bcr2);

    /* Set up descriptor rings - card owns all RX buffers, driver owns all TX */
    memset(rx_desc, 0, sizeof(rx_desc));
    memset(tx_desc, 0, sizeof(tx_desc));
    for (int i = 0; i < RX_COUNT; i++) {
        rx_desc[i].buf_addr = (uint32_t)(uint64_t)rx_buffers[i];
        rx_desc[i].bcnt = (uint16_t)(0xF000 | ((uint16_t)(-(int)BUFFER_SIZE) & 0x0FFF));
        rx_desc[i].flags = 0x80; /* card owns it */
    }
    for (int i = 0; i < TX_COUNT; i++) {
        tx_desc[i].buf_addr = (uint32_t)(uint64_t)tx_buffers[i];
        tx_desc[i].flags = 0x00; /* driver owns it */
    }

    /* Initialization block */
    memset(&init_block, 0, sizeof(init_block));
    init_block.mode = 0;
    init_block.rlen = (uint8_t)(RX_RLEN << 4);
    init_block.tlen = (uint8_t)(TX_TLEN << 4);
    memcpy(init_block.mac, mac, 6);
    memset(init_block.ladr, 0, 8);
    init_block.rx_desc_addr = (uint32_t)(uint64_t)rx_desc;
    init_block.tx_desc_addr = (uint32_t)(uint64_t)tx_desc;

    uint32_t ib_addr = (uint32_t)(uint64_t)&init_block;
    write_csr(1, ib_addr & 0xFFFF);
    write_csr(2, (ib_addr >> 16) & 0xFFFF);

    /* Mask TINT and IDON (we poll for init-done instead), leave RINT enabled */
    write_csr(3, 0x0300);
    /* Auto-pad short packets */
    write_csr(4, read_csr(4) | 0x0800);

    /* Hook up the PCI interrupt line before starting the card */
    uint8_t irq_line = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);
    register_interrupt_handler((uint8_t)(32 + irq_line), pcnet_irq_handler);
    irq_clear_mask(irq_line);
    if (irq_line >= 8) irq_clear_mask(2);

    /* Start initialization and poll for completion (IDON masked, so we poll CSR0 bit 8) */
    write_csr(0, 0x0001);
    int timeout = 1000000;
    while (!(read_csr(0) & 0x0100) && timeout--) { }

    /* Clear INIT+STOP, set STRT and enable interrupts */
    uint32_t csr0 = read_csr(0);
    csr0 = (csr0 & ~0x05u) | 0x02 | 0x40;
    write_csr(0, csr0);

    kprintf("[pcnet] initialized, MAC %x:%x:%x:%x:%x:%x, IRQ %d\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], irq_line);
    serial_write("[pcnet] card ready\n");
    nic_ready = true;
    return true;
}

bool pcnet_send(const void *data, uint16_t len) {
    if (len > BUFFER_SIZE) return false;
    if (!driver_owns(&tx_desc[tx_ptr])) return false; /* ring full */

    memcpy(tx_buffers[tx_ptr], data, len);
    tx_desc[tx_ptr].bcnt = (uint16_t)(0xF000 | ((uint16_t)(-(int)len) & 0x0FFF));
    tx_desc[tx_ptr].flags = 0x80 | 0x03; /* OWN | STP | ENP */

    tx_ptr = (tx_ptr + 1) % TX_COUNT;
    return true;
}

_Static_assert(sizeof(pcnet_desc_t) == 16, "pcnet_desc_t must be exactly 16 bytes");
