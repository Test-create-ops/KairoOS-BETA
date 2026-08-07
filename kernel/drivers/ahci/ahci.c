#include "../pci/pci.c"
#include "../../lib/framebuffer.h"
#include "../../lib/memory.h"
#include "../../memory/heap.h"

#define AHCI_CLASS   0x01
#define AHCI_SUBCLASS 0x06
#define AHCI_BAR5     0x24

// Offsets de ABAR
#define HBA_CAP       0x00
#define HBA_GHC       0x04
#define HBA_IS        0x08
#define HBA_PI        0x0C
#define HBA_PORT_BASE 0x100
#define HBA_PORT_SZ   0x80

// Port registers
#define PORT_PxCMD    0x00
#define PORT_PxTFD    0x08
#define PORT_PxSIG    0x0C
#define PORT_PxSSTS   0x10
#define PORT_PxSCTL   0x14
#define PORT_PxCI     0x20
#define PORT_PxCLB    0x28
#define PORT_PxCLBU   0x2C
#define PORT_PxFB     0x30
#define PORT_PxFBU    0x34
#define PORT_PxIE     0x3C

// Comandi ATA
#define ATA_CMD_DMA_READ  0x25
#define ATA_CMD_DMA_WRITE 0x35
#define ATA_CMD_IDENTIFY  0xEC

// PxCMD flags
#define PxCMD_ST   (1 << 0)
#define PxCMD_FRE  (1 << 4)
#define PxCMD_FR   (1 << 14)
#define PxCMD_CR   (1 << 15)

// PxSIG values
#define SIG_SATA   0x00000101
#define SIG_ATAPI  0xEB140101
#define SIG_SEMB   0xC33C0101
#define SIG_PM     0x96690101

static volatile uint32_t *ahci_abar = 0;
static int ahci_port_count = 0;
static int ahci_ports[32];
int ahci_disk_port = -1;

// Struct per Command Header (32 bytes)
typedef struct {
    uint32_t opts;
    uint32_t opts2;
    uint32_t ctba;   // Command Table Descriptor Base Address (low)
    uint32_t ctbau;  // Command Table Descriptor Base Address (high)
    uint32_t res[4];
} __attribute__((packed)) ahci_cmd_hdr_t;

// Struct per PRDT entry (16 bytes)
typedef struct {
    uint32_t dba;    // Data Base Address (low)
    uint32_t dbau;   // Data Base Address (high)
    uint32_t reserved;
    uint32_t dbc;    // Byte count (bit 31 = interrupt on completion)
} __attribute__((packed)) ahci_prdt_t;

// Struct per Command Table (128 bytes header + variable PRDT)
typedef struct {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  res[48];
    ahci_prdt_t prdt[];
} __attribute__((packed)) ahci_cmd_table_t;

// Struct per Received FIS
typedef struct {
    uint8_t  dsfis[28];
    uint8_t  pad0[4];
    uint8_t  psfis[20];
    uint8_t  pad1[12];
    uint8_t  rfis[28];
    uint8_t  pad2[4];
    uint8_t  sdbfis[8];
    uint8_t  ufis[64];
    uint8_t  pad3[96];
} __attribute__((packed)) ahci_fis_t;

static uint32_t ahci_read(volatile uint32_t *addr, int off)
{
    return *(volatile uint32_t *)((volatile char *)addr + off);
}

static void ahci_write(volatile uint32_t *addr, int off, uint32_t val)
{
    *(volatile uint32_t *)((volatile char *)addr + off) = val;
}

static int ahci_port_wait_clear(volatile uint32_t *port, int off, uint32_t mask, int timeout_ms)
{
    for (int i = 0; i < timeout_ms * 1000; i++) {
        if (!(ahci_read(port, off) & mask)) return 0;
        for (volatile int d = 0; d < 100; d++);
    }
    return -1;
}

static int ahci_port_wait_set(volatile uint32_t *port, int off, uint32_t mask, int timeout_ms)
{
    for (int i = 0; i < timeout_ms * 1000; i++) {
        if (ahci_read(port, off) & mask) return 0;
        for (volatile int d = 0; d < 100; d++);
    }
    return -1;
}

static int ahci_port_init(int port_num)
{
    volatile uint32_t *port = (volatile uint32_t *)((volatile char *)ahci_abar + HBA_PORT_BASE + port_num * HBA_PORT_SZ);
    ahci_write(port, PORT_PxCMD, ahci_read(port, PORT_PxCMD) & ~PxCMD_ST);
    ahci_port_wait_clear(port, PORT_PxCMD, PxCMD_CR, 1);
    ahci_write(port, PORT_PxCMD, ahci_read(port, PORT_PxCMD) & ~PxCMD_FRE);
    ahci_port_wait_clear(port, PORT_PxCMD, PxCMD_FR, 1);
    ahci_fis_t *fis = kmalloc(sizeof(ahci_fis_t) + 64);
    memset(fis, 0, sizeof(ahci_fis_t) + 64);
    ahci_write(port, PORT_PxFB, (uint32_t)(uintptr_t)fis);
    ahci_write(port, PORT_PxFBU, 0);
    ahci_write(port, PORT_PxIE, 0);
    ahci_cmd_hdr_t *cmd_list = kmalloc(256 + 64);
    memset(cmd_list, 0, 256 + 64);
    ahci_write(port, PORT_PxCLB, (uint32_t)(uintptr_t)cmd_list);
    ahci_write(port, PORT_PxCLBU, 0);
    ahci_write(port, PORT_PxCMD, ahci_read(port, PORT_PxCMD) | PxCMD_FRE);
    ahci_write(port, PORT_PxCMD, ahci_read(port, PORT_PxCMD) | PxCMD_ST);
    char tmp[64];
    fb_write("AHCI porta ");
    tmp[0] = '0' + port_num;
    tmp[1] = 0;
    fb_write(tmp);
    fb_write(" inizializzata\n");
    return 0;
}

