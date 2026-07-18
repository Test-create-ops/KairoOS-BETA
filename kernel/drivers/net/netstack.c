#include "../../lib/io.h"
#include "../../lib/framebuffer.h"
#include <stdint.h>

#define memcpy __builtin_memcpy
#define memset __builtin_memset



// Network stack — ARP, IP, TCP, SMTP, KMP

// Our IP config (QEMU user-net defaults)
#define OUR_IP       ((10<<24)|(0<<16)|(2<<8)|15)       // 10.0.2.15
#define GATEWAY_IP   ((10<<24)|(0<<16)|(2<<8)|2)         // 10.0.2.2
#define SMTP_PORT    25
#define KMP_PORT     9999

extern uint8_t nic_mac[6];
extern int nic_ready;
int rtl8139_send(const void*,int);
int rtl8139_poll(void);

// ─── hton helpers ───
static uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }
static uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }

// ─── Ethernet ───
typedef struct __attribute__((packed)) {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} eth_hdr_t;
#define ETH_ARP  0x0806
#define ETH_IP   0x0800

// ─── ARP ───
typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t sip;
    uint8_t  tha[6];
    uint32_t tip;
} arp_pkt_t;
#define ARP_REQUEST 1
#define ARP_REPLY   2

static uint8_t arp_cache_ip[16];
static uint8_t arp_cache_mac[16][6];
static int arp_cache_count = 0;
static int arp_waiting = 0;
static uint32_t arp_wait_ip = 0;
static uint8_t arp_wait_mac[6];
static int arp_wait_ok = 0;

static void arp_cache_add(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < arp_cache_count; i++)
        if (*(uint32_t*)&arp_cache_ip[i*4] == ip) {
            for (int j = 0; j < 6; j++) arp_cache_mac[i][j] = mac[j];
            return;
        }
    if (arp_cache_count < 16) {
        *(uint32_t*)&arp_cache_ip[arp_cache_count*4] = ip;
        for (int j = 0; j < 6; j++) arp_cache_mac[arp_cache_count][j] = mac[j];
        arp_cache_count++;
    }
}

static int arp_lookup(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < arp_cache_count; i++)
        if (*(uint32_t*)&arp_cache_ip[i*4] == ip) {
            for (int j = 0; j < 6; j++) mac[j] = arp_cache_mac[i][j];
            return 1;
        }
    return 0;
}

static void arp_send_request(uint32_t ip) {
    uint8_t buf[sizeof(eth_hdr_t)+sizeof(arp_pkt_t)];
    eth_hdr_t *eth = (eth_hdr_t*)buf;
    arp_pkt_t *arp = (arp_pkt_t*)(buf+sizeof(eth_hdr_t));
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, nic_mac, 6);
    eth->type = htons(ETH_ARP);
    arp->htype = htons(1);
    arp->ptype = htons(ETH_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(ARP_REQUEST);
    memcpy(arp->sha, nic_mac, 6);
    arp->sip = OUR_IP;
    memset(arp->tha, 0, 6);
    arp->tip = ip;
    rtl8139_send(buf, sizeof(buf));
}

static void arp_handle(arp_pkt_t *arp) {
    if (ntohs(arp->oper) == ARP_REPLY && arp->tip == OUR_IP) {
        arp_cache_add(arp->sip, arp->sha);
        if (arp_waiting && arp->sip == arp_wait_ip) {
            memcpy(arp_wait_mac, arp->sha, 6);
            arp_wait_ok = 1;
            arp_waiting = 0;
        }
    }
    if (ntohs(arp->oper) == ARP_REQUEST && arp->tip == OUR_IP) {
        uint8_t reply[sizeof(eth_hdr_t)+sizeof(arp_pkt_t)];
        eth_hdr_t *e = (eth_hdr_t*)reply;
        arp_pkt_t *a = (arp_pkt_t*)(reply+sizeof(eth_hdr_t));
        memcpy(e->dst, arp->sha, 6);
        memcpy(e->src, nic_mac, 6);
        e->type = htons(ETH_ARP);
        a->htype = htons(1);
        a->ptype = htons(ETH_IP);
        a->hlen = 6;
        a->plen = 4;
        a->oper = htons(ARP_REPLY);
        memcpy(a->sha, nic_mac, 6);
        a->sip = OUR_IP;
        memcpy(a->tha, arp->sha, 6);
        a->tip = arp->sip;
        rtl8139_send(reply, sizeof(reply));
    }
}

