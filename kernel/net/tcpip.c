#include "../lib/framebuffer.h"

typedef struct {
    unsigned char dst_mac[6];
    unsigned char src_mac[6];
    unsigned short ethertype;
} eth_header_t;

typedef struct {
    unsigned char ver_ihl;
    unsigned char tos;
    unsigned short len;
    unsigned short id;
    unsigned short flags;
    unsigned char ttl;
    unsigned char proto;
    unsigned short checksum;
    unsigned int src;
    unsigned int dst;
} ip_header_t;

void net_send_arp(void)
{
    fb_write("Invio ARP...\n");
}

void net_send_ping(unsigned int ip)
{
    (void)ip;
    fb_write("Invio ping...\n");
}