static int ahci_send_cmd(int port_num, uint16_t fis_len, int is_write, uint64_t lba, uint16_t sector_count, void *data)
{
    volatile uint32_t *port = (volatile uint32_t *)((volatile char *)ahci_abar + HBA_PORT_BASE + port_num * HBA_PORT_SZ);
    ahci_cmd_hdr_t *hdr = (ahci_cmd_hdr_t *)(uintptr_t)ahci_read(port, PORT_PxCLB);
    ahci_cmd_table_t *table = kmalloc(sizeof(ahci_cmd_table_t) + sizeof(ahci_prdt_t));
    memset(table, 0, sizeof(ahci_cmd_table_t) + sizeof(ahci_prdt_t));
    memset(table->cfis, 0, 64);
    table->cfis[0] = 0x27;
    table->cfis[1] = 0x80;
    table->cfis[2] = is_write ? ATA_CMD_DMA_WRITE : ATA_CMD_DMA_READ;
    table->cfis[4] = (uint8_t)lba;
    table->cfis[5] = (uint8_t)(lba >> 8);
    table->cfis[6] = (uint8_t)(lba >> 16);
    table->cfis[7] = 0x40;
    table->cfis[8] = (uint8_t)(lba >> 24);
    table->cfis[9] = (uint8_t)(lba >> 32);
    table->cfis[10] = (uint8_t)(lba >> 40);
    table->cfis[12] = (uint8_t)sector_count;
    table->cfis[13] = (uint8_t)(sector_count >> 8);
    int data_bytes = sector_count * 512;
    table->prdt[0].dba = (uint32_t)(uintptr_t)data;
    table->prdt[0].dbc = (data_bytes - 1) | (1 << 31);
    hdr->opts = (1 << 16) | (1 << 6) | (sizeof(ahci_fis_t) / 4) | (1 << 0);
    hdr->ctba = (uint32_t)(uintptr_t)table;
    hdr->ctbau = 0;
    int slot = 0;
    ahci_write(port, PORT_PxCI, 1 << slot);
    if (ahci_port_wait_clear(port, PORT_PxCI, 1 << slot, 10000)) {
        fb_write("AHCI cmd timeout\n");
        kfree(table);
        return -1;
    }
    kfree(table);
    return 0;
}

int ahci_read_sectors(int port, uint64_t lba, uint16_t count, void *buf)
{
    return ahci_send_cmd(port, 5, 0, lba, count, buf);
}

int ahci_write_sectors(int port, uint64_t lba, uint16_t count, const void *buf)
{
    return ahci_send_cmd(port, 5, 1, lba, count, (void *)buf);
}

void ahci_init(void)
{
    for (int i = 0; i < pci_count; i++) {
        unsigned int class = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 8);
        unsigned char base_class = (class >> 24) & 0xFF;
        unsigned char sub_class = (class >> 16) & 0xFF;
        if (base_class == AHCI_CLASS && sub_class == AHCI_SUBCLASS) {
            fb_write("AHCI controller trovato!\n");
            unsigned int bar5 = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, AHCI_BAR5);
            ahci_abar = (volatile uint32_t *)(uintptr_t)(bar5 & ~0xF);
            pci_write32(pci_list[i].bus, pci_list[i].slot, 0, 4, pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 4) | 0x06);
            uint32_t pi = ahci_read(ahci_abar, HBA_PI);
            ahci_write(ahci_abar, HBA_GHC, ahci_read(ahci_abar, HBA_GHC) | (1 << 31));
            ahci_port_count = 0;
            for (int p = 0; p < 32; p++) {
                if (pi & (1 << p)) {
                    volatile uint32_t *port = (volatile uint32_t *)((volatile char *)ahci_abar + HBA_PORT_BASE + p * HBA_PORT_SZ);
                    uint32_t ssts = ahci_read(port, PORT_PxSSTS);
                    uint32_t sig = ahci_read(port, PORT_PxSIG);
                    if ((ssts & 0x0F) == 0x03 && sig == SIG_SATA) {
                        ahci_ports[ahci_port_count++] = p;
                        ahci_port_init(p);
                    }
                }
            }
            if (ahci_port_count > 0) ahci_disk_port = ahci_ports[0];
            char buf[32];
            buf[0] = '0' + ahci_port_count;
            buf[1] = 0;
            fb_write(" porte SATA trovate: ");
            fb_write(buf);
            fb_write("\n");
            break;
        }
    }
}
