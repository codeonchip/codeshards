// raw_icmp_sender.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <time.h>

#define DEST_IP "8.8.8.8" // Change to your target

// Checksum function
unsigned short checksum(void *b, int len)
{
    unsigned short *buf = b;
    unsigned int sum = 0;
    unsigned short result;

    for (sum = 0; len > 1; len -= 2)
        sum += *buf++;

    if (len == 1)
        sum += *(unsigned char *)buf;

    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    result = ~sum;

    return result;
}

int main()
{
    int sockfd;
    char buffer[1024];

    // Create raw socket for ICMP
    sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd < 0)
    {
        perror("socket() failed");
        return 1;
    }

    struct icmphdr *icmp = (struct icmphdr *)buffer;
    char *data = buffer + sizeof(struct icmphdr);
    int datalen = sprintf(data, "Raw ICMP Ping Test %ld", time(NULL));

    // Fill ICMP header
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->un.echo.id = getpid() & 0xFFFF;
    icmp->un.echo.sequence = 1;
    icmp->checksum = 0;

    int packet_len = sizeof(struct icmphdr) + datalen;
    icmp->checksum = checksum((unsigned short *)icmp, packet_len);

    // Destination
    struct sockaddr_in dest;
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(DEST_IP);

    // Send ICMP packet
    if (sendto(sockfd, buffer, packet_len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) <= 0)
    {
        perror("sendto() failed");
        return 1;
    }

    printf("ICMP Echo Request sent to %s\n", DEST_IP);

    close(sockfd);
    return 0;
}
