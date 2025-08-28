// xcp_host_udp.c
// Spec-inspired XCP demo over UDP (NOT production XCP).
// Implements: CONNECT(0xFF), DISCONNECT(0xFE), GET_STATUS(0xFD),
// GET_ID(0xFA), SET_MTA(0xF6), UPLOAD(0xF5), SHORT_UPLOAD(0xF4), DOWNLOAD(0xF0)
// Byte order: little-endian for addresses/sizes, like typical XCP examples.
//
// Build: gcc -std=c99 -O2 -Wall xcp_host_udp.c -o xcp_host
// Run:   ./xcp_host [port]
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define XCP_CMD_CONNECT 0xFF
#define XCP_CMD_DISCONNECT 0xFE
#define XCP_CMD_GET_STATUS 0xFD
#define XCP_CMD_SYNCH 0xFC
#define XCP_CMD_GET_ID 0xFA
#define XCP_CMD_SET_MTA 0xF6
#define XCP_CMD_UPLOAD 0xF5
#define XCP_CMD_SHORT_UPLOAD 0xF4
#define XCP_CMD_DOWNLOAD 0xF0

#define XCP_PID_RES 0xFF // Positive response PID (common in examples)
#define XCP_PID_ERR 0xFE // Error response PID

#define MEM_SIZE (64 * 1024)
static uint8_t mem_space[MEM_SIZE];

static uint32_t mta_addr = 0; // MTA0 address (address extension ignored in this toy)
static uint8_t mta_ext = 0;

static uint8_t session_status = 0x00; // e.g., DAQ_RUNNING bit would live here
static uint8_t prot_mask = 0x00;      // seed/key protected resources bitmask
static uint16_t session_id = 0x0001;

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void clamp_mta(void)
{
    if (mta_addr >= MEM_SIZE)
        mta_addr = MEM_SIZE - 1;
}

static void log_hex(const char *tag, const uint8_t *buf, size_t n)
{
    fprintf(stderr, "%s (%zu):", tag, n);
    for (size_t i = 0; i < n; ++i)
        fprintf(stderr, " %02X", buf[i]);
    fprintf(stderr, "\n");
}

