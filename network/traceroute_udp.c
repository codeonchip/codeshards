// UDP-based traceroute for Linux (IPv4)
// Sends UDP datagrams with increasing TTL to high-numbered ports.
// Listens for ICMP Time Exceeded (hops) and ICMP Dest Unreach/Port (destination).
// sudo required (raw ICMP receive socket).
//
// Usage: ./traceroute_udp [-m max_hops] [-q probes] [-w timeout_ms] [-p base_port] host

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static double elapsed_ms(struct timeval a, struct timeval b)
{
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_usec - a.tv_usec) / 1000.0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-m max_hops] [-q probes] [-w timeout_ms] [-p base_port] host\n", prog);
}

int main(int argc, char **argv)
{
    int max_hops = 30;
    int probes = 3;
    int timeout_ms = 1000;
    int base_port = 33434;

    int opt;
    while ((opt = getopt(argc, argv, "m:q:w:p:")) != -1)
    {
        switch (opt)
        {
        case 'm':
            max_hops = atoi(optarg);
            break;
        case 'q':
            probes = atoi(optarg);
            break;
        case 'w':
            timeout_ms = atoi(optarg);
            break;
        case 'p':
            base_port = atoi(optarg);
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (optind >= argc)
    {
        usage(argv[0]);
        return 1;
    }

    const char *host = argv[optind];

    // Resolve destination (IPv4)
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM; // for the sending socket
    int gai = getaddrinfo(host, NULL, &hints, &res);
    if (gai != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        return 1;
    }
    struct sockaddr_in dst = *(struct sockaddr_in *)res->ai_addr;
    char dst_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dst.sin_addr, dst_ip, sizeof(dst_ip));

    // UDP send socket (kernel builds IP/UDP headers)
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0)
    {
        perror("socket udp");
        freeaddrinfo(res);
        return 1;
    }

    // ICMP receive socket (raw)
    int icmp_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (icmp_sock < 0)
    {
        perror("socket raw icmp");
        close(udp_sock);
        freeaddrinfo(res);
        return 1;
    }

    // Receive timeout on ICMP socket
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    if (setsockopt(icmp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
        perror("setsockopt SO_RCVTIMEO");
    }

    printf("traceroute (UDP) to %s (%s), %d hops max, %d probes, %d ms timeout, base port %d\n",
           host, dst_ip, max_hops, probes, timeout_ms, base_port);

    int reached = 0;

    for (int ttl = 1; ttl <= max_hops && !reached; ++ttl)
    {
        if (setsockopt(udp_sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0)
        {
            perror("setsockopt IP_TTL");
            break;
        }

        printf("%2d  ", ttl);
        fflush(stdout);

        char printed_ip[INET_ADDRSTRLEN] = "";
        int printed_any = 0;

        for (int p = 0; p < probes; ++p)
        {
            // Unique destination port per probe (classic traceroute trick)
            int dport = base_port + ttl * probes + p;

            struct sockaddr_in probe_dst = dst;
            probe_dst.sin_port = htons(dport);

            char payload[4] = {0, 0, 0, 0}; // tiny payload; not required
            struct timeval t0;
            gettimeofday(&t0, NULL);

            ssize_t s = sendto(udp_sock, payload, sizeof(payload), 0,
                               (struct sockaddr *)&probe_dst, sizeof(probe_dst));
            if (s < 0)
            {
                perror("sendto");
                printf("* ");
                continue;
            }

            // Receive ICMP
            unsigned char buf[1024];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t r = recvfrom(icmp_sock, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from, &fromlen);

            struct timeval t1;
            gettimeofday(&t1, NULL);

            if (r < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    printf("* ");
                    continue;
                }
                else
                {
                    perror("recvfrom");
                    printf("* ");
                    continue;
                }
            }

            if (r < (ssize_t)sizeof(struct iphdr))
            {
                printf("? ");
                continue;
            }

            // Outer IP + ICMP
            struct iphdr *ip = (struct iphdr *)buf;
            int iphdr_len = ip->ihl * 4;
            if (r < iphdr_len + (ssize_t)sizeof(struct icmphdr))
            {
                printf("? ");
                continue;
            }
            struct icmphdr *icmph = (struct icmphdr *)(buf + iphdr_len);

            // ICMP payload: original IP header + 8 bytes (includes UDP header)
            unsigned char *inner = (unsigned char *)icmph + sizeof(struct icmphdr);
            size_t remain = r - iphdr_len - sizeof(struct icmphdr);
            if (remain < sizeof(struct iphdr) + 8)
            {
                printf("? ");
                continue;
            }
            struct iphdr *oip = (struct iphdr *)inner;
            int oiphdr_len = oip->ihl * 4;
            if (remain < (size_t)oiphdr_len + 8)
            {
                printf("? ");
                continue;
            }
            // Only process if the original was UDP to our dest
            if (oip->protocol != IPPROTO_UDP)
            {
                printf("? ");
                continue;
            }
            struct udphdr *oudp = (struct udphdr *)(inner + oiphdr_len);
            int o_dport = ntohs(oudp->dest);

            if (o_dport != dport)
            {
                // Not our probe for this (rare if multiple outstanding)
                printf("? ");
                continue;
            }

            // Print hop IP once
            char hop_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, hop_ip, sizeof(hop_ip));
            if (!printed_any || strcmp(printed_ip, hop_ip) != 0)
            {
                if (printed_any)
                    printf("  ");
                printf("%s  ", hop_ip);
                strncpy(printed_ip, hop_ip, sizeof(printed_ip));
                printed_any = 1;
            }

            double rtt = elapsed_ms(t0, t1);
            printf("%.2f ms  ", rtt);

            if (icmph->type == ICMP_DEST_UNREACH && icmph->code == ICMP_PORT_UNREACH)
            {
                // Destination host reached (port unreachable at target)
                reached = 1;
            }
        }

        if (!printed_any)
            printf("*");
        printf("\n");
    }

    close(icmp_sock);
    close(udp_sock);
    freeaddrinfo(res);
    return 0;
}
