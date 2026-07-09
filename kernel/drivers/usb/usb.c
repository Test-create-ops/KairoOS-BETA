#include "usb.h"
#include "../../lib/io.h"

// ─── PCI Configuration Space Access ───

static uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = 0x80000000 | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t addr = 0x80000000 | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, value);
}

// ─── UHCI Registers (all I/O port based) ───

#define USBCMD     0x00
#define USBSTS     0x02
#define USBINTR    0x04
#define FRNUM      0x06
#define FLBASEADD  0x08
#define SOFMOD     0x0C
#define PORTSC1    0x10
#define PORTSC2    0x12

// USBCMD bits
#define CMD_RUN     0x0001
#define CMD_HCRESET 0x0002
#define CMD_GRESET  0x0004
#define CMD_CF      0x0010  // Configured Flag
#define CMD_MAXP    0x0040  // Max packet (0=32, 1=64)

// USBSTS bits
#define STS_USBINT  0x0001
#define STS_ERROR   0x0002
#define STS_PCD     0x0004  // Port Change Detect
#define STS_HCH     0x0020  // Host Controller Halted

// PORTSC bits
#define PORT_CONN       0x0001
#define PORT_CONNCHG    0x0002
#define PORT_ENABLE     0x0004
#define PORT_ENABLECHG  0x0008
#define PORT_LSDA       0x0100  // Low Speed Device Attached
#define PORT_RESET      0x0200
#define PORT_SUSPEND    0x1000

// ─── UHCI Data Structures ───

#define FRAME_LIST_SIZE 1024
#define MAX_TDS 32
#define MAX_USB_DEVICES 8

// Transfer Descriptor (16 bytes, 16-byte aligned)
struct uhci_td {
    uint32_t link;     // Link pointer
    uint32_t status;   // Control/status
    uint32_t token;    // Token (PID, device, endpoint, toggle, maxlen)
    uint32_t buffer;   // Buffer pointer (physical)
};

// Queue Head (8 bytes, 16-byte aligned)
struct uhci_qh {
    uint32_t link;
    uint32_t element;
};

// PID defines for USB tokens
#define USB_PID_SETUP  0x2D
#define USB_PID_IN     0x69
#define USB_PID_OUT    0xE1

// TD token field helpers
#define TD_TOKEN_DEVADDR(v)  (((uint32_t)(v) & 0x7F) << 8)
#define TD_TOKEN_EP(v)       (((uint32_t)(v) & 0x0F) << 15)
#define TD_TOKEN_TOGGLE(v)   (((uint32_t)(v) & 0x01) << 19)
#define TD_TOKEN_MAXLEN(v)   (((uint32_t)((v)-1) & 0x7FF) << 21)

// TD status bits
#define TD_CTRL_ACTIVE   (1 << 23)
#define TD_CTRL_IOC      (1 << 24)
#define TD_CTRL_LS       (1 << 26)
#define TD_CTRL_STALLED  (1 << 22)
#define TD_CTRL_NAK      (1 << 19)
#define TD_CTRL_TIMEOUT  (1 << 18)
#define TD_ACTLEN_MASK   0x7FF

// Link pointer types
// Bit 0 = Terminate (1 = end of list)
// Bit 1 = Type (0 = TD, 1 = QH)
// Bit 2 = Depth/Breadth (for TD links only)
#define LINK_TERM   0x0001
#define LINK_QH     0x0002
#define LINK_TD     0x0000  // No flags — not terminated, next is TD
// For TD link: address must have bits 2:0 = 0

// ─── Static Data ───

static uint16_t uhci_io_base = 0;
static int usb_irq = 0;
static int usb_initialized = 0;

// Frame list (4KB, must be 4KB aligned) — BSS in identity-mapped region
static uint32_t frame_list[FRAME_LIST_SIZE] __attribute__((aligned(4096)));

// Queue head (16-byte aligned)
static struct uhci_qh async_qh __attribute__((aligned(16)));

// Transfer descriptors pool (16-byte aligned each)
static struct uhci_td td_pool[MAX_TDS] __attribute__((aligned(16)));

// Data buffers for TDs

// Forward declarations
static int uhci_control_transfer(uint8_t dev_addr, uint8_t *setup_pkt, uint8_t *data_buf, int data_len, int is_in);

// Device info
static int device_count = 0;
static char device_names[MAX_USB_DEVICES][32];
static uint16_t device_vid[MAX_USB_DEVICES];
static uint16_t device_pid[MAX_USB_DEVICES];
static int device_class[MAX_USB_DEVICES];
static int device_subclass[MAX_USB_DEVICES];
static int device_ports[MAX_USB_DEVICES];
static uint8_t device_addr[MAX_USB_DEVICES];

