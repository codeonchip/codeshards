// nmea_parser.c — NMEA-0183 parser for GGA & RMC with checksum
// Build: gcc -std=c99 -O2 -Wall nmea_parser.c -o nmea_parser
// Run:   ./nmea_parser and ctrl-d      (uses built-in test lines)
//        cat gps.log | ./nmea_parser   (parses live or recorded GPS data)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static int hex2(const char *p)
{
    int v = 0;
    for (int i = 0; i < 2; i++)
    {
        char c = p[i];
        v <<= 4;
        if ('0' <= c && c <= '9')
            v |= c - '0';
        else if ('A' <= c && c <= 'F')
            v |= 10 + (c - 'A');
        else if ('a' <= c && c <= 'f')
            v |= 10 + (c - 'a');
        else
            return -1;
    }
    return v;
}

static int nmea_checksum_ok(const char *line)
{
    if (*line != '$')
        return 0;
    const char *p = line + 1;
    unsigned char cs = 0;
    while (*p && *p != '*' && *p != '\r' && *p != '\n')
        cs ^= (unsigned char)*p++;
    if (*p != '*')
        return 0;
    int want = hex2(p + 1);
    if (want < 0)
        return 0;
    return (cs == (unsigned)want);
}

// ddmm.mmmm → decimal degrees
static double nmea_deg_from_ddmm(const char *s)
{
    if (!s || !*s)
        return 0.0;
    double v = atof(s);
    int deg = (int)(v / 100.0);
    double min = v - deg * 100.0;
    return deg + (min / 60.0);
}

static char *field(char *s, int idx)
{
    // return pointer to field idx (0-based after sentence type)
    int n = 0;
    char *p = s;
    if (idx == 0)
        return p;
    while (*p)
    {
        if (*p == ',')
        {
            if (++n == idx)
                return p + 1;
        }
        else if (*p == '*')
        {
            return NULL;
        }
        ++p;
    }
    return NULL;
}

static void parse_gga(char *payload)
{
    char *f0 = field(payload, 0); // time
    char *f1 = field(payload, 1); // lat
    char *f2 = field(payload, 2); // N/S
    char *f3 = field(payload, 3); // lon
    char *f4 = field(payload, 4); // E/W
    char *f5 = field(payload, 5); // fix
    char *f6 = field(payload, 6); // sats
    char *f7 = field(payload, 7); // HDOP
    char *f8 = field(payload, 8); // alt

    if (!f1 || !f3)
        return;

    double lat = nmea_deg_from_ddmm(f1);
    if (f2 && (*f2 == 'S' || *f2 == 's'))
        lat = -lat;
    double lon = nmea_deg_from_ddmm(f3);
    if (f4 && (*f4 == 'W' || *f4 == 'w'))
        lon = -lon;

    printf("GGA time=%s lat=%.6f lon=%.6f fix=%s sats=%s hdop=%s alt=%sm\n",
           f0 ? f0 : "", lat, lon,
           f5 ? f5 : "", f6 ? f6 : "", f7 ? f7 : "", f8 ? f8 : "");
}

static void parse_rmc(char *payload)
{
    char *f0 = field(payload, 0); // time
    char *f1 = field(payload, 1); // status
    char *f2 = field(payload, 2); // lat
    char *f3 = field(payload, 3); // N/S
    char *f4 = field(payload, 4); // lon
    char *f5 = field(payload, 5); // E/W
    char *f6 = field(payload, 6); // speed
    char *f7 = field(payload, 7); // course
    char *f8 = field(payload, 8); // date

    if (!f1 || (*f1 != 'A' && *f1 != 'a'))
    {
        printf("RMC time=%s status=invalid\n", f0 ? f0 : "");
        return;
    }
    double lat = nmea_deg_from_ddmm(f2);
    if (f3 && (*f3 == 'S' || *f3 == 's'))
        lat = -lat;
    double lon = nmea_deg_from_ddmm(f4);
    if (f5 && (*f5 == 'W' || *f5 == 'w'))
        lon = -lon;
    double spd_kn = f6 ? atof(f6) : 0.0;
    double cog = f7 ? atof(f7) : 0.0;

    printf("RMC time=%s date=%s lat=%.6f lon=%.6f sog=%.2fkn cog=%.1f°\n",
           f0 ? f0 : "", f8 ? f8 : "", lat, lon, spd_kn, cog);
}

static void process_line(char *line)
{
    if (!nmea_checksum_ok(line))
        return;
    if (strlen(line) < 7)
        return;
    char type[6] = {0};
    memcpy(type, line + 1, 5);
    char *comma = strchr(line, ',');
    char *star = strchr(line, '*');
    if (!comma || !star || comma > star)
        return;
    *star = 0;
    char *payload = comma + 1;

    if (!memcmp(type + 2, "GGA", 3))
    {
        parse_gga(payload);
    }
    else if (!memcmp(type + 2, "RMC", 3))
    {
        parse_rmc(payload);
    }
}

int main(void)
{
    static const char *test_lines[] = {
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,,*6A",
        "$GPGGA,092750.000,5321.6802,N,00630.3372,W,1,08,1.0,73.1,M,55.2,M,,*76",
        "$GPRMC,092751.000,A,5321.6802,N,00630.3372,W,0.13,309.62,120598,,,A*77",
        NULL};

    char buf[512];
    int used_stdin = 0;

    // Try stdin first
    while (fgets(buf, sizeof(buf), stdin))
    {
        used_stdin = 1;
        process_line(buf);
    }

    // If nothing came from stdin, use test lines
    if (!used_stdin)
    {
        for (int i = 0; test_lines[i]; i++)
        {
            char tmp[512];
            strncpy(tmp, test_lines[i], sizeof(tmp));
            tmp[sizeof(tmp) - 1] = 0;
            process_line(tmp);
        }
    }
    return 0;
}
