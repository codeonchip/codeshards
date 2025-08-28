/* at_parser.c
 * Minimal AT command parser with grouped demo command lists.
 *
 * Run profiles:
 *   ./at_parser -p basic
 *   ./at_parser -p gsm
 *   ./at_parser -p diag
 *   ./at_parser -p all     (default)
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>

#define RX_BUF_SZ 512
#define MAX_ARGS 8

/* ------------ Grouped demo command lists ------------ */
static const char *const demo_at_basic[] = {
    "AT",
    "ATI",
    "ATE0",
    "ATE1",
    "ATZ",
    "ATD5551234",
    "ATH",
    NULL};

static const char *const demo_at_gsm[] = {
    "AT+GMR",
    "AT+CSQ?",
    "AT+CMGF=1",
    "AT+CMGS=\"+1234567890\",\"Hello from GSM demo\"",
    NULL};

static const char *const demo_at_diag[] = {
    "AT+CSQ?;+GMR",
    "AT;+GMR;+CSQ?",
    "ATE0;AT+CSQ?;ATE1",
    NULL};

/* ------------ Utilities ------------ */
static void trim(char *s)
{
    size_t n = strlen(s), i = 0;
    while (i < n && isspace((unsigned char)s[i]))
        i++;
    size_t j = n;
    while (j > i && isspace((unsigned char)s[j - 1]))
        j--;
    if (i > 0)
        memmove(s, s + i, j - i);
    s[j - i] = '\0';
}

static int stricmp_(const char *a, const char *b)
{
    while (*a && *b)
    {
        int ca = tolower((unsigned char)*a++), cb = tolower((unsigned char)*b++);
        if (ca != cb)
            return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int strnicmp_(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (!ca || !cb)
            return ca - cb;
        ca = (unsigned char)tolower(ca);
        cb = (unsigned char)tolower(cb);
        if (ca != cb)
            return ca - cb;
    }
    return 0;
}

static void rsp_ok(void) { printf("OK\r\n"); }
static void rsp_error(void) { printf("ERROR\r\n"); }
static void rsp_cme(int n) { printf("+CME ERROR: %d\r\n", n); }

static int g_echo_enabled = 1;
static int g_text_mode = 1; /* 1=text, 0=PDU */

/* ------------ Command handlers ------------ */
static int cmd_AT(void)
{
    rsp_ok();
    return 0;
}

static int cmd_ATE_set(int val)
{
    if (val == 0 || val == 1)
    {
        g_echo_enabled = val;
        rsp_ok();
        return 0;
    }
    rsp_error();
    return -1;
}

static int cmd_ATE_query(void)
{
    printf("%d\r\n", g_echo_enabled ? 1 : 0);
    rsp_ok();
    return 0;
}

static int cmd_ATI(void)
{
    printf("Manufacturer: DemoCorp\r\nModel: DemoModem\r\nRevision: 1.0\r\n");
    rsp_ok();
    return 0;
}

static int cmd_ATZ(void)
{
    g_echo_enabled = 1;
    printf("RESET\r\n");
    rsp_ok();
    return 0;
}

static int cmd_ATD(const char *num)
{
    if (!num || !*num)
    {
        rsp_error();
        return -1;
    }
    printf("DIAL %s\r\nCONNECT\r\n", num);
    rsp_ok();
    return 0;
}

static int cmd_ATH(void)
{
    printf("HANGUP\r\n");
    rsp_ok();
    return 0;
}

static int cmd_GMR_exec(void)
{
    printf("DemoFW 2.0.7\r\n");
    rsp_ok();
    return 0;
}

static int cmd_CSQ_query(void)
{
    printf("+CSQ: 20,99\r\n");
    rsp_ok();
    return 0;
}

static int cmd_CMGF_query(void)
{
    printf("+CMGF: %d\r\n", g_text_mode);
    rsp_ok();
    return 0;
}

static int cmd_CMGF_set(int v)
{
    if (v == 0 || v == 1)
    {
        g_text_mode = v;
        rsp_ok();
        return 0;
    }
    rsp_cme(50);
    return -1;
}

static int cmd_CMGS_set(const char *num, const char *msg)
{
    if (!num || !msg)
    {
        rsp_cme(50);
        return -1;
    }
    printf("> Sending to %s...\r\n> %s\r\n+CMGS: 45\r\n", num, msg);
    rsp_ok();
    return 0;
}

/* ------------ Minimal argument helpers ------------ */
static int parse_int(const char *s, int *out)
{
    if (!s || !*s)
        return 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return 0;
    *out = (int)v;
    return 1;
}

/* expect "+CMGS=\"num\",\"msg\"" (simple, no escapes) */
static int parse_cmgs(char *cmd, char **num, char **msg)
{
    char *p = strchr(cmd, '"');
    if (!p)
        return 0;
    char *q = strchr(p + 1, '"');
    if (!q)
        return 0;
    *q = '\0';
    *num = p + 1;
    p = strchr(q + 1, '"');
    if (!p)
        return 0;
    q = strchr(p + 1, '"');
    if (!q)
        return 0;
    *q = '\0';
    *msg = p + 1;
    return 1;
}

/* ------------ Core parser (single command after "AT") ------------ */
static int handle_single(char *cmd)
{
    trim(cmd);
    if (*cmd == '\0')
        return cmd_AT();

    /* Query vs set vs exec detection */
    size_t n = strlen(cmd);
    int is_query = (n > 0 && cmd[n - 1] == '?');

    /* ATE forms: "E0"/"E1" or "E=0/1" or "E?" */
    if (toupper((unsigned char)cmd[0]) == 'E')
    {
        if (is_query && n == 2)
            return cmd_ATE_query();
        if (cmd[1] == '=' && (cmd[2] == '0' || cmd[2] == '1') && cmd[3] == '\0')
            return cmd_ATE_set(cmd[2] - '0');
        if ((cmd[1] == '0' || cmd[1] == '1') && cmd[2] == '\0')
            return cmd_ATE_set(cmd[1] - '0');
        /* bare "E" -> treat as E1 */
        if (cmd[1] == '\0')
            return cmd_ATE_set(1);
        rsp_error();
        return -1;
    }

    /* Short basics */
    if (stricmp_(cmd, "I") == 0)
        return cmd_ATI();
    if (stricmp_(cmd, "Z") == 0)
        return cmd_ATZ();
    if (stricmp_(cmd, "H") == 0)
        return cmd_ATH();
    if (toupper((unsigned char)cmd[0]) == 'D')
        return cmd_ATD(cmd + 1);

    /* Extended */
    if (stricmp_(cmd, "+GMR") == 0 || (is_query && stricmp_(cmd, "+GMR?") == 0))
        return cmd_GMR_exec();

    if (is_query && stricmp_(cmd, "+CSQ?") == 0)
        return cmd_CSQ_query();

    if (is_query && stricmp_(cmd, "+CMGF?") == 0)
        return cmd_CMGF_query();

    if (strnicmp_(cmd, "+CMGF=", 6) == 0)
    {
        int v = 0;
        if (!parse_int(cmd + 6, &v))
        {
            rsp_cme(50);
            return -1;
        }
        return cmd_CMGF_set(v);
    }

    if (strnicmp_(cmd, "+CMGS=", 6) == 0)
    {
        char *num = NULL, *msg = NULL;
        if (!parse_cmgs(cmd + 6, &num, &msg))
        {
            rsp_cme(50);
            return -1;
        }
        return cmd_CMGS_set(num, msg);
    }

    rsp_error();
    return -1;
}

/* Handle a full line like "AT+CSQ?;+GMR" (with optional echo) */
static void process_line(char *line)
{
    if (g_echo_enabled)
    {
        fputs(line, stdout);
        if (line[strlen(line) - 1] != '\n')
            fputc('\n', stdout);
    }
    trim(line);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = '\0';
    if (n < 2 || !(tolower((unsigned char)line[0]) == 'a' && tolower((unsigned char)line[1]) == 't'))
    {
        rsp_error();
        return;
    }
    char *p = line + 2;
    if (*p == '\0')
    {
        rsp_ok();
        return;
    }

    char buf[RX_BUF_SZ];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buf, ";", &save);
    while (tok)
    {
        char piece[RX_BUF_SZ];
        strncpy(piece, tok, sizeof(piece) - 1);
        piece[sizeof(piece) - 1] = '\0';
        if (handle_single(piece) != 0)
            return; /* stop on first error */
        tok = strtok_r(NULL, ";", &save);
    }
}