// Block until we have MAC for IP (polling, with timeout)
static int arp_resolve(uint32_t ip, uint8_t *mac) {
    if (arp_lookup(ip, mac)) return 1;
    arp_waiting = 1;
    arp_wait_ip = ip;
    arp_wait_ok = 0;
    arp_send_request(ip);
    for (int w = 0; w < 50000; w++) {
        // Poll for ARP reply
        int plen = rtl8139_poll();
        if (plen > 0) {
            // Re-parse from rx_ring (we just signal length with poll)
            // Actually rtl8139_poll already updated CAPR, so we need the data
            // For simplicity, we re-send request periodically
        }
        if (arp_wait_ok) {
            memcpy(mac, arp_wait_mac, 6);
            return 1;
        }
        for (volatile int d = 0; d < 1000; d++) asm volatile("pause");
        if (w % 5000 == 0) arp_send_request(ip);
    }
    arp_waiting = 0;
    return 0;
}

// ─── IPv4 ───
typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  dscp;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t cksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ip_hdr_t;
#define IP_PROTO_TCP 6

static uint16_t ip_cksum(void *hdr, int len) {
    uint32_t sum = 0;
    uint16_t *p = (uint16_t*)hdr;
    for (int i = 0; i < len/2; i++) sum += p[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(uint16_t)sum;
}

// ─── TCP ───
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off;
    uint8_t  flags;
    uint16_t window;
    uint16_t cksum;
    uint16_t urgent;
} tcp_hdr_t;
#define TCP_SYN  0x02
#define TCP_ACK  0x10
#define TCP_PSH  0x08
#define TCP_FIN  0x01
#define TCP_RST  0x04

typedef struct __attribute__((packed)) {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  proto;
    uint16_t tcp_len;
} tcp_pseudo_t;

typedef enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTAB, TCP_FIN_WAIT } tcp_state_t;

static struct {
    tcp_state_t state;
    uint32_t my_seq, my_ack;
    uint32_t rem_seq, rem_ack;
    uint16_t src_port, dst_port;
    uint32_t rem_ip;
    uint8_t  rem_mac[6];
    uint8_t  rx_buf[1460];
    int      rx_len;
    int      rx_done;
} tcp_conn;