// HID tablet state
static int hid_tablet_idx = -1;
static uint8_t hid_report_buf[16];

// USB device descriptor (18 bytes)
struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

// ─── Helper: wait for controller to halt ───

static int uhci_wait_halt(void) {
    for (int i = 0; i < 10000; i++) {
        if (inw(uhci_io_base + USBSTS) & STS_HCH) return 1;
    }
    return 0;
}

// ─── Helper: wait for controller to start ───

static int uhci_wait_start(void) {
    for (int i = 0; i < 10000; i++) {
        if (!(inw(uhci_io_base + USBSTS) & STS_HCH)) return 1;
    }
    return 0;
}

// ─── Helper: wait for TD completion ───

static int uhci_wait_td(volatile struct uhci_td *td, int timeout_ms) {
    for (int i = 0; i < timeout_ms * 100; i++) {
        if (!(td->status & TD_CTRL_ACTIVE)) {
            if (td->status & TD_CTRL_STALLED) return -1;
            if (td->status & TD_CTRL_TIMEOUT) return -2;
            return 0; // success
        }
        // Short delay
        for (volatile int d = 0; d < 100; d++);
    }
    return -3; // timeout
}

// ─── Initialize UHCI controller ───

static int uhci_init_controller(uint16_t io_base) {
    uhci_io_base = io_base;

    // 1. Stop the controller
    outw(uhci_io_base + USBCMD, 0);
    if (!uhci_wait_halt()) return 0;

    // 2. Host controller reset
    outw(uhci_io_base + USBCMD, CMD_HCRESET);
    for (volatile int d = 0; d < 10000; d++);
    // Wait for HCRESET to self-clear
    for (int i = 0; i < 10000; i++) {
        if (!(inw(uhci_io_base + USBCMD) & CMD_HCRESET)) break;
        for (volatile int d = 0; d < 100; d++);
    }
    outw(uhci_io_base + USBCMD, 0);
    if (!uhci_wait_halt()) return 0;

    // 3. Set frame length adjust
    outb(uhci_io_base + SOFMOD, 64);

    // 4. Point frame list to our allocated array — all entries point to async QH
    uintptr_t fl_phys = (uintptr_t)frame_list;
    async_qh.link = (uint32_t)(uintptr_t)&async_qh | LINK_QH; // Point to self (circular)
    async_qh.element = LINK_TERM; // Empty queue
    for (int i = 0; i < FRAME_LIST_SIZE; i++) {
        frame_list[i] = (uint32_t)(uintptr_t)&async_qh | LINK_QH;
    }
    outl(uhci_io_base + FLBASEADD, (uint32_t)fl_phys);

    // 6. Run the controller
    outw(uhci_io_base + USBCMD, CMD_RUN | CMD_MAXP); // Run with 64 byte packets
    if (!uhci_wait_start()) return 0;

    // 7. Set Configured Flag
    outw(uhci_io_base + USBCMD, inw(uhci_io_base + USBCMD) | CMD_CF);

    // 8. Enable interrupts
    outw(uhci_io_base + USBINTR, STS_USBINT | STS_PCD | STS_ERROR);

    return 1;
}

// ─── Schedule a control transfer on the async QH ───

