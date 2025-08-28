/*
 * ppp_demo.c — Minimal PPP encoder/decoder with byte-stuffing + CRC-16/PPP (FCS).
 *
 * Frame:  0x7E  [FF 03]  [Protocol_hi Protocol_lo]  [Payload...]  [FCS lo FCS hi]  0x7E
 * - Address=0xFF, Control=0x03 (UI frames)
 * - Protocol examples: 0x0021 IPv4, 0x0057 IPv6, 0xC021 LCP, 0x8021 IPCP
 * - FCS: CRC-16/PPP (poly 0x8408, init 0xFFFF, final ones' complement), sent little-endian.
 * - Byte-stuffing: 0x7E and 0x7D are escaped as 0x7D, (byte ^ 0x20).
 *   (PPP can escape more bytes via ACCM; we keep it minimal for clarity.)
 *
 * Build: gcc -std=c99 -O2 -Wall ppp_demo.c -o ppp_demo
 * Run:   ./ppp_demo
 *        echo "DE AD BE EF" | ./ppp_demo --proto 0x8021
 * Or
 * ./ppp_demo and then ctrl-d
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ---- PPP constants ---- */
#define PPP_FLAG 0x7E
#define PPP_ESC 0x7D
#define PPP_XOR 0x20
#define PPP_ADDR 0xFF
#define PPP_CTRL 0x03

/* ---- CRC-16/PPP ---- */
static uint16_t crc16_ppp_update(uint16_t fcs, uint8_t b)
{
    fcs ^= b;
    for (int i = 0; i < 8; ++i)
    {
        fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : (fcs >> 1);
    }
    return fcs;
}
static uint16_t crc16_ppp(const uint8_t *data, size_t len)
{
    uint16_t fcs = 0xFFFF;
    for (size_t i = 0; i < len; ++i)
        fcs = crc16_ppp_update(fcs, data[i]);
    return (uint16_t)~fcs; /* ones' complement */
}
/* PPP "good residue" when CRC run over (header+info+FCS) */
static int ppp_crc_verify(const uint8_t *hdr_info, size_t len, const uint8_t fcs_le[2])
{
    uint16_t fcs = 0xFFFF;
    for (size_t i = 0; i < len; ++i)
        fcs = crc16_ppp_update(fcs, hdr_info[i]);
    fcs = crc16_ppp_update(fcs, fcs_le[0]);
    fcs = crc16_ppp_update(fcs, fcs_le[1]);
    return (fcs == 0xF0B8);
}

/* ---- Utils ---- */
static void hexdump(const char *tag, const uint8_t *b, size_t n)
{
    printf("%s (%zu):", tag, n);
    for (size_t i = 0; i < n; ++i)
        printf(" %02X", b[i]);
    puts("");
}
static size_t read_hex_line(uint8_t *out, size_t cap)
{
    char line[4096];
    if (!fgets(line, sizeof(line), stdin))
        return 0;
    size_t w = 0;
    char *p = line;
    while (*p && w < cap)
    {
        while (*p && isspace((unsigned char)*p))
            ++p;
        if (!*p)
            break;
        unsigned v;
        if (sscanf(p, "%x", &v) == 1)
        {
            out[w++] = (uint8_t)(v & 0xFF);
            while (*p && !isspace((unsigned char)*p))
                ++p;
        }
        else
            break;
    }
    return w;
}
static int should_escape(uint8_t b)
{
    return (b == PPP_FLAG || b == PPP_ESC);
}

/* ---- Encoder: payload -> PPP frame bytes (stuffed) ---- */
static size_t ppp_encode(uint16_t protocol, const uint8_t *payload, size_t plen,
                         uint8_t *out, size_t outcap)
{
    size_t w = 0;
    if (outcap < 1)
        return 0;
    out[w++] = PPP_FLAG;

    /* Build header+info for FCS calc (no flags, no escapes yet) */
    uint8_t hdr[2 + 2]; /* [FF 03] + Protocol */
    hdr[0] = PPP_ADDR;
    hdr[1] = PPP_CTRL;
    hdr[2] = (uint8_t)((protocol >> 8) & 0xFF);
    hdr[3] = (uint8_t)(protocol & 0xFF);

    /* Compute FCS on header+payload */
    uint16_t fcs = 0xFFFF;
    for (size_t i = 0; i < sizeof(hdr); ++i)
        fcs = crc16_ppp_update(fcs, hdr[i]);
    for (size_t i = 0; i < plen; ++i)
        fcs = crc16_ppp_update(fcs, payload[i]);
    fcs = (uint16_t)~fcs;
    uint8_t fcs_lo = (uint8_t)(fcs & 0xFF), fcs_hi = (uint8_t)(fcs >> 8);

    /* Write header with stuffing */
    for (size_t i = 0; i < sizeof(hdr); ++i)
    {
        uint8_t b = hdr[i];
        if (should_escape(b))
        {
            if (w + 2 > outcap)
                return 0;
            out[w++] = PPP_ESC;
            out[w++] = (uint8_t)(b ^ PPP_XOR);
        }
        else
        {
            if (w + 1 > outcap)
                return 0;
            out[w++] = b;
        }
    }
    /* Write payload with stuffing */
    for (size_t i = 0; i < plen; ++i)
    {
        uint8_t b = payload[i];
        if (should_escape(b))
        {
            if (w + 2 > outcap)
                return 0;
            out[w++] = PPP_ESC;
            out[w++] = (uint8_t)(b ^ PPP_XOR);
        }
        else
        {
            if (w + 1 > outcap)
                return 0;
            out[w++] = b;
        }
    }
    /* Write FCS (LE) with stuffing */
    uint8_t fcs_bytes[2] = {fcs_lo, fcs_hi};
    for (int i = 0; i < 2; ++i)
    {
        uint8_t b = fcs_bytes[i];
        if (should_escape(b))
        {
            if (w + 2 > outcap)
                return 0;
            out[w++] = PPP_ESC;
            out[w++] = (uint8_t)(b ^ PPP_XOR);
        }
        else
        {
            if (w + 1 > outcap)
                return 0;
            out[w++] = b;
        }
    }

    if (w + 1 > outcap)
        return 0;
    out[w++] = PPP_FLAG;
    return w;
}