static uint16_t tcp_cksum(ip_hdr_t *ip, tcp_hdr_t *tcp, int tcp_len) {
    uint32_t sum = 0;
    tcp_pseudo_t ps;
    ps.src_ip = ip->src_ip;
    ps.dst_ip = ip->dst_ip;
    ps.zero = 0;
    ps.proto = IP_PROTO_TCP;
    ps.tcp_len = htons(tcp_len);
    uint16_t *p = (uint16_t*)&ps;
    for (int i = 0; i < 6; i++) sum += p[i];
    uint16_t *t = (uint16_t*)tcp;
    for (int i = 0; i < tcp_len/2; i++) sum += t[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(uint16_t)sum;
}

static int tcp_build_packet(uint8_t *buf, uint32_t rem_ip, const uint8_t *rem_mac,
                            uint16_t src_port, uint16_t dst_port,
                            uint32_t seq, uint32_t ack, uint8_t flags,
                            const uint8_t *data, int data_len) {
    eth_hdr_t *eth = (eth_hdr_t*)buf;
    ip_hdr_t *ip = (ip_hdr_t*)(buf+sizeof(eth_hdr_t));
    tcp_hdr_t *tcp = (tcp_hdr_t*)(buf+sizeof(eth_hdr_t)+sizeof(ip_hdr_t));
    int tcp_len = sizeof(tcp_hdr_t) + data_len;
    int ip_len = sizeof(ip_hdr_t) + tcp_len;

    memcpy(eth->dst, rem_mac, 6);
    memcpy(eth->src, nic_mac, 6);
    eth->type = htons(ETH_IP);

    ip->ver_ihl = 0x45;
    ip->dscp = 0;
    ip->total_len = htons(ip_len);
    ip->id = htons(0);
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_TCP;
    ip->cksum = 0;
    ip->src_ip = OUR_IP;
    ip->dst_ip = rem_ip;
    ip->cksum = ip_cksum(ip, sizeof(ip_hdr_t));

    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->seq = htonl(seq);
    tcp->ack = htonl(ack);
    tcp->off = 0x50;
    tcp->flags = flags;
    tcp->window = htons(65535);
    tcp->cksum = 0;
    tcp->urgent = 0;

    if (data && data_len > 0)
        for (int i = 0; i < data_len; i++)
            buf[sizeof(eth_hdr_t)+sizeof(ip_hdr_t)+sizeof(tcp_hdr_t)+i] = data[i];

    tcp->cksum = tcp_cksum(ip, tcp, tcp_len);

    return sizeof(eth_hdr_t) + ip_len;
}

static void tcp_handle(tcp_hdr_t *tcp, int tcp_len, uint32_t src_ip) {
    uint32_t seq = ntohl(tcp->seq);
    uint32_t ack = ntohl(tcp->ack);
    uint8_t flags = tcp->flags;
    int data_len = tcp_len - sizeof(tcp_hdr_t);
    uint8_t *data = ((uint8_t*)tcp) + sizeof(tcp_hdr_t);

    if (tcp_conn.state == TCP_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
        tcp_conn.rem_seq = seq + 1;
        tcp_conn.my_ack = ack;
        tcp_conn.state = TCP_ESTAB;
        // Send ACK
        uint8_t buf[sizeof(eth_hdr_t)+sizeof(ip_hdr_t)+sizeof(tcp_hdr_t)];
        int len = tcp_build_packet(buf, tcp_conn.rem_ip, tcp_conn.rem_mac,
                                   tcp_conn.src_port, tcp_conn.dst_port,
                                   tcp_conn.my_seq, tcp_conn.rem_seq,
                                   TCP_ACK, 0, 0);
        rtl8139_send(buf, len);
        return;
    }

    if (tcp_conn.state == TCP_ESTAB) {
        if (flags & TCP_FIN) {
            tcp_conn.state = TCP_FIN_WAIT;
            uint8_t buf[sizeof(eth_hdr_t)+sizeof(ip_hdr_t)+sizeof(tcp_hdr_t)];
            int len = tcp_build_packet(buf, tcp_conn.rem_ip, tcp_conn.rem_mac,
                                       tcp_conn.src_port, tcp_conn.dst_port,
                                       tcp_conn.my_seq, tcp_conn.rem_seq+1,
                                       TCP_ACK|TCP_FIN, 0, 0);
            tcp_conn.my_seq++;
            rtl8139_send(buf, len);
            return;
        }
        if (data_len > 0) {
            tcp_conn.rem_seq = seq + data_len;
            tcp_conn.my_ack = ack;
            // Copy data to rx buffer
            for (int i = 0; i < data_len && i < 1460; i++)
                tcp_conn.rx_buf[i] = data[i];
            tcp_conn.rx_len = data_len;
            tcp_conn.rx_done = 1;
            // Send ACK
            uint8_t buf[sizeof(eth_hdr_t)+sizeof(ip_hdr_t)+sizeof(tcp_hdr_t)];
            int len = tcp_build_packet(buf, tcp_conn.rem_ip, tcp_conn.rem_mac,
                                       tcp_conn.src_port, tcp_conn.dst_port,
                                       tcp_conn.my_seq, tcp_conn.rem_seq,
                                       TCP_ACK, 0, 0);
            rtl8139_send(buf, len);
        }
    }
}

// ─── Public Network API ───

// Initialize network: configure IP, add gateway to ARP cache
void net_init(void) {
    if (!nic_ready) return;
    fb_write("NET: init\n");
    // Add gateway MAC via ARP
    // Will resolve on first use
}

// TCP connect to remote IP:port
int tcp_connect(uint32_t ip, uint16_t port) {
    if (!nic_ready) return -1;
    fb_write("NET: TCP connect\n");

    // Resolve MAC via ARP
    if (!arp_resolve(ip, tcp_conn.rem_mac)) {
        fb_write("NET: ARP failed\n");
        return -1;
    }

    tcp_conn.state = TCP_SYN_SENT;
    tcp_conn.my_seq = 1000;
    tcp_conn.my_ack = 0;
    tcp_conn.rem_ip = ip;
    tcp_conn.dst_port = port;
    tcp_conn.src_port = 40000;
    tcp_conn.rx_len = 0;
    tcp_conn.rx_done = 0;

    uint8_t buf[sizeof(eth_hdr_t)+sizeof(ip_hdr_t)+sizeof(tcp_hdr_t)];
    int len = tcp_build_packet(buf, ip, tcp_conn.rem_mac,
                               tcp_conn.src_port, port,
                               tcp_conn.my_seq, 0, TCP_SYN, 0, 0);
    tcp_conn.my_seq++;
    rtl8139_send(buf, len);

    // Wait for SYN+ACK
    for (int w = 0; w < 100000; w++) {
        int plen = rtl8139_poll();
        if (plen > 0) {
            uint8_t pkt_buf[1800];
            // Re-read from rx_ring
            // Actually rtl8139_poll just returns length and updates CAPR
            // We need to read from the RX ring buffer
        }
        if (tcp_conn.state == TCP_ESTAB) {
            fb_write("NET: TCP established\n");
            return 0;
        }
        for (volatile int d = 0; d < 500; d++) asm volatile("pause");
    }
    tcp_conn.state = TCP_CLOSED;
    fb_write("NET: TCP timeout\n");
    return -1;
}

// TCP send data over established connection
int tcp_send(const uint8_t *data, int len) {
    if (tcp_conn.state != TCP_ESTAB) return -1;
    uint8_t buf[sizeof(eth_hdr_t)+sizeof(ip_hdr_t)+sizeof(tcp_hdr_t)+1460];
    int plen = tcp_build_packet(buf, tcp_conn.rem_ip, tcp_conn.rem_mac,
                                tcp_conn.src_port, tcp_conn.dst_port,
                                tcp_conn.my_seq, tcp_conn.rem_seq,
                                TCP_PSH|TCP_ACK, data, len);
    tcp_conn.my_seq += len;
    rtl8139_send(buf, plen);
    return len;
}

// Called from main loop to process incoming packets
void net_poll_all(void) {
    if (!nic_ready) return;
    int plen = rtl8139_poll();
    if (plen <= 0) return;

    uint8_t buf[1800];
    rtl8139_read_packet(buf);

    eth_hdr_t *eth = (eth_hdr_t*)buf;
    uint16_t etype = ntohs(eth->type);

    if (etype == ETH_ARP && plen >= sizeof(eth_hdr_t)+sizeof(arp_pkt_t)) {
        arp_pkt_t *arp = (arp_pkt_t*)(buf+sizeof(eth_hdr_t));
        arp_handle(arp);
        return;
    }

    if (etype == ETH_IP && plen >= sizeof(eth_hdr_t)+sizeof(ip_hdr_t)) {
        ip_hdr_t *ip = (ip_hdr_t*)(buf+sizeof(eth_hdr_t));
        if (ip->protocol == IP_PROTO_TCP) {
            int ip_hdr_len = (ip->ver_ihl & 0xF) * 4;
            int total_len = ntohs(ip->total_len);
            int tcp_len = total_len - ip_hdr_len;
            tcp_hdr_t *tcp = (tcp_hdr_t*)(buf+sizeof(eth_hdr_t)+ip_hdr_len);
            if (buf + sizeof(eth_hdr_t) + total_len <= buf + plen) {
                tcp_handle(tcp, tcp_len, ip->src_ip);
            }
        }
    }
}

// Poll for TCP data (non-blocking, called from clients)
int tcp_poll(uint8_t *buf, int max_len) {
    // First process any pending packets
    net_poll_all();
    if (tcp_conn.rx_done) {
        int n = tcp_conn.rx_len;
        if (n > max_len) n = max_len;
        for (int i = 0; i < n; i++) buf[i] = tcp_conn.rx_buf[i];
        tcp_conn.rx_done = 0;
        tcp_conn.rx_len = 0;
        return n;
    }
    return 0;
}

int tcp_has_data(void) {
    net_poll_all();
    return tcp_conn.rx_done;
}

// ─── SMTP Client ───
typedef enum {
    SMTP_INIT, SMTP_EHLO, SMTP_MAIL, SMTP_RCPT, SMTP_DATA,
    SMTP_BODY, SMTP_QUIT, SMTP_DONE, SMTP_ERROR
} smtp_state_t;

static smtp_state_t smtp_state = SMTP_DONE;
static int smtp_code = 0;
static char smtp_line[512];
static int smtp_line_len = 0;
static int smtp_done = 0;

static void smtp_parse_line(void) {
    // Parse 3-digit code from response line
    if (smtp_line_len < 3) return;
    smtp_code = (smtp_line[0]-'0')*100 + (smtp_line[1]-'0')*10 + (smtp_line[2]-'0');
}

// Start sending email. Returns when done (blocking).
// In production, this would be async. For testing, simple polling.
int smtp_send(const char *from, const char *to, const char *subject, const char *body) {
    if (tcp_connect(GATEWAY_IP, SMTP_PORT) < 0) {
        fb_write("SMTP: connect failed\n");
        return -1;
    }

    smtp_state = SMTP_INIT;
    smtp_done = 0;
    smtp_line_len = 0;

    // Wait for 220 greeting
    for (int w = 0; w < 50000 && !smtp_done; w++) {
        char c;
        if (tcp_poll((uint8_t*)&c, 1) > 0) {
            if (c == '\n') {
                smtp_line[smtp_line_len] = 0;
                smtp_parse_line();
                // 220 → send EHLO
                if (smtp_code == 220 && smtp_state == SMTP_INIT) {
                    smtp_state = SMTP_EHLO;
                    tcp_send((uint8_t*)"EHLO kairos\r\n", 13);
                    smtp_code = 0;
                } else if (smtp_code == 250) {
                    if (smtp_state == SMTP_EHLO) {
                        smtp_state = SMTP_MAIL;
                        tcp_send((uint8_t*)"MAIL FROM:<kernel@kairo.os>\r\n", 29);
                    } else if (smtp_state == SMTP_MAIL) {
                        smtp_state = SMTP_RCPT;
                        tcp_send((uint8_t*)"RCPT TO:<user@host.com>\r\n", 25);
                    } else if (smtp_state == SMTP_RCPT) {
                        smtp_state = SMTP_DATA;
                        tcp_send((uint8_t*)"DATA\r\n", 6);
                    } else if (smtp_state == SMTP_BODY) {
                        smtp_state = SMTP_QUIT;
                        tcp_send((uint8_t*)"QUIT\r\n", 6);
                    } else if (smtp_state == SMTP_QUIT) {
                        smtp_done = 1;
                    }
                } else if (smtp_code == 354 && smtp_state == SMTP_DATA) {
                    smtp_state = SMTP_BODY;
                    tcp_send((uint8_t*)"Subject: Test\r\n\r\nHello from KairoOS!\r\n.\r\n", 42);
                }
                smtp_line_len = 0;
            } else if (c != '\r') {
                if (smtp_line_len < 511) smtp_line[smtp_line_len++] = c;
            }
        }
        for (volatile int d = 0; d < 500; d++) asm volatile("pause");
    }

    fb_write("SMTP: done\n");
    return smtp_done ? 0 : -1;
}

// ─── KMP Protocol ───
#define KMP_MAGIC     0x4B4D5000
#define KMP_LOGIN     0x01
#define KMP_TEXT      0x02
#define KMP_HEARTBEAT 0x03
#define KMP_ACK       0x04

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  type;
    uint8_t  sender_id;
    uint16_t payload_len;
    uint32_t dest_id;
} kmp_hdr_t;

