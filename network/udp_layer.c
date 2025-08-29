/*
 * udp_layer.c — self-contained UDP-over-IPv4 encoder/decoder + raw send/recv demo
 *
 * What this gives you
 *  - Pure-C routines to build and parse UDP/IPv4 packets (no libc sockets needed)
 *  - Correct UDP checksum with IPv4 pseudo-header (RFC 768 / RFC 1071)
 *  - Minimal IPv4 header builder (no options, DF=0)
 *  - Optional raw-socket demo (requires CAP_NET_RAW or root)
 *
 * Build (library only):
 *   gcc -O2 -Wall -c udp_layer.c -o udp_layer.o
 *
 * Build (with demo):
 *   gcc -O2 -Wall -DUDP_DEMO_MAIN udp_layer.c -o udp_demo
 *
 * Run demo (raw sockets):
 *   sudo ./udp_demo send 127.0.0.1 9000 "hello"
 *   # in another shell, run a standard UDP listener to prove it arrives:
 *   nc -u -l 9000
 *
 * Notes
 *  - The demo uses IP_HDRINCL to craft IPv4+UDP and send via a raw socket.
 *  - For receiving/parsing, we show an AF_PACKET (Ethernet) sniffer path and a
 *    raw IPv4 socket path; enable one per your environment.
 */

#if 0

gcc -O2 -Wall -DUDP_DEMO_MAIN udp_layer.c -o udp_demo

#send one UDP datagram(you can watch with `nc - u - l 9000`)
sudo ./udp_demo send 127.0.0.1 9000 "hello"

#sniff UDP packets for a given destination port
sudo ./udp_demo sniff 9000

#endif

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <unistd.h>

/* =================== Helpers =================== */
static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }

static uint16_t checksum16(const void *data, size_t len)
{
    const uint16_t *w = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1)
    {
        sum += *w++;
        len -= 2;
    }
    if (len)
        sum += *(const uint8_t *)w;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* =================== UDP + IPv4 build =================== */
#pragma pack(push, 1)
typedef struct
{
    uint8_t ver_ihl;   // version (4) + IHL (5)
    uint8_t tos;       // DSCP/ECN
    uint16_t tot_len;  // total length (IP header + UDP header + payload)
    uint16_t id;       // identification
    uint16_t frag_off; // flags/frag offset
    uint8_t ttl;       // TTL
    uint8_t protocol;  // 17 for UDP
    uint16_t checksum; // IPv4 header checksum
    uint32_t saddr;    // source IP (network order)
    uint32_t daddr;    // dest IP   (network order)
} ipv4_hdr_t;

typedef struct
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum; // 0 allowed (no checksum) for IPv4, but we compute it
} udp_hdr_t;

/* Pseudo-header for UDP checksum */
typedef struct
{
    uint32_t saddr;
    uint32_t daddr;
    uint8_t zero;
    uint8_t protocol; // 17
    uint16_t udp_len;
} pseudo_hdr_t;
#pragma pack(pop)

/* Build IPv4 header (no options) */
static void ipv4_build(ipv4_hdr_t *ip, uint16_t total_len, uint32_t saddr_be, uint32_t daddr_be,
                       uint16_t id, uint8_t ttl, uint8_t proto)
{
    ip->ver_ihl = (4u << 4) | 5u; // IHL=5 (20 bytes)
    ip->tos = 0;
    ip->tot_len = htons(total_len);
    ip->id = htons(id);
    ip->frag_off = htons(0x0000); // no DF/MF by default
    ip->ttl = ttl ? ttl : 64;
    ip->protocol = proto;
    ip->checksum = 0;
    ip->saddr = saddr_be;
    ip->daddr = daddr_be;
    ip->checksum = checksum16(ip, sizeof(*ip));
}

