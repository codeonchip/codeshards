// raw_udp_sender.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/socket.h>

#define DEST_IP "192.168.1.100"
#define DEST_PORT 12345
#define SRC_IP "192.168.1.10"
#define SRC_PORT 54321

// Checksum calculation function
unsigned short checksum(unsigned short *buf, int nwords)
{
    unsigned long sum = 0;
    while (nwords-- > 0)
        sum += *buf++;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return (unsigned short)(~sum);
}

int main()
{
    char buffer[4096];

    // Create raw socket
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0)
    {
        perror("socket() failed");
        return 1;
    }

    // Set IP_HDRINCL to tell the kernel that headers are included
    int one = 1;
    if (setsockopt(sock, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0)
    {
        perror("setsockopt() failed");
        return 1;
    }

    // Zero out buffer
    memset(buffer, 0, sizeof(buffer));

    // Pointers to headers
    struct iphdr *iph = (struct iphdr *)buffer;
    struct udphdr *udph = (struct udphdr *)(buffer + sizeof(struct iphdr));

    // Data part
    char *data = buffer + sizeof(struct iphdr) + sizeof(struct udphdr);
    const char *msg = "Hello from raw UDP!";
    int datalen = strlen(msg);
    strcpy(data, msg);

    // Fill in the IP Header
    iph->ihl = 5;
    iph->version = 4;
    iph->tos = 0;
    iph->tot_len = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + datalen);
    iph->id = htons(54321);
    iph->frag_off = 0;
    iph->ttl = 64;
    iph->protocol = IPPROTO_UDP;
    iph->check = 0;
    iph->saddr = inet_addr(SRC_IP);
    iph->daddr = inet_addr(DEST_IP);
    iph->check = checksum((unsigned short *)buffer, iph->ihl * 2);

    // UDP Header
    udph->source = htons(SRC_PORT);
    udph->dest = htons(DEST_PORT);
    udph->len = htons(sizeof(struct udphdr) + datalen);
    udph->check = 0; // UDP checksum is optional

    // Destination info
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = udph->dest;
    sin.sin_addr.s_addr = iph->daddr;

    // Send packet
    if (sendto(sock, buffer, ntohs(iph->tot_len), 0,
               (struct sockaddr *)&sin, sizeof(sin)) < 0)
    {
        perror("sendto() failed");
        return 1;
    }

    printf("Raw UDP packet sent to %s:%d\n", DEST_IP, DEST_PORT);

    close(sock);
    return 0;
}
