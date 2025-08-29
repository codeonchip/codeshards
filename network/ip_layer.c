/*
 * ip_layer.c — a IPv4 layer in C (header build/parse, checksum, fragmentation)
 *
 * What this provides
 *  - IPv4 header struct (no options) + parser/validator
 *  - Header checksum (RFC 791 / RFC 1071)
 *  - Packet builder for arbitrary protocol numbers
 *  - Simple fragmentation helper that emits fragments via a user callback
 *  - Optional demo main that crafts and sends a raw IPv4 packet (requires CAP_NET_RAW)
 *
 * Non-goals (kept small on purpose)
 *  - Full options support (IHL > 5) — parser rejects options by default
 *  - Reassembly cache (OS job); we only expose flags/offset
 *
 * Build (library):
 *   gcc -O2 -Wall -c ip_layer.c -o ip_layer.o
 *
 * Build (with demo):
 *   gcc -O2 -Wall -DIP_DEMO_MAIN ip_layer.c -o ip_demo
 *
 * Demo usage:
 *   sudo ./ip_demo send 127.0.0.1 253 "hello-ip"   # protocol 253 (experimental)
 */

#if 0
gcc -O2 -Wall -DIP_DEMO_MAIN ip_layer.c -o ip_demo
sudo ./ip_demo send 127.0.0.1 253 "hello-ip"
#endif

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <unistd.h>

/* =================== IPv4 header (no options) =================== */
#pragma pack(push, 1)
typedef struct
{
    uint8_t ver_ihl;   // version (4) + IHL (5)
    uint8_t tos;       // DSCP/ECN
    uint16_t tot_len;  // total length (header + payload)
    uint16_t id;       // identification
    uint16_t frag_off; // flags/fragment offset
    uint8_t ttl;       // time to live
    uint8_t protocol;  // e.g., 1=ICMP, 6=TCP, 17=UDP
    uint16_t checksum; // header checksum
    uint32_t saddr;    // source IP (network order)
    uint32_t daddr;    // dest IP   (network order)
} ipv4_hdr_t;
#pragma pack(pop)

/* Helpers for flags/offset */
#define IPV4_FLAG_RESERVED 0x8000
#define IPV4_FLAG_DF 0x4000
#define IPV4_FLAG_MF 0x2000
#define IPV4_FRAG_OFF_MASK 0x1FFF

/* =================== Checksum =================== */
static uint16_t ipv4_checksum(const void *hdr, size_t len)
{
    const uint16_t *w = (const uint16_t *)hdr;
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

/* =================== Build header =================== */
void ipv4_build_header(ipv4_hdr_t *ip,
                       uint16_t total_len,
                       uint32_t saddr_be,
                       uint32_t daddr_be,
                       uint16_t id,
                       uint8_t ttl,
                       uint8_t protocol,
                       bool df)
{
    ip->ver_ihl = (4u << 4) | 5u; // IHL=5 (20 bytes)
    ip->tos = 0;
    ip->tot_len = htons(total_len);
    ip->id = htons(id);
    uint16_t fo = 0;
    if (df)
        fo |= IPV4_FLAG_DF;
    ip->frag_off = htons(fo);
    ip->ttl = ttl ? ttl : 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->saddr = saddr_be;
    ip->daddr = daddr_be;
    ip->checksum = ipv4_checksum(ip, sizeof(*ip));
}

/* =================== Packet builder =================== */
/* Compose IPv4 packet in-place: [IPv4 hdr][payload] */
size_t ipv4_build_packet(uint8_t *buf, size_t buflen,
                         uint32_t saddr_be, uint32_t daddr_be,
                         uint8_t proto, uint8_t ttl,
                         const void *payload, size_t payload_len,
                         uint16_t id, bool df)
{
    size_t need = sizeof(ipv4_hdr_t) + payload_len;
    if (buflen < need)
        return 0;
    ipv4_hdr_t *ip = (ipv4_hdr_t *)buf;
    if (payload_len)
        memcpy(buf + sizeof(*ip), payload, payload_len);
    ipv4_build_header(ip, (uint16_t)need, saddr_be, daddr_be, id, ttl, proto, df);
    return need;
}

/* =================== Parser/validator =================== */

typedef struct
{
    const ipv4_hdr_t *ip; // pointer into original buffer
    size_t ihl;           // bytes
    size_t total_len;     // bytes
    uint8_t version;
    bool hdr_ok;           // header checksum valid
    bool has_options;      // true if IHL>20
    uint16_t flags_off_be; // network-order flags+offset
} ipv4_parsed_t;

bool ipv4_parse(const uint8_t *pkt, size_t len, ipv4_parsed_t *out)
{
    if (len < sizeof(ipv4_hdr_t))
        return false;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)pkt;
    out->ip = ip;
    out->version = ip->ver_ihl >> 4;
    uint8_t ihl_words = ip->ver_ihl & 0x0F;
    out->ihl = (size_t)ihl_words * 4u;
    out->has_options = (ihl_words > 5);
    if (out->version != 4)
        return false;
    if (out->ihl < sizeof(ipv4_hdr_t) || out->ihl > len)
        return false;

    out->total_len = ntohs(ip->tot_len);
    if (out->total_len < out->ihl || out->total_len > len)
        return false;

    // validate header checksum
    ipv4_hdr_t tmp;
    memcpy(&tmp, ip, sizeof(tmp));
    tmp.checksum = 0;
    uint16_t cs = ipv4_checksum(&tmp, sizeof(tmp));
    out->hdr_ok = (cs == ip->checksum);

    out->flags_off_be = ip->frag_off;
    return true;
}