/* ------------ Demo runner ------------ */
static void run_demo_group(const char *title, const char *const group[])
{
    if (!group)
        return;
    printf("\n== %s ==\n", title);
    for (const char *const *p = group; *p; ++p)
    {
        char line[RX_BUF_SZ];
        strncpy(line, *p, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        printf(">>> %s\n", line);
        process_line(line);
    }
}

static int want_group(const char *profile, const char *name)
{
    return (profile == NULL) || (strcmp(profile, "all") == 0) || (strcmp(profile, name) == 0);
}

/* ------------ Main ------------ */
int main(int argc, char **argv)
{
    const char *profile = "all";
    if (argc >= 3 && strcmp(argv[1], "-p") == 0)
        profile = argv[2];
    else if (argc == 2 && strncmp(argv[1], "-p=", 3) == 0)
        profile = argv[1] + 3;

    printf("Demo AT Parser (profiles: basic | gsm | diag | all)\r\n");
    printf("Selected profile: %s\r\n", profile);

    if (want_group(profile, "basic"))
        run_demo_group("BASIC", demo_at_basic);
    if (want_group(profile, "gsm"))
        run_demo_group("GSM/SMS", demo_at_gsm);
    if (want_group(profile, "diag"))
        run_demo_group("DIAGNOSTIC", demo_at_diag);

    printf("\n--- Interactive mode (type AT lines, end with Enter) ---\r\n");
    char line[RX_BUF_SZ];
    while (fgets(line, sizeof(line), stdin))
    {
        process_line(line);
    }
    return 0;
}