static size_t handle_xcp(const uint8_t *req, size_t req_len, uint8_t *res, size_t res_cap)
{
    if (req_len == 0)
        return 0;
    uint8_t cmd = req[0];

    switch (cmd)
    {
    case XCP_CMD_CONNECT:
    {
        // Request: [0]=0xFF, [1]=mode (0=normal)
        // Response (8 bytes, per common table):
        // [0]=PID 0xFF, [1]=resource avail (bit2=DAQ), [2]=comm mode (bit7=Intel),
        // [3]=MAX_CTO, [4..5]=MAX_DTO (le), [6]=PL_VER, [7]=TL_VER
        if (res_cap < 8)
            return 0;
        res[0] = XCP_PID_RES;
        res[1] = 0x04; // DAQ available
        res[2] = 0x80; // Intel (little-endian)
        res[3] = 32;   // MAX_CTO
        res[4] = 32;
        res[5] = 0x00; // MAX_DTO = 32
        res[6] = 0x01; // Protocol layer version
        res[7] = 0x01; // Transport layer version (demo)
        return 8;
    }
    case XCP_CMD_GET_STATUS:
    {
        // Response (8 bytes demo): [0]=0xFF, [1]=session_status, [2]=prot_mask,
        // [3]=reserved 0x00, [4..5]=session_id (LE), [6]=0, [7]=0
        if (res_cap < 8)
            return 0;
        res[0] = XCP_PID_RES;
        res[1] = session_status;
        res[2] = prot_mask;
        res[3] = 0x00;
        put_le16(&res[4], session_id);
        res[6] = 0x00;
        res[7] = 0x00;
        return 8;
    }
    case XCP_CMD_GET_ID:
    {
        // Very simple ASCII ID: "DEMO_XCP_UDP"
        // Response: [0]=0xFF, [1]=idMode(0=ASCII), [2]=len, [3..]=bytes
        const char *id = "DEMO_XCP_UDP";
        size_t idlen = strlen(id);
        size_t need = 3 + idlen;
        if (res_cap < need)
            return 0;
        res[0] = XCP_PID_RES;
        res[1] = 0x00;
        res[2] = (uint8_t)idlen;
        memcpy(&res[3], id, idlen);
        return need;
    }
    case XCP_CMD_SET_MTA:
    {
        // Request: [0]=0xF6, [1]=addr_ext, [2..5]=address (LE)
        if (req_len < 6)
            goto err_format;
        mta_ext = req[1];
        mta_addr = get_le32(&req[2]);
        clamp_mta();
        // Positive response with PID only
        if (res_cap < 1)
            return 0;
        res[0] = XCP_PID_RES;
        return 1;
    }
    case XCP_CMD_UPLOAD:
    {
        // Request: [0]=0xF5, [1]=n (number of elements, element=byte in this toy)
        if (req_len < 2)
            goto err_format;
        uint8_t n = req[1];
        if (n == 0)
            n = 1;
        if (mta_addr + n > MEM_SIZE)
            n = (uint8_t)(MEM_SIZE - mta_addr);
        if (res_cap < 1 + n)
            return 0;
        res[0] = XCP_PID_RES;
        memcpy(&res[1], &mem_space[mta_addr], n);
        mta_addr += n;
        return (size_t)(1 + n);
    }
    case XCP_CMD_SHORT_UPLOAD:
    {
        // Request: [0]=0xF4, [1]=n, [2]=reserved, [3]=ext, [4..7]=address (LE)
        if (req_len < 8)
            goto err_format;
        uint8_t n = req[1];
        uint8_t ext = req[3];
        (void)ext; // ignored in this toy
        uint32_t addr = get_le32(&req[4]);
        if (addr >= MEM_SIZE)
            addr = MEM_SIZE - 1;
        if (addr + n > MEM_SIZE)
            n = (uint8_t)(MEM_SIZE - addr);
        if (res_cap < 1 + n)
            return 0;
        res[0] = XCP_PID_RES;
        memcpy(&res[1], &mem_space[addr], n);
        return (size_t)(1 + n);
    }
    case XCP_CMD_DOWNLOAD:
    {
        // Request: [0]=0xF0, [1]=n, [2..2+n-1]=data
        if (req_len < 2)
            goto err_format;
        uint8_t n = req[1];
        if (req_len < (size_t)(2 + n))
            goto err_format;
        if (mta_addr + n > MEM_SIZE)
            n = (uint8_t)(MEM_SIZE - mta_addr);
        memcpy(&mem_space[mta_addr], &req[2], n);
        mta_addr += n;
        if (res_cap < 1)
            return 0;
        res[0] = XCP_PID_RES;
        return 1;
    }
    case XCP_CMD_DISCONNECT:
    case XCP_CMD_SYNCH:
    {
        if (res_cap < 1)
            return 0;
        res[0] = XCP_PID_RES;
        return 1;
    }
    default:
        break;
    }

err_format:
    if (res_cap < 2)
        return 0;
    res[0] = XCP_PID_ERR; // error
    res[1] = 0x20;        // generic "ERR_CMD_SYNTAX" style
    return 2;
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 5555;

    // Seed demo memory with some bytes
    for (int i = 0; i < MEM_SIZE; ++i)
        mem_space[i] = (uint8_t)(i & 0xFF);
    // Place a faux signal at 0x1000 (4 bytes)
    mem_space[0x1000] = 0x11;
    mem_space[0x1001] = 0x22;
    mem_space[0x1002] = 0x33;
    mem_space[0x1003] = 0x44;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(fd);
        return 1;
    }
    fprintf(stderr, "XCP UDP slave listening on 0.0.0.0:%d\n", port);

    uint8_t inbuf[2048], outbuf[2048];
    for (;;)
    {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        ssize_t n = recvfrom(fd, inbuf, sizeof(inbuf), 0, (struct sockaddr *)&peer, &plen);
        if (n <= 0)
            continue;
        log_hex("REQ", inbuf, (size_t)n);

        size_t outn = handle_xcp(inbuf, (size_t)n, outbuf, sizeof(outbuf));
        if (outn > 0)
        {
            log_hex("RES", outbuf, outn);
            sendto(fd, outbuf, outn, 0, (struct sockaddr *)&peer, plen);
        }
    }
    return 0;
}
