// Minimal ICMP traceroute (Linux, IPv4)
// sudo required. For learning/demo purposes.
//
// Usage: ./traceroute_icmp [-m max_hops] [-q probes] [-w timeout_ms] <host>

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static unsigned short icmp_checksum(const void *buf, int len)
{
    unsigned long sum = 0;
    const unsigned short *p = buf;
    while (len > 1)
    {
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
        sum += *(const unsigned char *)p;
    // fold
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (unsigned short)(~sum);
}

static double elapsed_ms(struct timeval a, struct timeval b)
{
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_usec - a.tv_usec) / 1000.0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-m max_hops] [-q probes] [-w timeout_ms] host\n", prog);
}

int main(int argc, char **argv)
{
    int max_hops = 30;
    int probes = 3;
    int timeout_ms = 1000;

    int opt;
    while ((opt = getopt(argc, argv, "m:q:w:")) != -1)
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

    // Resolve destination
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET; // IPv4 only for simplicity
    hints.ai_socktype = SOCK_RAW;
    int gai = getaddrinfo(host, NULL, &hints, &res);
    if (gai != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        return 1;
    }
    struct sockaddr_in dst = *(struct sockaddr_in *)res->ai_addr;
    char dst_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dst.sin_addr, dst_ip, sizeof(dst_ip));

    // Raw ICMP socket; kernel builds IP header for us
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0)
    {
        perror("socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)");
        freeaddrinfo(res);
        return 1;
    }

    // Receive timeout
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
        perror("setsockopt(SO_RCVTIMEO)");
        // continue anyway
    }

    printf("traceroute to %s (%s), %d hops max, %d probes, %d ms timeout\n",
           host, dst_ip, max_hops, probes, timeout_ms);

    // Identify our echo by (id, seq)
    unsigned short ident = (unsigned short)getpid();

    int reached = 0;

    for (int ttl = 1; ttl <= max_hops && !reached; ++ttl)
    {
        // Set TTL for outgoing probes
        if (setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0)
        {
            perror("setsockopt(IP_TTL)");
            break;
        }

        printf("%2d  ", ttl);
        fflush(stdout);

        char last_hop_ip[INET_ADDRSTRLEN] = "";
        int printed_hop = 0;

        for (int p = 0; p < probes; ++p)
        {
            // Build ICMP Echo Request
            unsigned char pkt[64];
            memset(pkt, 0, sizeof(pkt));
            struct icmphdr *icmp = (struct icmphdr *)pkt;
            icmp->type = ICMP_ECHO;
            icmp->code = 0;
            icmp->un.echo.id = htons(ident);
            icmp->un.echo.sequence = htons((unsigned short)(ttl * 64 + p));
            // Add a little payload (timestamp)
            struct timeval sent_tv;
            gettimeofday(&sent_tv, NULL);
            memcpy(pkt + sizeof(*icmp), &sent_tv, sizeof(sent_tv));
            int pkt_len = sizeof(*icmp) + (int)sizeof(sent_tv);
            icmp->checksum = 0;
            icmp->checksum = icmp_checksum(pkt, pkt_len);

            // Send
            struct timeval t0;
            gettimeofday(&t0, NULL);
            ssize_t s = sendto(sock, pkt, pkt_len, 0,
                               (struct sockaddr *)&dst, sizeof(dst));
            if (s < 0)
            {
                perror("sendto");
                printf("* ");
                continue;
            }

            // Receive
            unsigned char buf[512];
            struct sockaddr_in reply_addr;
            socklen_t reply_len = sizeof(reply_addr);
            ssize_t r = recvfrom(sock, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&reply_addr, &reply_len);
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

            // Parse IP header of received packet
            if (r < (ssize_t)sizeof(struct iphdr))
            {
                printf("? ");
                continue;
            }
            struct iphdr *ip = (struct iphdr *)buf;
            int iphdr_len = ip->ihl * 4;
            if (r < iphdr_len + (ssize_t)sizeof(struct icmphdr))
            {
                printf("? ");
                continue;
            }
            struct icmphdr *ricmp = (struct icmphdr *)(buf + iphdr_len);

            char hop_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &reply_addr.sin_addr, hop_ip, sizeof(hop_ip));

            // For ICMP Time Exceeded, the payload contains the original IP+8 bytes, which should include our ICMP header
            int is_our_probe = 0;
            if (ricmp->type == ICMP_TIME_EXCEEDED || ricmp->type == ICMP_DEST_UNREACH)
            {
                unsigned char *inner = (unsigned char *)ricmp + sizeof(struct icmphdr);
                if (r >= iphdr_len + (int)sizeof(struct icmphdr) + (int)sizeof(struct iphdr) + 8)
                {
                    struct iphdr *oip = (struct iphdr *)inner;
                    int oiphdr_len = oip->ihl * 4;
                    if (r >= iphdr_len + (int)sizeof(struct icmphdr) + oiphdr_len + (int)sizeof(struct icmphdr))
                    {
                        struct icmphdr *oicmp = (struct icmphdr *)(inner + oiphdr_len);
                        if (oicmp->type == ICMP_ECHO &&
                            ntohs(oicmp->un.echo.id) == ident)
                        {
                            unsigned short oseq = ntohs(oicmp->un.echo.sequence);
                            if (oseq == (unsigned short)(ttl * 64 + p))
                                is_our_probe = 1;
                        }
                    }
                }
            }
            else if (ricmp->type == ICMP_ECHOREPLY)
            {
                // Direct echo reply from destination
                if (ntohs(ricmp->un.echo.id) == ident &&
                    ntohs(ricmp->un.echo.sequence) == (unsigned short)(ttl * 64 + p))
                {
                    is_our_probe = 1;
                }
            }

            if (!is_our_probe)
            {
                // Not our packet; show placeholder (rare with timeout filter)
                printf("? ");
                continue;
            }

            if (!printed_hop || strcmp(last_hop_ip, hop_ip) != 0)
            {
                if (printed_hop)
                    printf("  "); // spacing before times if IP already printed
                printf("%s  ", hop_ip);
                strncpy(last_hop_ip, hop_ip, sizeof(last_hop_ip));
                printed_hop = 1;
            }

            double rtt = elapsed_ms(t0, t1);
            printf("%.2f ms  ", rtt);

            if (ricmp->type == ICMP_ECHOREPLY)
            {
                reached = 1; // destination reached
            }
        }

        if (!printed_hop)
            printf("*");
        printf("\n");
    }

    close(sock);
    freeaddrinfo(res);
    return 0;
}