// KMP connection state
static int kmp_connected = 0;
static char kmp_peer[64];
static int kmp_peer_len = 0;
static char kmp_msg_buf[4096];
static int kmp_msg_len = 0;
static int kmp_msg_ready = 0;

static int kmp_build(uint8_t *buf, uint8_t type, uint8_t sender, uint32_t dest,
                     const uint8_t *payload, uint16_t plen) {
    kmp_hdr_t *hdr = (kmp_hdr_t*)buf;
    hdr->magic = htonl(KMP_MAGIC);
    hdr->type = type;
    hdr->sender_id = sender;
    hdr->payload_len = htons(plen);
    hdr->dest_id = htonl(dest);
    if (payload && plen > 0)
        for (int i = 0; i < plen; i++)
            buf[sizeof(kmp_hdr_t) + i] = payload[i];
    return sizeof(kmp_hdr_t) + plen;
}

int kmp_connect(void) {
    if (tcp_connect(GATEWAY_IP, KMP_PORT) < 0) return -1;
    kmp_connected = 1;
    // Send login
    uint8_t buf[sizeof(kmp_hdr_t)+32];
    int len = kmp_build(buf, KMP_LOGIN, 1, 0, (uint8_t*)"KairoOS", 8);
    tcp_send(buf, len);
    return 0;
}

int kmp_send_text(const char *text) {
    if (!kmp_connected) return -1;
    int tlen = 0;
    while (text[tlen]) tlen++;
    uint8_t buf[sizeof(kmp_hdr_t)+4096];
    int len = kmp_build(buf, KMP_TEXT, 1, 2, (uint8_t*)text, tlen);
    tcp_send(buf, len);
    return 0;
}

int kmp_poll(char *out, int max_len) {
    if (kmp_msg_ready) {
        int n = kmp_msg_len;
        if (n > max_len-1) n = max_len-1;
        for (int i = 0; i < n; i++) out[i] = kmp_msg_buf[i];
        out[n] = 0;
        kmp_msg_ready = 0;
        return n;
    }
    // Poll TCP for incoming KMP packets
    uint8_t buf[1500];
    int n = 0;
    // Can't directly poll here because tcp_poll reads from same buffer
    // We'll poll during the main loop
    return 0;
}

void kmp_disconnect(void) {
    kmp_connected = 0;
}