/* ---- Decoder (streaming) ---- */
typedef struct
{
    uint8_t buf[4096];
    size_t len;
    int in_frame;
    int esc;
} ppp_dec_t;

static void ppp_dec_init(ppp_dec_t *d) { memset(d, 0, sizeof(*d)); }

typedef void (*ppp_frame_cb)(uint16_t proto, const uint8_t *payload, size_t plen, int fcs_ok, void *user);

/* Feed stuffed bytes; on full frame, callback with parsed Protocol + payload */
static void ppp_decode_feed(ppp_dec_t *d, const uint8_t *data, size_t n, ppp_frame_cb cb, void *user)
{
    for (size_t i = 0; i < n; ++i)
    {
        uint8_t b = data[i];

        if (b == PPP_FLAG)
        {
            if (d->in_frame && d->len >= (2 + 2 + 2))
            {
                /* We have: [FF 03] [Proto_hi Proto_lo] [payload...] [FCS lo hi] */
                size_t L = d->len;
                uint8_t *f = d->buf;

                /* Basic checks */
                if (f[0] == PPP_ADDR && f[1] == PPP_CTRL)
                {
                    if (L >= 6)
                    {
                        uint16_t proto = (uint16_t)(f[2] << 8 | f[3]);
                        size_t info_len = L - 4 - 2; /* minus header 4 and FCS 2 */
                        uint8_t *info = &f[4];
                        uint8_t fcs_le[2] = {f[4 + info_len], f[5 + info_len]};

                        int ok = ppp_crc_verify(f, 4 + info_len, fcs_le);
                        if (cb)
                            cb(proto, info, info_len, ok, user);
                    }
                }
            }
            /* Reset/start */
            d->in_frame = 1;
            d->len = 0;
            d->esc = 0;
            continue;
        }

        if (!d->in_frame)
            continue;

        if (d->esc)
        {
            b ^= PPP_XOR;
            d->esc = 0;
        }
        else if (b == PPP_ESC)
        {
            d->esc = 1;
            continue;
        }

        if (d->len < sizeof(d->buf))
            d->buf[d->len++] = b;
        else
        {
            d->in_frame = 0;
            d->len = 0;
            d->esc = 0;
        } /* overflow: drop */
    }
}

/* ---- Demo ---- */
static void on_ppp_frame(uint16_t proto, const uint8_t *payload, size_t plen, int fcs_ok, void *user)
{
    (void)user;
    printf("DECODED PPP frame: proto=0x%04X  payload_len=%zu  FCS=%s\n",
           proto, plen, fcs_ok ? "OK" : "BAD");
    hexdump("payload", payload, plen);
}

/* small dummy IPv4 header (20B, no options) + 4B data */
static uint8_t demo_ipv4[] = {
    0x45, 0x00, 0x00, 0x18, 0x12, 0x34, 0x00, 0x00, 0x40, 0x11, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x01, 0x0A, 0x00, 0x00, 0x02, 0xDE, 0xAD, 0xBE, 0xEF};

int main(int argc, char **argv)
{
    /* Parse optional --proto 0xNNNN; default IPv4 (0x0021) */
    uint16_t proto = 0x0021;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--proto") == 0 && i + 1 < argc)
        {
            proto = (uint16_t)strtoul(argv[++i], NULL, 0);
        }
    }

    /* Read optional hex payload from stdin; else use demo_ipv4 */
    uint8_t inbuf[2048];
    size_t inlen = 0;
    {
        int c = fgetc(stdin);
        if (c != EOF)
        {
            ungetc(c, stdin);
            inlen = read_hex_line(inbuf, sizeof(inbuf));
        }
    }
    const uint8_t *payload = (inlen ? inbuf : demo_ipv4);
    size_t plen = (inlen ? inlen : sizeof(demo_ipv4));

    printf("Protocol=0x%04X\n", proto);
    hexdump("payload", payload, plen);

    uint8_t framed[8192];
    size_t framed_len = ppp_encode(proto, payload, plen, framed, sizeof(framed));
    if (!framed_len)
    {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    hexdump("framed", framed, framed_len);

    /* Feed back in small chunks to decoder */
    ppp_dec_t dec;
    ppp_dec_init(&dec);
    size_t pos = 0;
    while (pos < framed_len)
    {
        size_t chunk = (framed_len - pos > 7) ? 7 : (framed_len - pos);
        ppp_decode_feed(&dec, &framed[pos], chunk, on_ppp_frame, NULL);
        pos += chunk;
    }
    /* trailing flag to flush, optional */
    uint8_t flag = PPP_FLAG;
    ppp_decode_feed(&dec, &flag, 1, on_ppp_frame, NULL);
    return 0;
}