static int uhci_control_transfer(uint8_t dev_addr, uint8_t *setup_pkt, uint8_t *data_buf, int data_len, int is_in) {
    int td_idx = 0;

    // Need: SETUP TD + (optional) DATA TD + STATUS TD
    if (td_idx + 3 > MAX_TDS) return -4;

    // Clear the TD pool area we'll use
    for (int i = 0; i < 3; i++) {
        td_pool[i].link = LINK_TERM;
        td_pool[i].status = 0;
        td_pool[i].token = 0;
        td_pool[i].buffer = 0;
    }

    // --- SETUP TD ---
    struct uhci_td *setup_td = &td_pool[td_idx++];
    setup_td->status = TD_CTRL_ACTIVE | TD_CTRL_IOC;
    setup_td->token = USB_PID_SETUP | TD_TOKEN_DEVADDR(dev_addr) | TD_TOKEN_TOGGLE(0) | TD_TOKEN_MAXLEN(8);
    setup_td->buffer = (uint32_t)(uintptr_t)setup_pkt;

    // --- DATA TD (if data_len > 0) ---
    struct uhci_td *data_td = NULL;
    if (data_len > 0) {
        data_td = &td_pool[td_idx++];
        data_td->status = TD_CTRL_ACTIVE | TD_CTRL_IOC;
        if (is_in) {
            data_td->token = USB_PID_IN | TD_TOKEN_DEVADDR(dev_addr) | TD_TOKEN_TOGGLE(1) | TD_TOKEN_MAXLEN(data_len);
        } else {
            data_td->token = USB_PID_OUT | TD_TOKEN_DEVADDR(dev_addr) | TD_TOKEN_TOGGLE(1) | TD_TOKEN_MAXLEN(data_len);
        }
        data_td->buffer = (uint32_t)(uintptr_t)data_buf;
        setup_td->link = (uint32_t)(uintptr_t)data_td | LINK_TD;
    }

    // --- STATUS TD ---
    struct uhci_td *status_td = &td_pool[td_idx++];
    status_td->status = TD_CTRL_ACTIVE;
    if (is_in) {
        // For IN transfer, status stage is OUT with toggle 1
        status_td->token = USB_PID_OUT | TD_TOKEN_DEVADDR(dev_addr) | TD_TOKEN_TOGGLE(1) | TD_TOKEN_MAXLEN(0);
    } else {
        // For OUT transfer, status stage is IN with toggle 1
        status_td->token = USB_PID_IN | TD_TOKEN_DEVADDR(dev_addr) | TD_TOKEN_TOGGLE(1) | TD_TOKEN_MAXLEN(0);
    }
    status_td->buffer = 0;
    if (data_td) {
        data_td->link = (uint32_t)(uintptr_t)status_td | LINK_TD;
    } else {
        setup_td->link = (uint32_t)(uintptr_t)status_td | LINK_TD;
    }

    // Attach TD chain to QH
    async_qh.element = (uint32_t)(uintptr_t)setup_td | LINK_TD;

    // Wait for all TDs to complete
    for (int i = 0; i < td_idx; i++) {
        int ret = uhci_wait_td(&td_pool[i], 1000);
        if (ret < 0) {
            async_qh.element = LINK_TERM; // Reset QH
            return ret;
        }
    }

    async_qh.element = LINK_TERM; // Reset QH
    return 0;
}

// ─── Set USB device address ───

static int uhci_set_address(uint8_t old_addr, uint8_t new_addr) {
    uint8_t setup[8] = {
        0x00, 0x05,  // bmRequestType=0x00 (host->device, standard, device), bRequest=SET_ADDRESS
        new_addr, 0x00, // wValue = new address
        0x00, 0x00,  // wIndex
        0x00, 0x00   // wLength
    };
    return uhci_control_transfer(old_addr, setup, NULL, 0, 0);
}

// ─── Get device descriptor ───

static int uhci_get_descriptor(uint8_t dev_addr, uint8_t desc_type, uint8_t desc_index, uint16_t lang, void *buf, int len) {
    uint8_t setup[8] = {
        0x80, 0x06,  // bmRequestType=device->host, bRequest=GET_DESCRIPTOR
        desc_type, desc_index, // wValue (high byte = type, low byte = index)
        lang & 0xFF, (lang >> 8) & 0xFF, // wIndex
        len & 0xFF, (len >> 8) & 0xFF    // wLength
    };
    return uhci_control_transfer(dev_addr, setup, (uint8_t*)buf, len, 1);
}

// ─── Reset and enable a USB port ───

static int uhci_port_reset(int port) {
    uint16_t base = uhci_io_base + PORTSC1 + (port - 1) * 2;
    uint16_t portsc;

    // Reset the port (set bit 9)
    outw(base, PORT_RESET);
    for (volatile int d = 0; d < 50000; d++);

    // Clear reset — QEMU starts 50ms timer, device reconnects when timer fires
    outw(base, 0);
    portsc = inw(base);
    if (!(portsc & PORT_CONN)) return 0;

    // Clear connect-status-change bit
    outw(base, portsc | PORT_CONNCHG);

    // Poll for PORT_ENABLE (wait for QEMU reset timer to fire)
    for (int i = 0; i < 50000; i++) {
        portsc = inw(base);
        if (portsc & PORT_ENABLE) break;
        for (volatile int d = 0; d < 50; d++);
    }

    portsc = inw(base);
    if (!(portsc & PORT_ENABLE)) return 0;

    return 1;
}

// ─── Enumerate device on a port ───

