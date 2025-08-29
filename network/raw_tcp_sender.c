// raw_tcp_sender.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define DEST_IP "192.168.1.100"
#define DEST_PORT 1234
#define SRC_IP "192.168.1.10"
#define SRC_PORT 55555

// TCP pseudo-header (for checksum calculation)
struct pseudo_header
{
    u_int32_t src_addr;
    u_int32_t dst_addr;
    u_int8_t placeholder;
    u_int8_t protocol;
    u_int16_t tcp_length;
};

// Checksum function
unsigned short checksum(unsigned short *ptr, int nwords)
{
    unsigned long sum = 0;
    while (nwords-- > 0)
        sum += *ptr++;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return (unsigned short)(~sum);
}

int main()
{
    char buffer[4096];
    memset(buffer, 0, sizeof(buffer));

    // Raw socket
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (sock < 0)
    {
        perror("socket() failed");
        return 1;
    }

    // Tell kernel the IP header is included
    int one = 1;
    if (setsockopt(sock, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0)
    {
        perror("setsockopt() failed");
        return 1;
    }

    // IP header
    struct iphdr *iph = (struct iphdr *)buffer;
    struct tcphdr *tcph = (struct tcphdr *)(buffer + sizeof(struct iphdr));

    // Data (optional)
    char *data = buffer + sizeof(struct iphdr) + sizeof(struct tcphdr);
    const char *msg = "Raw TCP Hello!";
    int datalen = strlen(msg);
    strcpy(data, msg);

    // Fill IP header
    iph->ihl = 5;
    iph->version = 4;
    iph->tos = 0;
    iph->tot_len = htons(sizeof(struct iphdr) + sizeof(struct tcphdr) + datalen);
    iph->id = htons(54321);
    iph->frag_off = 0;
    iph->ttl = 64;
    iph->protocol = IPPROTO_TCP;
    iph->check = 0;
    iph->saddr = inet_addr(SRC_IP);
    iph->daddr = inet_addr(DEST_IP);
    iph->check = checksum((unsigned short *)buffer, iph->ihl * 2);

    // Fill TCP header
    tcph->source = htons(SRC_PORT);
    tcph->dest = htons(DEST_PORT);
    tcph->seq = htonl(0);
    tcph->ack_seq = 0;
    tcph->doff = 5;             // TCP header size
    tcph->syn = 1;              // SYN packet
    tcph->window = htons(5840); // default window size
    tcph->check = 0;
    tcph->urg_ptr = 0;

    // Pseudo header for checksum
    struct pseudo_header psh;
    psh.src_addr = inet_addr(SRC_IP);
    psh.dst_addr = inet_addr(DEST_IP);
    psh.placeholder = 0;
    psh.protocol = IPPROTO_TCP;
    psh.tcp_length = htons(sizeof(struct tcphdr) + datalen);

    int psize = sizeof(struct pseudo_header) + sizeof(struct tcphdr) + datalen;
    char *pseudogram = malloc(psize);
    memcpy(pseudogram, &psh, sizeof(struct pseudo_header));
    memcpy(pseudogram + sizeof(struct pseudo_header), tcph, sizeof(struct tcphdr) + datalen);

    tcph->check = checksum((unsigned short *)pseudogram, psize / 2);
    free(pseudogram);

    // Destination info
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = tcph->dest;
    sin.sin_addr.s_addr = iph->daddr;

    // Send packet
    if (sendto(sock, buffer, ntohs(iph->tot_len), 0,
               (struct sockaddr *)&sin, sizeof(sin)) < 0)
    {
        perror("sendto() failed");
        return 1;
    }

    printf("Raw TCP SYN packet sent to %s:%d\n", DEST_IP, DEST_PORT);

    close(sock);
    return 0;
}