/* =================== Fragmentation =================== */
/* Calls user callback once per fragment; callback should send/store the fragment.
 * Callback prototype: int (*emit)(const uint8_t *frag, size_t frag_len, void *user)
 * Returns number of fragments emitted, or -1 on error from callback.
 */
int ipv4_fragment_and_emit(uint32_t saddr_be, uint32_t daddr_be,
                           uint8_t proto, uint8_t ttl, uint16_t id,
                           const uint8_t *payload, size_t payload_len,
                           size_t mtu,
                           int (*emit)(const uint8_t *, size_t, void *), void *user)
{
    if (mtu < sizeof(ipv4_hdr_t) + 8)
        return -1;                                                    // sanity
    size_t max_payload_per_frag = ((mtu - sizeof(ipv4_hdr_t)) & ~7u); // multiple of 8 bytes
    size_t offset = 0;
    int count = 0;
    while (offset < payload_len)
    {
        size_t frag_payload = payload_len - offset;
        bool more = false;
        if (frag_payload > max_payload_per_frag)
        {
            frag_payload = max_payload_per_frag;
            more = true;
        }
        size_t frag_len = sizeof(ipv4_hdr_t) + frag_payload;
        uint8_t *buf = (uint8_t *)malloc(frag_len);
        if (!buf)
            return -1;
        ipv4_hdr_t *ip = (ipv4_hdr_t *)buf;
        memcpy(buf + sizeof(*ip), payload + offset, frag_payload);
        ip->ver_ihl = (4u << 4) | 5u;
        ip->tos = 0;
        ip->tot_len = htons((uint16_t)frag_len);
        ip->id = htons(id);
        uint16_t off_units = (uint16_t)(offset / 8u);
        uint16_t fo = (more ? IPV4_FLAG_MF : 0) | (off_units & IPV4_FRAG_OFF_MASK);
        ip->frag_off = htons(fo);
        ip->ttl = ttl ? ttl : 64;
        ip->protocol = proto;
        ip->checksum = 0;
        ip->saddr = saddr_be;
        ip->daddr = daddr_be;
        ip->checksum = ipv4_checksum(ip, sizeof(*ip));

        int rc = emit(buf, frag_len, user);
        free(buf);
        if (rc != 0)
            return -1;
        count++;
        offset += frag_payload;
    }
    return count;
}

/* =================== Demo main =================== */
#ifdef IP_DEMO_MAIN
static int emit_sendto(const uint8_t *frag, size_t len, void *user)
{
    int s = *(int *)user; // raw socket
    struct sockaddr_in dst = {0};
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)frag;
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ip->daddr;
    ssize_t wr = sendto(s, frag, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (wr < 0)
    {
        perror("sendto");
        return -1;
    }
    return 0;
}

static int demo_send(const char *dst_ip_str, uint8_t proto, const char *data)
{
    int s = socket(AF_INET, SOCK_RAW, proto);
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

    struct in_addr dst_addr;
    if (inet_pton(AF_INET, dst_ip_str, &dst_addr) != 1)
    {
        fprintf(stderr, "bad dst ip\n");
        close(s);
        return 1;
    }

    // choose a source IP (for demo, loopback if dst is loopback)
    uint32_t saddr_be = (dst_addr.s_addr == htonl(INADDR_LOOPBACK)) ? htonl(INADDR_LOOPBACK) : htonl(0x7F000001);

    const uint8_t *payload = (const uint8_t *)data;
    size_t plen = strlen(data);
    size_t mtu = 1500; // demo MTU; adjust per iface

    int nfrag = ipv4_fragment_and_emit(saddr_be, dst_addr.s_addr, proto, 64, (uint16_t)rand(),
                                       payload, plen, mtu, emit_sendto, &s);
    if (nfrag < 0)
    {
        fprintf(stderr, "send failed\n");
        close(s);
        return 1;
    }
    printf("sent %d fragment(s) proto=%u to %s\n", nfrag, proto, dst_ip_str);
    close(s);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "Usage: %s send <dst_ip> <proto> <data>\n", argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "send"))
    {
        uint8_t proto = (uint8_t)atoi(argv[3]);
        return demo_send(argv[2], proto, argv[4]);
    }
    fprintf(stderr, "unknown command\n");
    return 1;
}
#endif