static int uhci_enumerate(int port) {
    uint16_t portsc = inw(uhci_io_base + PORTSC1 + (port - 1) * 2);
    if (!(portsc & PORT_CONN)) return 0;

    // Reset the port
    if (!uhci_port_reset(port)) return 0;

    // Get device descriptor (first 8 bytes to determine max packet size)
    struct usb_device_descriptor dd;
    uint8_t setup[8] = {
        0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00
    };
    int ret = uhci_control_transfer(0, setup, (uint8_t*)&dd, 8, 1);
    if (ret < 0) return 0;

    // Assign address 1
    ret = uhci_set_address(0, 1);
    if (ret < 0) return 0;

    // Read full device descriptor (18 bytes) from address 1
    ret = uhci_get_descriptor(1, 1, 0, 0, &dd, 18);
    if (ret < 0) return 0;

    if (device_count >= MAX_USB_DEVICES) return 1;

    int idx = device_count;
    device_vid[idx] = dd.idVendor;
    device_pid[idx] = dd.idProduct;
    device_class[idx] = dd.bDeviceClass;
    device_subclass[idx] = dd.bDeviceSubClass;
    device_ports[idx] = port;
    device_addr[idx] = 1;

    // Detect QEMU usb-tablet (VID 0x0627, PID 0x0001)
    if (dd.idVendor == 0x0627 && dd.idProduct == 0x0001) {
        hid_tablet_idx = idx;
    }

    // Build human-readable name
    char *name = device_names[idx];
    int pos = 0;

    // Class-based names
    if (dd.bDeviceClass == 0x00) {
        // Class info in interface descriptor - treat as "Generic"
        name[pos++] = 'G'; name[pos++] = 'e'; name[pos++] = 'n'; name[pos++] = 'e'; name[pos++] = 'r'; name[pos++] = 'i'; name[pos++] = 'c';
    } else if (dd.bDeviceClass == 0x02) { name[pos++] = 'N'; name[pos++] = 'e'; name[pos++] = 't'; name[pos++] = 'w'; name[pos++] = 'o'; name[pos++] = 'r'; name[pos++] = 'k'; }
    else if (dd.bDeviceClass == 0x03) { name[pos++] = 'H'; name[pos++] = 'I'; name[pos++] = 'D'; }
    else if (dd.bDeviceClass == 0x08) { name[pos++] = 'M'; name[pos++] = 'a'; name[pos++] = 's'; name[pos++] = 's'; name[pos++] = ' '; name[pos++] = 'S'; name[pos++] = 't'; name[pos++] = 'o'; name[pos++] = 'r'; name[pos++] = 'a'; name[pos++] = 'g'; name[pos++] = 'e'; }
    else if (dd.bDeviceClass == 0x09) { name[pos++] = 'H'; name[pos++] = 'u'; name[pos++] = 'b'; }
    else if (dd.bDeviceClass == 0xE0) { name[pos++] = 'W'; name[pos++] = 'i'; name[pos++] = 'r'; name[pos++] = 'e'; name[pos++] = 'l'; name[pos++] = 'e'; name[pos++] = 's'; name[pos++] = 's'; }
    else { name[pos++] = 'U'; name[pos++] = 'S'; name[pos++] = 'B'; name[pos++] = ' '; name[pos++] = 'D'; name[pos++] = 'e'; name[pos++] = 'v'; name[pos++] = 'i'; name[pos++] = 'c'; name[pos++] = 'e'; }

    // Add VID:PID
    name[pos++] = ' '; name[pos++] = '(';
    // VID hex
    char hext[] = "0123456789ABCDEF";
    name[pos++] = hext[(dd.idVendor >> 12) & 0xF];
    name[pos++] = hext[(dd.idVendor >> 8) & 0xF];
    name[pos++] = hext[(dd.idVendor >> 4) & 0xF];
    name[pos++] = hext[dd.idVendor & 0xF];
    name[pos++] = ':';
    name[pos++] = hext[(dd.idProduct >> 12) & 0xF];
    name[pos++] = hext[(dd.idProduct >> 8) & 0xF];
    name[pos++] = hext[(dd.idProduct >> 4) & 0xF];
    name[pos++] = hext[dd.idProduct & 0xF];
    name[pos++] = ')';
    name[pos++] = 0;

    device_count++;
    return 1;
}

// ─── Public API ───