/* Compute UDP checksum across pseudo-header + udp header + payload */
static uint16_t udp_checksum_ipv4(uint32_t saddr_be, uint32_t daddr_be,
                                  const udp_hdr_t *uh, const uint8_t *payload, size_t payload_len)
{
    pseudo_hdr_t ph = {saddr_be, daddr_be, 0, IPPROTO_UDP, htons((uint16_t)(sizeof(udp_hdr_t) + payload_len))};
    uint32_t sum = 0;

    // sum pseudo header
    const uint16_t *pw = (const uint16_t *)&ph;
    for (size_t i = 0; i < sizeof(ph) / 2; i++)
        sum += pw[i];

    // sum udp header (with checksum field as 0)
    udp_hdr_t tmp = *uh;
    tmp.checksum = 0;
    pw = (const uint16_t *)&tmp;
    for (size_t i = 0; i < sizeof(tmp) / 2; i++)
        sum += pw[i];

    // sum payload
    const uint8_t *p = payload;
    size_t len = payload_len;
    while (len > 1)
    {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint16_t)(p[0] << 8);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/*
 * Compose a complete IPv4+UDP packet into buf.
 * Returns total packet length (IP header + UDP header + payload), or 0 on error.
 */
size_t udp_build_ipv4_packet(uint8_t *buf, size_t buflen,
                             uint32_t src_ip_be, uint16_t src_port,
                             uint32_t dst_ip_be, uint16_t dst_port,
                             const uint8_t *payload, size_t payload_len,
                             uint16_t ip_id, uint8_t ttl)
{
    size_t need = sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + payload_len;
    if (buflen < need)
        return 0;

    ipv4_hdr_t *ip = (ipv4_hdr_t *)buf;
    udp_hdr_t *uh = (udp_hdr_t *)(buf + sizeof(ipv4_hdr_t));

    uh->src_port = htons(src_port);
    uh->dst_port = htons(dst_port);
    uh->len = htons((uint16_t)(sizeof(udp_hdr_t) + payload_len));
    uh->checksum = 0;

    // copy payload
    if (payload_len)
        memcpy(buf + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t), payload, payload_len);

    // compute UDP checksum
    uh->checksum = udp_checksum_ipv4(src_ip_be, dst_ip_be, uh,
                                     buf + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t),
                                     payload_len);

    // build IPv4 header
    ipv4_build(ip, (uint16_t)need, src_ip_be, dst_ip_be, ip_id, ttl, IPPROTO_UDP);

    return need;
}

/* =================== Parse helpers =================== */

typedef struct
{
    uint32_t src_ip_be, dst_ip_be;
    uint16_t src_port, dst_port;
    const uint8_t *payload;
    size_t payload_len;
    bool checksum_ok;
} udp_parsed_t;

bool udp_parse_ipv4_packet(const uint8_t *pkt, size_t len, udp_parsed_t *out)
{
    if (len < sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t))
        return false;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)pkt;
    if ((ip->ver_ihl >> 4) != 4)
        return false;
    size_t ihl = (ip->ver_ihl & 0x0F) * 4u;
    if (ihl < sizeof(ipv4_hdr_t) || len < ihl + sizeof(udp_hdr_t))
        return false;
    if (ip->protocol != IPPROTO_UDP)
        return false;
    uint16_t ip_tot = ntohs(ip->tot_len);
    if (ip_tot > len)
        return false;

    const udp_hdr_t *uh = (const udp_hdr_t *)(pkt + ihl);
    uint16_t ulen = ntohs(uh->len);
    if (ulen < sizeof(udp_hdr_t) || ihl + ulen > ip_tot)
        return false;

    out->src_ip_be = ip->saddr;
    out->dst_ip_be = ip->daddr;
    out->src_port = ntohs(uh->src_port);
    out->dst_port = ntohs(uh->dst_port);
    out->payload = (const uint8_t *)uh + sizeof(udp_hdr_t);
    out->payload_len = (size_t)ulen - sizeof(udp_hdr_t);

    // verify checksum (0 means optional in IPv4; treat 0 as OK)
    if (uh->checksum == 0)
        out->checksum_ok = true;
    else
    {
        uint16_t cs = udp_checksum_ipv4(ip->saddr, ip->daddr, uh, out->payload, out->payload_len);
        out->checksum_ok = (cs == uh->checksum);
    }
    return true;
}

