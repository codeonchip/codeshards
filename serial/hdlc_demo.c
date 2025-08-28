/*
 * hdlc_demo.c
 * Minimal HDLC/PPP-style encoder/decoder with byte-stuffing and CRC-16/PPP.
 *
 * - Flag: 0x7E
 * - Escape: 0x7D, escaped_byte = byte ^ 0x20
 * - FCS: CRC-16/PPP (poly 0x8408 reflected, init 0xFFFF, final ones' complement)
 * - Valid frame check: if you run the CRC over payload+FCS, result is 0xF0B8.
 *
 * Build: gcc -std=c99 -O2 -Wall hdlc_demo.c -o hdlc_demo
 * 
 * echo "01 02 7E 03 7D 04" | ./hdlc_demo
 * or
 * ./hdlc_demo and then ctrl-d
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define HDLC_FLAG 0x7E
#define HDLC_ESC 0x7D
#define HDLC_XOR 0x20

/* ---------- CRC-16/PPP (CCITT, reflected) ---------- */
static uint16_t crc16_ppp_update(uint16_t fcs, uint8_t b)
{
    /* Polynomial 0x8408 (reversed 0x1021), init 0xFFFF */
    fcs ^= b;
    for (int i = 0; i < 8; ++i)
    {
        if (fcs & 1)
            fcs = (fcs >> 1) ^ 0x8408;
        else
            fcs = (fcs >> 1);
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

/* If you compute CRC over payload+FCS (little-endian), good frames end at 0xF0B8 */
static int crc16_ppp_verify(const uint8_t *payload, size_t len, const uint8_t fcs_le[2])
{
    uint8_t tmp[2048];
    if (len + 2 > sizeof(tmp))
        return 0;
    memcpy(tmp, payload, len);
    tmp[len + 0] = fcs_le[0];
    tmp[len + 1] = fcs_le[1];

    uint16_t fcs = 0xFFFF;
    for (size_t i = 0; i < len + 2; ++i)
        fcs = crc16_ppp_update(fcs, tmp[i]);
    return (fcs == 0xF0B8); /* PPP "good" residue */
}

/* ---------- Hex utils ---------- */
static void hexdump(const char *tag, const uint8_t *b, size_t n)
{
    printf("%s (%zu):", tag, n);
    for (size_t i = 0; i < n; ++i)
        printf(" %02X", b[i]);
    puts("");
}

/* ---------- Encoder: payload -> framed HDLC bytes ---------- */
static size_t hdlc_encode(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap)
{
    size_t w = 0;
    if (outcap < 1)
        return 0;
    out[w++] = HDLC_FLAG;

    /* Write payload with escaping */
    for (size_t i = 0; i < inlen; ++i)
    {
        uint8_t b = in[i];
        if (b == HDLC_FLAG || b == HDLC_ESC)
        {
            if (w + 2 > outcap)
                return 0;
            out[w++] = HDLC_ESC;
            out[w++] = b ^ HDLC_XOR;
        }
        else
        {
            if (w + 1 > outcap)
                return 0;
            out[w++] = b;
        }
    }

    /* Append FCS (little-endian), escaped if needed */
    uint16_t fcs = crc16_ppp(in, inlen);
    uint8_t f0 = (uint8_t)(fcs & 0xFF);
    uint8_t f1 = (uint8_t)(fcs >> 8);

    uint8_t fcs_bytes[2] = {f0, f1};
    for (int i = 0; i < 2; ++i)
    {
        uint8_t b = fcs_bytes[i];
        if (b == HDLC_FLAG || b == HDLC_ESC)
        {
            if (w + 2 > outcap)
                return 0;
            out[w++] = HDLC_ESC;
            out[w++] = b ^ HDLC_XOR;
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
    out[w++] = HDLC_FLAG;
    return w;
}

/* ---------- Decoder (streaming) ---------- */
typedef struct
{
    uint8_t buf[2048];
    size_t len;
    int in_frame;
    int esc;
} hdlc_dec_t;

static void hdlc_dec_init(hdlc_dec_t *d)
{
    memset(d, 0, sizeof(*d));
}

typedef void (*frame_cb_t)(const uint8_t *payload, size_t len, int fcs_ok, void *user);

/* Feed bytes; whenever a full frame arrives, call cb() with payload (without FCS) */
static void hdlc_decode_feed(hdlc_dec_t *d, const uint8_t *data, size_t n, frame_cb_t cb, void *user)
{
    for (size_t i = 0; i < n; ++i)
    {
        uint8_t b = data[i];

        if (b == HDLC_FLAG)
        {
            if (d->in_frame && d->len >= 2)
            {
                /* End of frame: split payload/FCS */
                if (d->len < 2)
                {
                    d->len = 0;
                    d->esc = 0;
                    continue;
                }
                size_t paylen = d->len - 2;
                uint8_t *p = d->buf;
                uint8_t fcs_le[2] = {d->buf[paylen], d->buf[paylen + 1]};
                int ok = crc16_ppp_verify(p, paylen, fcs_le);
                if (cb)
                    cb(p, paylen, ok, user);
            }
            /* Start new frame */
            d->in_frame = 1;
            d->len = 0;
            d->esc = 0;
            continue;
        }

        if (!d->in_frame)
            continue;

        if (d->esc)
        {
            b ^= HDLC_XOR;
            d->esc = 0;
        }
        else if (b == HDLC_ESC)
        {
            d->esc = 1;
            continue;
        }

        if (d->len < sizeof(d->buf))
            d->buf[d->len++] = b;
        else
        { /* overflow: drop frame */
            d->in_frame = 0;
            d->len = 0;
            d->esc = 0;
        }
    }
}

/* ---------- Small helper: parse hex from stdin into bytes ---------- */
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

/* ---------- Demo ---------- */

static void on_frame(const uint8_t *payload, size_t len, int fcs_ok, void *user)
{
    (void)user;
    printf("DECODED frame: payload_len=%zu  FCS=%s\n", len, fcs_ok ? "OK" : "BAD");
    hexdump("payload", payload, len);
}

int main(void)
{
    /* Default demo payload (contains 0x7E and 0x7D to show escaping) */
    uint8_t demo[] = {0x01, 0x02, 0x7E, 0x03, 0x7D, 0x04, 0x11, 0x22, 0x33};

    /* If the user typed a hex line on stdin, use that instead */
    uint8_t inbuf[1024];
    size_t inlen = 0;
    if (!feof(stdin))
    {
        /* Non-blocking-ish read: try to get a line; if none, use default */
        int c = fgetc(stdin);
        if (c != EOF)
        {
            ungetc(c, stdin);
            inlen = read_hex_line(inbuf, sizeof(inbuf));
        }
    }
    const uint8_t *payload = (inlen > 0) ? inbuf : demo;
    size_t payload_len = (inlen > 0) ? inlen : sizeof(demo);

    hexdump("payload", payload, payload_len);

    /* Encode to HDLC frame(s) */
    uint8_t framed[4096];
    size_t framed_len = hdlc_encode(payload, payload_len, framed, sizeof(framed));
    if (!framed_len)
    {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    hexdump("framed", framed, framed_len);

    /* Now feed back to decoder in arbitrary chunks to simulate a stream */
    hdlc_dec_t dec;
    hdlc_dec_init(&dec);
    size_t pos = 0;
    while (pos < framed_len)
    {
        size_t chunk = (framed_len - pos > 5) ? 5 : (framed_len - pos);
        hdlc_decode_feed(&dec, &framed[pos], chunk, on_frame, NULL);
        pos += chunk;
    }
    /* Send an extra flag to close any half-complete frame (optional) */
    uint8_t end = HDLC_FLAG;
    hdlc_decode_feed(&dec, &end, 1, on_frame, NULL);

    return 0;
}
