#include "../pci/pci.c"
#include "../../lib/io.h"
#include "../../lib/framebuffer.h"
#include <stdint.h>

// RTL8139 registers (I/O ports)
#define RTL_IDR0     0x00
#define RTL_IDR4     0x04
#define RTL_CR       0x37
#define RTL_RBSA     0x30
#define RTL_CAPR     0x38
#define RTL_IMR      0x3C
#define RTL_ISR      0x3E
#define RTL_TSD0     0x40
#define RTL_TSAD0    0x44
#define RTL_TSD1     0x48
#define RTL_TSAD1    0x4C
#define RTL_TSD2     0x50
#define RTL_TSAD2    0x54
#define RTL_TSD3     0x58
#define RTL_TSAD3    0x5C
#define RTL_CONFIG1  0x52
#define RTL_CONFIG5  0xDA

// CR bits
#define CR_RST  0x10
#define CR_RE   0x08
#define CR_TE   0x04

// ISR bits
#define ISR_ROK 0x01
#define ISR_TOK 0x02
#define ISR_RER 0x04
#define ISR_TER 0x08

#define RX_BUF_SIZE 8192
#define TX_BUF_SIZE 2048

static uint16_t rtl_iobase = 0;
static uint8_t rtl_mac[6];
static int rtl_irq = 0;

// RX ring buffer (must be 16-byte aligned)
static uint8_t rx_ring[RX_BUF_SIZE + 16] __attribute__((aligned(16)));
static int rx_offset = 0;

// TX buffers (4 descriptors)
static uint8_t tx_bufs[4][TX_BUF_SIZE] __attribute__((aligned(16)));
static int tx_cur = 0;

// Public MAC
uint8_t nic_mac[6];
int nic_ready = 0;

static void rtl_write8(uint16_t reg, uint8_t val) {
    outb(rtl_iobase + reg, val);
}

static uint8_t rtl_read8(uint16_t reg) {
    return inb(rtl_iobase + reg);
}

static void rtl_write16(uint16_t reg, uint16_t val) {
    outw(rtl_iobase + reg, val);
}

static uint16_t rtl_read16(uint16_t reg) {
    return inw(rtl_iobase + reg);
}

static void rtl_write32(uint16_t reg, uint32_t val) {
    outl(rtl_iobase + reg, val);
}

static uint32_t rtl_read32(uint16_t reg) {
    return inl(rtl_iobase + reg);
}

void rtl8139_init(void) {
    for (int i = 0; i < pci_count; i++) {
        if (pci_list[i].vendor == 0x10EC && pci_list[i].device == 0x8139) {
            fb_write("RTL8139: detected\n");
            uint32_t bar0 = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x10);
            rtl_iobase = bar0 & 0xFFFC;
            uint32_t irq_line = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x3C);
            rtl_irq = irq_line & 0xFF;
            break;
        }
    }
    if (!rtl_iobase) {
        fb_write("RTL8139: not found\n");
        return;
    }

    // Power on the chip (clear CONFIG1 PM bits)
    rtl_write8(RTL_CONFIG1, 0x00);

    // Software reset
    rtl_write8(RTL_CR, CR_RST);
    for (volatile int d = 0; d < 10000; d++) asm volatile("pause");
    for (int w = 0; w < 100; w++) {
        if (!(rtl_read8(RTL_CR) & CR_RST)) break;
        for (volatile int d = 0; d < 1000; d++) asm volatile("pause");
    }

    // Read MAC address
    rtl_mac[0] = rtl_read8(RTL_IDR0);
    rtl_mac[1] = rtl_read8(RTL_IDR0 + 1);
    rtl_mac[2] = rtl_read8(RTL_IDR0 + 2);
    rtl_mac[3] = rtl_read8(RTL_IDR0 + 3);
    rtl_mac[4] = rtl_read8(RTL_IDR0 + 4);
    rtl_mac[5] = rtl_read8(RTL_IDR0 + 5);
    for (int i = 0; i < 6; i++) nic_mac[i] = rtl_mac[i];

    fb_write("RTL8139: MAC ");
    for (int i = 0; i < 6; i++) {
        char hex[3];
        hex[0] = "0123456789ABCDEF"[rtl_mac[i] >> 4];
        hex[1] = "0123456789ABCDEF"[rtl_mac[i] & 0xF];
        hex[2] = 0;
        fb_write(hex);
        if (i < 5) fb_write(":");
    }
    fb_write("\n");

    // Set RX buffer address (physical address, but in kernel space it's fine)
    uint32_t rx_phys = (uint32_t)(uint64_t)rx_ring;
    rtl_write32(RTL_RBSA, rx_phys);

    // Unmask interrupts: RX OK + TX OK + RX error + TX error
    rtl_write16(RTL_IMR, ISR_ROK | ISR_TOK | ISR_RER | ISR_TER);

    // Enable RX and TX
    rtl_write8(RTL_CR, CR_TE | CR_RE);

    // Set CONFIG5: byte order enable (required for PCI)
    uint8_t cfg5 = rtl_read8(RTL_CONFIG5);
    rtl_write8(RTL_CONFIG5, cfg5 | 0x80);

    nic_ready = 1;
    fb_write("RTL8139: ready\n");
}