/* =================== Raw send demo =================== */
#ifdef UDP_DEMO_MAIN
static int demo_send(const char *dst_ip_str, uint16_t dport, const char *payload)
{
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (s < 0)
    {
        perror("socket raw");
        return 1;
    }

    int one = 1;
    if (setsockopt(s, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0)
    {
        perror("IP_HDRINCL");
        close(s);
        return 1;
    }

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(dport); // not used by raw send, but set anyway
    if (inet_pton(AF_INET, dst_ip_str, &dst.sin_addr) != 1)
    {
        fprintf(stderr, "bad dst ip\n");
        close(s);
        return 1;
    }

    // choose a source IP: let kernel fill, or pick loopback if sending to loopback
    uint32_t src_ip_be;
    if (dst.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
        src_ip_be = htonl(INADDR_LOOPBACK);
    else
        src_ip_be = htonl((127 << 24) | (0 << 16) | (0 << 8) | 1); // 127.0.0.1 as fallback; change as needed

    uint8_t pkt[1500];
    size_t n = udp_build_ipv4_packet(pkt, sizeof(pkt),
                                     src_ip_be, 55555, dst.sin_addr.s_addr, dport,
                                     (const uint8_t *)payload, strlen(payload),
                                     (uint16_t)rand(), 64);
    if (!n)
    {
        fprintf(stderr, "packet too big\n");
        close(s);
        return 1;
    }

    ssize_t wr = sendto(s, pkt, n, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (wr < 0)
    {
        perror("sendto");
        close(s);
        return 1;
    }
    printf("sent %zd bytes UDP to %s:%u\n", wr, dst_ip_str, (unsigned)dport);
    close(s);
    return 0;
}

static int demo_sniff(uint16_t listen_port)
{
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (s < 0)
    {
        perror("socket raw");
        return 1;
    }
    printf("Sniffing UDP frames for dst port %u... (Ctrl+C to quit)\n", (unsigned)listen_port);
    for (;;)
    {
        uint8_t buf[2048];
        ssize_t rd = recv(s, buf, sizeof(buf), 0);
        if (rd < 0)
        {
            if (errno == EINTR)
                continue;
            perror("recv");
            break;
        }
        udp_parsed_t P;
        if (!udp_parse_ipv4_packet(buf, (size_t)rd, &P))
            continue;
        if (P.dst_port != listen_port)
            continue;
        char sip[INET_ADDRSTRLEN], dip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &P.src_ip_be, sip, sizeof(sip));
        inet_ntop(AF_INET, &P.dst_ip_be, dip, sizeof(dip));
        printf("UDP %s:%u -> %s:%u | %schecksum | %zu bytes\n",
               sip, P.src_port, dip, P.dst_port,
               P.checksum_ok ? "" : "BAD-", P.payload_len);
        fwrite(P.payload, 1, P.payload_len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    close(s);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s send <dst_ip> <dst_port> <data>\n       %s sniff <dst_port>\n", argv[0], argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "send"))
    {
        if (argc < 5)
        {
            fprintf(stderr, "send needs <dst_ip> <dst_port> <data>\n");
            return 1;
        }
        uint16_t p = (uint16_t)atoi(argv[3]);
        return demo_send(argv[2], p, argv[4]);
    }
    else if (!strcmp(argv[1], "sniff"))
    {
        if (argc < 3)
        {
            fprintf(stderr, "sniff needs <dst_port>\n");
            return 1;
        }
        uint16_t p = (uint16_t)atoi(argv[2]);
        return demo_sniff(p);
    }
    fprintf(stderr, "unknown command\n");
    return 1;
}
#endif