int usb_init(void) {
    device_count = 0;
    uhci_io_base = 0;

    // Scan PCI bus 0 for UHCI controllers (class 0x0C, subclass 0x03, prog-if 0x00)
    // UHCI is often at function 2 of a multi-function device, so scan funcs 0-7
    for (int slot = 0; slot < 32; slot++) {
        for (int func = 0; func < 8; func++) {
            uint32_t vendor_device = pci_read_config(0, slot, func, 0);
            if (vendor_device == 0xFFFFFFFF) {
                if (func == 0) break; // Device not present at func 0, skip this slot
                continue;
            }

            uint32_t class_rev = pci_read_config(0, slot, func, 0x08);
            uint8_t class_code = (class_rev >> 24) & 0xFF;
            uint8_t subclass = (class_rev >> 16) & 0xFF;
            uint8_t prog_if = (class_rev >> 8) & 0xFF;

            if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x00) {
                // Found UHCI controller
                uint32_t bar4 = pci_read_config(0, slot, func, 0x20);
                uint16_t io_base = bar4 & 0xFFF0;

                if (io_base == 0) continue;

                // Get IRQ
                uint32_t intr = pci_read_config(0, slot, func, 0x3C);
                usb_irq = intr & 0xFF;

                // Enable bus mastering (bit 2) and I/O space (bit 0)
                uint32_t cmd = pci_read_config(0, slot, func, 0x04);
                cmd |= 0x05; // I/O space + bus master
                pci_write_config(0, slot, func, 0x04, cmd);

                if (uhci_init_controller(io_base)) {
                    usb_initialized = 1;

                    // Enumerate ports 1 and 2
                    uhci_enumerate(1);
                    uhci_enumerate(2);

                    return device_count;
                }
            }
        }
    }

    // If no UHCI found, try PCI function 2 of device 1 (standard PIIX4 location)
    uint32_t vendor_device = pci_read_config(0, 1, 2, 0);
    if (vendor_device != 0xFFFFFFFF) {
        uint32_t class_rev = pci_read_config(0, 1, 2, 0x08);
        uint8_t class_code = (class_rev >> 24) & 0xFF;
        uint8_t subclass = (class_rev >> 16) & 0xFF;

        if (class_code == 0x0C && subclass == 0x03) {
            uint32_t bar4 = pci_read_config(0, 1, 2, 0x20);
            uint16_t io_base = bar4 & 0xFFF0;
            if (io_base) {
                uint32_t cmd = pci_read_config(0, 1, 2, 0x04);
                cmd |= 0x05;
                pci_write_config(0, 1, 2, 0x04, cmd);
                if (uhci_init_controller(io_base)) {
                    usb_initialized = 1;
                    uhci_enumerate(1);
                    uhci_enumerate(2);
                    return device_count;
                }
            }
        }
    }

    return 0;
}

int usb_device_count(void) {
    return device_count;
}

const char* usb_device_name(int index) {
    if (index < 0 || index >= device_count) return "";
    return device_names[index];
}

int usb_device_vid(int index) {
    if (index < 0 || index >= device_count) return 0;
    return device_vid[index];
}

int usb_device_pid(int index) {
    if (index < 0 || index >= device_count) return 0;
    return device_pid[index];
}

int usb_poll(void) {
    if (!usb_initialized) return 0;

    // Check for port status changes
    for (int port = 1; port <= 2; port++) {
        uint16_t portsc = inw(uhci_io_base + PORTSC1 + (port - 1) * 2);
        if (portsc & PORT_CONNCHG) {
            // Clear the change bit
            outw(uhci_io_base + PORTSC1 + (port - 1) * 2, portsc | PORT_CONNCHG);
            return 1; // Tell caller something changed
        }
    }
    return 0;
}

// ─── USB HID Tablet Poll ───

int usb_hid_read(int *x, int *y, int *btn)
{
    if (hid_tablet_idx < 0) return 0;
    if (!usb_initialized) return 0;

    uint8_t dev = device_addr[hid_tablet_idx];

    // GET_REPORT (Input) setup packet
    uint8_t setup[8] = {
        0xA1, 0x01,  // bmRequestType, bRequest
        0x00, 0x01,  // wValue: Report ID=0, Report Type=Input
        0x00, 0x00,  // wIndex: Interface 0
        0x10, 0x00   // wLength: 16 bytes
    };

    int ret = uhci_control_transfer(dev, setup, hid_report_buf, 16, 1);
    if (ret < 0) return 0;

    // QEMU usb-tablet: byte0=buttons, byte1-2=X (le16), byte3-4=Y (le16)
    *btn = hid_report_buf[0] & 0x07;
    *x = hid_report_buf[1] | ((int)hid_report_buf[2] << 8);
    *y = hid_report_buf[3] | ((int)hid_report_buf[4] << 8);

    return 1;
}