int rtl8139_send(const void *data, int len) {
    if (!nic_ready) return -1;
    if (len < 14 || len > TX_BUF_SIZE) return -1;

    int tx = tx_cur;
    tx_cur = (tx_cur + 1) & 3;

    // Copy packet to TX buffer
    const uint8_t *src = (const uint8_t*)data;
    for (int i = 0; i < len; i++)
        tx_bufs[tx][i] = src[i];

    // Set TX address
    uint32_t tx_phys = (uint32_t)(uint64_t)&tx_bufs[tx];
    rtl_write32(RTL_TSAD0 + tx * 8, tx_phys);

    // Send: write size with early TX flag
    rtl_write32(RTL_TSD0 + tx * 8, len | 0x80000000);

    // Wait for TX to complete (polling)
    for (volatile int d = 0; d < 50000; d++) {
        uint32_t stat = rtl_read32(RTL_TSD0 + tx * 8);
        if (stat & 0x80000000) break;
        asm volatile("pause");
    }

    return len;
}

// Last received packet buffer
static uint8_t last_pkt[1800];
static int last_pkt_len = 0;

// Poll for incoming packet.
// Returns packet length, 0 if nothing received.
// Data can be read with rtl8139_read_packet().
int rtl8139_poll(void) {
    if (!nic_ready) return 0;

    uint16_t isr = rtl_read16(RTL_ISR);
    if (isr & ISR_RER) {
        rtl_write16(RTL_ISR, isr);
        rx_offset = 0;
        uint32_t rx_phys = (uint32_t)(uint64_t)rx_ring;
        rtl_write32(RTL_RBSA, rx_phys);
        rtl_write8(RTL_CR, CR_TE | CR_RE);
        return 0;
    }

    uint16_t rx_status = *(volatile uint16_t*)&rx_ring[rx_offset];
    if (!(rx_status & 0x0001)) return 0;

    uint16_t rx_size = *(volatile uint16_t*)&rx_ring[rx_offset + 2];
    int pkt_size = rx_size & 0x3FFF;

    if (pkt_size < 14 || pkt_size > 1800) {
        rx_offset = 0;
        rtl_write16(RTL_CAPR, rx_offset);
        return 0;
    }

    // Save packet data
    const uint8_t *src = (const uint8_t*)&rx_ring[rx_offset + 4];
    for (int i = 0; i < pkt_size; i++)
        last_pkt[i] = src[i];
    last_pkt_len = pkt_size;

    // Update CAPR
    int new_offset = rx_offset + ((rx_size + 4 + 3) & ~3);
    if (new_offset >= RX_BUF_SIZE) new_offset -= RX_BUF_SIZE;
    rtl_write16(RTL_CAPR, new_offset & 0xFFFC);
    rx_offset = new_offset;

    return pkt_size;
}

// Copy received packet data
void rtl8139_read_packet(uint8_t *dst) {
    for (int i = 0; i < last_pkt_len; i++)
        dst[i] = last_pkt[i];
}

int rtl8139_last_len(void) {
    return last_pkt_len;
}

// Access to MAC for higher layers
void rtl8139_get_mac(uint8_t *mac) {
    for (int i = 0; i < 6; i++) mac[i] = rtl_mac[i];
}
