// sniff_raw.c - Minimal raw-socket packet sniffer for Linux (AF_PACKET)
// Build: gcc -O2 -Wall -Wextra -o sniff_raw sniff_raw.c
// Usage: sudo ./sniff_raw [-i iface] [-p] [-x] [-n count]
//
//  -i iface   Interface name (e.g., eth0). If omitted, receives from all.
//  -p         Enable promiscuous mode (requires -i).
//  -x         Hex dump packet payload.
//  -n count   Stop after capturing 'count' packets.
//
// Notes:
//  * Requires root privileges.
//  * Linux only (uses PF_PACKET / SOCK_RAW / ETH_P_ALL).
//  * Promiscuous mode is attached as a membership; it’s removed when the socket closes.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/if.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void hex_dump(const unsigned char *buf, size_t len)
{
    const size_t cols = 16;
    for (size_t i = 0; i < len; i += cols)
    {
        printf("%04zx  ", i);
        for (size_t j = 0; j < cols; ++j)
        {
            if (i + j < len)
                printf("%02x ", buf[i + j]);
            else
                printf("   ");
        }
        printf(" ");
        for (size_t j = 0; j < cols; ++j)
        {
            if (i + j < len)
            {
                unsigned char c = buf[i + j];
                printf("%c", (c >= 32 && c <= 126) ? c : '.');
            }
        }
        printf("\n");
    }
}

static void print_ip_port_proto(const unsigned char *pkt, size_t len, uint16_t ethertype)
{
    if (ethertype == ETH_P_IP)
    {
        if (len < 14 + 20)
            return; // Ethernet + min IPv4
        const unsigned char *ip = pkt + 14;
        unsigned ihl = (ip[0] & 0x0F) * 4U;
        if (ihl < 20 || 14 + ihl > len)
            return;
        uint8_t proto = ip[9];
        char src[16], dst[16];
        snprintf(src, sizeof(src), "%u.%u.%u.%u", ip[12], ip[13], ip[14], ip[15]);
        snprintf(dst, sizeof(dst), "%u.%u.%u.%u", ip[16], ip[17], ip[18], ip[19]);

        const unsigned char *l4 = ip + ihl;
        if (proto == IPPROTO_TCP || proto == IPPROTO_UDP)
        {
            if ((size_t)(l4 - pkt + 4) > len)
            {
                printf(" IPv4 %s -> %s proto=%u\n", src, dst, proto);
                return;
            }
            uint16_t sport = (uint16_t)(l4[0] << 8 | l4[1]);
            uint16_t dport = (uint16_t)(l4[2] << 8 | l4[3]);
            printf(" IPv4 %s:%u -> %s:%u %s\n",
                   src, ntohs(*(uint16_t *)&l4[0]),
                   dst, ntohs(*(uint16_t *)&l4[2]),
                   (proto == IPPROTO_TCP) ? "TCP" : "UDP");
        }
        else
        {
            printf(" IPv4 %s -> %s proto=%u\n", src, dst, proto);
        }
    }
    else if (ethertype == ETH_P_IPV6)
    {
        if (len < 14 + 40)
            return;
        const unsigned char *ip6 = pkt + 14;
        uint8_t next = ip6[6];
        char src[40], dst[40];
        struct in6_addr s6, d6;
        memcpy(&s6, ip6 + 8, 16);
        memcpy(&d6, ip6 + 24, 16);
        inet_ntop(AF_INET6, &s6, src, sizeof(src));
        inet_ntop(AF_INET6, &d6, dst, sizeof(dst));
        printf(" IPv6 %s -> %s next=%u\n", src, dst, next);
    }
    else
    {
        // Non-IP (ARP, LLDP, etc.)
    }
}

static void print_eth_header(const unsigned char *pkt, size_t len, uint16_t *out_ethertype)
{
    if (len < 14)
        return;
    const unsigned char *d = pkt + 0;
    const unsigned char *s = pkt + 6;
    uint16_t type = (uint16_t)(pkt[12] << 8 | pkt[13]);
    *out_ethertype = type;

    printf(" ETH %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x type=0x%04x",
           s[0], s[1], s[2], s[3], s[4], s[5],
           d[0], d[1], d[2], d[3], d[4], d[5],
           type);
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    bool promiscuous = false;
    bool do_hex = false;
    long limit = -1;

    int opt;
    while ((opt = getopt(argc, argv, "i:pxn:")) != -1)
    {
        switch (opt)
        {
        case 'i':
            ifname = optarg;
            break;
        case 'p':
            promiscuous = true;
            break;
        case 'x':
            do_hex = true;
            break;
        case 'n':
            limit = strtol(optarg, NULL, 10);
            break;
        default:
            fprintf(stderr, "Usage: %s [-i iface] [-p] [-x] [-n count]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGINT, on_sigint);

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0)
    {
        perror("socket(AF_PACKET,SOCK_RAW)");
        return 1;
    }

    // Optional: bind to an interface
    int ifindex = 0;
    if (ifname && *ifname)
    {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
        if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
        {
            perror("ioctl(SIOCGIFINDEX)");
            close(fd);
            return 1;
        }
        ifindex = ifr.ifr_ifindex;

        struct sockaddr_ll sll;
        memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = htons(ETH_P_ALL);
        sll.sll_ifindex = ifindex;
        if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0)
        {
            perror("bind(AF_PACKET)");
            close(fd);
            return 1;
        }
    }

    // Optional: enable promiscuous mode (requires interface)
    struct packet_mreq mr;
    bool promisc_added = false;
    if (promiscuous)
    {
        if (ifindex == 0)
        {
            fprintf(stderr, "[-p] requires -i <iface>\n");
            close(fd);
            return 1;
        }
        memset(&mr, 0, sizeof(mr));
        mr.mr_ifindex = ifindex;
        mr.mr_type = PACKET_MR_PROMISC;
        if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0)
        {
            perror("setsockopt(PACKET_ADD_MEMBERSHIP, PROMISC)");
            // continue even if it fails
        }
        else
        {
            promisc_added = true;
        }
    }

    printf("Sniffing %s (promisc=%s). Press Ctrl+C to stop.\n",
           ifname ? ifname : "all interfaces",
           promisc_added ? "on" : "off");

    unsigned char buf[65536];
    long count = 0;

    while (!g_stop && (limit < 0 || count < limit))
    {
        struct sockaddr_ll from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0)
        {
            if (errno == EINTR)
                break;
            perror("recvfrom");
            continue;
        }

        // Timestamp
        struct timeval tv;
        gettimeofday(&tv, NULL);

        char iname[IFNAMSIZ] = {0};
        if_indextoname(from.sll_ifindex, iname);

        // Decode Ethernet
        uint16_t ethertype = 0;
        printf("[%ld.%06ld] if=%s len=%zd",
               (long)tv.tv_sec, (long)tv.tv_usec, iname[0] ? iname : "?", n);
        print_eth_header(buf, (size_t)n, &ethertype);
        print_ip_port_proto(buf, (size_t)n, ntohs(ethertype));
        printf("\n");

        if (do_hex)
        {
            hex_dump(buf, (size_t)n);
            printf("\n");
        }

        ++count;
    }

    if (promisc_added)
    {
        // best-effort removal (kernel should remove on close anyway)
        setsockopt(fd, SOL_PACKET, PACKET_DROP_MEMBERSHIP, &mr, sizeof(mr));
    }
    close(fd);
    printf("Captured %ld packet(s). Bye.\n", count);
    return 0;
}
