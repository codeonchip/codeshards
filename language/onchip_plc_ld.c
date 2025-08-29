/* Minimal Ladder (LD) interpreter with IL-style input
 * Features: LD/LDN, AND/ANDN, OR/ORN, NOT, OUT/SET/RESET, TON (on-delay), ENDRUNG
 * Timer semantics (TON): Q turns TRUE when IN has been TRUE for PT ms; resets when IN=FALSE
 * Compile:  gcc -std=c99 -O2 -Wall plc_ld.c -o plc_ld
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

/* ---------------- Config limits ---------------- */
#define MAX_SYMBOLS 128
#define MAX_TIMERS 64
#define MAX_RUNGS 128
#define MAX_INSTR 256
#define MAX_LINE 256
#define NAME_LEN 32

/* ---------------- Symbol table ---------------- */
typedef struct
{
    char name[NAME_LEN];
    bool value;
} Symbol;

static Symbol g_symbols[MAX_SYMBOLS];
static int g_symbol_count = 0;

static int sym_index(const char *name)
{
    for (int i = 0; i < g_symbol_count; ++i)
    {
        if (strcmp(g_symbols[i].name, name) == 0)
            return i;
    }
    if (g_symbol_count >= MAX_SYMBOLS)
    {
        fprintf(stderr, "Symbol table overflow for '%s'\n", name);
        return -1;
    }
    strncpy(g_symbols[g_symbol_count].name, name, NAME_LEN - 1);
    g_symbols[g_symbol_count].name[NAME_LEN - 1] = '\0';
    g_symbols[g_symbol_count].value = false;
    return g_symbol_count++;
}

static bool sym_get(int idx)
{
    if (idx < 0 || idx >= g_symbol_count)
        return false;
    return g_symbols[idx].value;
}

static void sym_set(int idx, bool v)
{
    if (idx < 0 || idx >= g_symbol_count)
        return;
    g_symbols[idx].value = v;
}

/* ---------------- TON timer table ---------------- */
typedef struct
{
    char name[NAME_LEN];
    uint32_t PT_ms; /* preset time (ms) */
    uint32_t ET_ms; /* elapsed time (ms) */
    bool IN_prev;   /* previous IN */
    bool Q;         /* output */
    bool used;      /* has this timer been referenced */
} TonTimer;

static TonTimer g_timers[MAX_TIMERS];

static int ton_index(const char *name)
{
    for (int i = 0; i < MAX_TIMERS; ++i)
    {
        if (g_timers[i].used && strcmp(g_timers[i].name, name) == 0)
            return i;
    }
    for (int i = 0; i < MAX_TIMERS; ++i)
    {
        if (!g_timers[i].used)
        {
            g_timers[i].used = true;
            strncpy(g_timers[i].name, name, NAME_LEN - 1);
            g_timers[i].name[NAME_LEN - 1] = '\0';
            g_timers[i].PT_ms = 0;
            g_timers[i].ET_ms = 0;
            g_timers[i].IN_prev = false;
            g_timers[i].Q = false;
            return i;
        }
    }
    fprintf(stderr, "Timer table overflow for '%s'\n", name);
    return -1;
}

/* Update a TON given IN and dt; returns Q */
static bool ton_eval(int idx, bool IN, uint32_t dt_ms, uint32_t PT_ms_override)
{
    if (idx < 0 || idx >= MAX_TIMERS || !g_timers[idx].used)
        return false;
    TonTimer *t = &g_timers[idx];

    /* Allow each rung to set/override PT for clarity (last value sticks). */
    if (PT_ms_override > 0)
        t->PT_ms = PT_ms_override;

    if (IN)
    {
        if (t->IN_prev)
        {
            /* continue timing */
            uint64_t sum = (uint64_t)t->ET_ms + dt_ms;
            t->ET_ms = (sum > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)sum;
        }
        else
        {
            /* started timing this scan */
            t->ET_ms = (dt_ms > 0) ? dt_ms : 0;
        }
    }
    else
    {
        t->ET_ms = 0;
    }
    t->IN_prev = IN;
    t->Q = (t->ET_ms >= t->PT_ms) && (t->PT_ms > 0);
    return t->Q;
}

/* ---------------- Program representation ---------------- */
typedef enum
{
    OPC_LD,    /* ACC = var */
    OPC_LDN,   /* ACC = !var */
    OPC_AND,   /* ACC = ACC & var */
    OPC_ANDN,  /* ACC = ACC & !var */
    OPC_OR,    /* ACC = ACC | var */
    OPC_ORN,   /* ACC = ACC | !var */
    OPC_NOT,   /* ACC = !ACC */
    OPC_TON,   /* ACC = TON(timer, IN=ACC, PT=...) */
    OPC_OUT,   /* var = ACC */
    OPC_SET,   /* if (ACC) var = 1; */
    OPC_RESET, /* if (ACC) var = 0; */
    OPC_ENDRUNG
} OpCode;

typedef struct
{
    OpCode op;
    int var_idx;    /* for *_var ops */
    int timer_idx;  /* for TON */
    uint32_t pt_ms; /* for TON */
} Instr;

typedef struct
{
    Instr instrs[MAX_INSTR];
    int len;
} Rung;

typedef struct
{
    Rung rungs[MAX_RUNGS];
    int rung_count;
} Program;

/* ---------------- Small helpers ---------------- */
static void trim(char *s)
{
    /* Trim leading and trailing whitespace in-place */
    size_t len = strlen(s);
    size_t start = 0;
    while (start < len && isspace((unsigned char)s[start]))
        start++;
    size_t end = len;
    while (end > start && isspace((unsigned char)s[end - 1]))
        end--;
    if (start > 0)
        memmove(s, s + start, end - start);
    s[end - start] = '\0';
}

static bool is_comment_or_blank(const char *s)
{
    if (!*s)
        return true;
    if (s[0] == ';' || s[0] == '#')
        return true;
    if (s[0] == '/' && s[1] == '/')
        return true;
    /* blank? */
    for (const char *p = s; *p; ++p)
        if (!isspace((unsigned char)*p))
            return false;
    return true;
}

/* Parse: TON <TimerName> PT=<number> (space/comma between args allowed) */
static bool parse_ton_args(const char *line, char *timer_name_out, uint32_t *pt_out)
{
    /* Accept forms:
       "TON T1 PT=2000"
       "TON  T1,  PT=2000"
    */
    const char *p = line;
    while (*p && !isspace((unsigned char)*p))
        p++; /* skip 'TON' */
    while (*p && isspace((unsigned char)*p))
        p++;
    if (!*p)
        return false;

    /* Timer name */
    int i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_'))
    {
        if (i < NAME_LEN - 1)
            timer_name_out[i++] = *p;
        p++;
    }
    timer_name_out[i] = '\0';
    while (*p && (isspace((unsigned char)*p) || *p == ','))
        p++;

    /* Expect PT= */
    if (toupper((unsigned char)p[0]) != 'P' || toupper((unsigned char)p[1]) != 'T' || p[2] != '=')
        return false;
    p += 3;
    /* Number */
    uint32_t val = 0;
    if (!isdigit((unsigned char)*p))
        return false;
    while (isdigit((unsigned char)*p))
    {
        val = (val * 10u) + (uint32_t)(*p - '0');
        p++;
    }
    *pt_out = val;
    return true;
}

/* ---------------- Parser: turns IL text into Program ---------------- */
static bool program_parse(Program *prog, const char *src)
{
    prog->rung_count = 0;
    Rung *current = NULL;

    char buf[MAX_LINE];
    const char *p = src;

    while (*p)
    {
        /* read one line */
        size_t n = 0;
        while (*p && *p != '\n' && n < MAX_LINE - 1)
            buf[n++] = *p++;
        if (*p == '\n')
            p++;
        buf[n] = '\0';
        trim(buf);
        if (is_comment_or_blank(buf))
            continue;

        /* Start new rung if none yet */
        if (current == NULL)
        {
            if (prog->rung_count >= MAX_RUNGS)
            {
                fprintf(stderr, "Too many rungs\n");
                return false;
            }
            current = &prog->rungs[prog->rung_count++];
            current->len = 0;
        }

        /* opcode word */
        char op[NAME_LEN] = {0};
        int consumed = 0;
        if (sscanf(buf, "%31s%n", op, &consumed) != 1)
            continue;

        /* normalize to upper */
        for (char *q = op; *q; ++q)
            *q = (char)toupper((unsigned char)*q);

        /* pointer to rest */
        const char *rest = buf + consumed;
        while (*rest == ' ' || *rest == '\t')
            rest++;

        Instr ins = {0};

        if (strcmp(op, "LD") == 0 || strcmp(op, "LDN") == 0 ||
            strcmp(op, "AND") == 0 || strcmp(op, "ANDN") == 0 ||
            strcmp(op, "OR") == 0 || strcmp(op, "ORN") == 0 ||
            strcmp(op, "OUT") == 0 || strcmp(op, "SET") == 0 ||
            strcmp(op, "RESET") == 0)
        {

            /* parse identifier */
            char name[NAME_LEN] = {0};
            int j = 0;
            while (*rest && (isalnum((unsigned char)*rest) || *rest == '_'))
            {
                if (j < NAME_LEN - 1)
                    name[j++] = *rest;
                rest++;
            }
            name[j] = '\0';
            if (name[0] == '\0' && (strcmp(op, "NOT") != 0))
            {
                fprintf(stderr, "Expected identifier after %s\n", op);
                return false;
            }
            int idx = (name[0] != '\0') ? sym_index(name) : -1;

            if (strcmp(op, "LD") == 0)
            {
                ins.op = OPC_LD;
                ins.var_idx = idx;
            }
            if (strcmp(op, "LDN") == 0)
            {
                ins.op = OPC_LDN;
                ins.var_idx = idx;
            }
            if (strcmp(op, "AND") == 0)
            {
                ins.op = OPC_AND;
                ins.var_idx = idx;
            }
            if (strcmp(op, "ANDN") == 0)
            {
                ins.op = OPC_ANDN;
                ins.var_idx = idx;
            }
            if (strcmp(op, "OR") == 0)
            {
                ins.op = OPC_OR;
                ins.var_idx = idx;
            }
            if (strcmp(op, "ORN") == 0)
            {
                ins.op = OPC_ORN;
                ins.var_idx = idx;
            }
            if (strcmp(op, "OUT") == 0)
            {
                ins.op = OPC_OUT;
                ins.var_idx = idx;
            }
            if (strcmp(op, "SET") == 0)
            {
                ins.op = OPC_SET;
                ins.var_idx = idx;
            }
            if (strcmp(op, "RESET") == 0)
            {
                ins.op = OPC_RESET;
                ins.var_idx = idx;
            }
        }
        else if (strcmp(op, "NOT") == 0)
        {
            ins.op = OPC_NOT;
        }
        else if (strcmp(op, "TON") == 0)
        {
            char tname[NAME_LEN] = {0};
            uint32_t pt = 0;
            if (!parse_ton_args(buf, tname, &pt))
            {
                fprintf(stderr, "Bad TON syntax: '%s'\n", buf);
                return false;
            }
            ins.op = OPC_TON;
            ins.timer_idx = ton_index(tname);
            ins.pt_ms = pt;
        }
        else if (strcmp(op, "ENDRUNG") == 0)
        {
            ins.op = OPC_ENDRUNG;
        }
        else
        {
            fprintf(stderr, "Unknown opcode: '%s'\n", op);
            return false;
        }

        if (current->len >= MAX_INSTR)
        {
            fprintf(stderr, "Too many instructions in rung\n");
            return false;
        }
        current->instrs[current->len++] = ins;

        if (ins.op == OPC_ENDRUNG)
            current = NULL; /* close rung */
    }

    /* auto-close last rung if not explicitly closed */
    if (current && (current->len == 0 || current->instrs[current->len - 1].op != OPC_ENDRUNG))
    {
        Instr endi = {.op = OPC_ENDRUNG};
        if (current->len < MAX_INSTR)
            current->instrs[current->len++] = endi;
    }

    return true;
}

/* ---------------- Scan-cycle execution ---------------- */
static void plc_scan(const Program *prog, uint32_t dt_ms)
{
    for (int r = 0; r < prog->rung_count; ++r)
    {
        const Rung *rg = &prog->rungs[r];
        bool acc = false;
        bool acc_init = false;

        for (int i = 0; i < rg->len; ++i)
        {
            const Instr *in = &rg->instrs[i];
            switch (in->op)
            {
            case OPC_LD:
                acc = sym_get(in->var_idx);
                acc_init = true;
                break;
            case OPC_LDN:
                acc = !sym_get(in->var_idx);
                acc_init = true;
                break;
            case OPC_AND:
                if (!acc_init)
                {
                    acc = sym_get(in->var_idx);
                    acc_init = true;
                }
                else
                    acc = acc && sym_get(in->var_idx);
                break;
            case OPC_ANDN:
                if (!acc_init)
                {
                    acc = !sym_get(in->var_idx);
                    acc_init = true;
                }
                else
                    acc = acc && !sym_get(in->var_idx);
                break;
            case OPC_OR:
                if (!acc_init)
                {
                    acc = sym_get(in->var_idx);
                    acc_init = true;
                }
                else
                    acc = acc || sym_get(in->var_idx);
                break;
            case OPC_ORN:
                if (!acc_init)
                {
                    acc = !sym_get(in->var_idx);
                    acc_init = true;
                }
                else
                    acc = acc || !sym_get(in->var_idx);
                break;
            case OPC_NOT:
                acc = !acc;
                acc_init = true;
                break;
            case OPC_TON:
                acc = ton_eval(in->timer_idx, acc, dt_ms, in->pt_ms);
                break;
            case OPC_OUT:
                sym_set(in->var_idx, acc);
                break;
            case OPC_SET:
                if (acc)
                    sym_set(in->var_idx, true);
                break;
            case OPC_RESET:
                if (acc)
                    sym_set(in->var_idx, false);
                break;
            case OPC_ENDRUNG: /* nothing */
                break;
            default:
                break;
            }
        }
    }
}

/* ---------------- Demo ---------------- */
static const char *demo_program =
    "; --- Rung 1: Motor seal-in (Start & !Stop) OR Motor ---\n"
    "LD Start\n"
    "ANDN Stop\n"
    "OR Motor\n"
    "OUT Motor\n"
    "ENDRUNG\n"
    "\n"
    "; --- Rung 2: Mirror Motor to Seal indicator ---\n"
    "LD Motor\n"
    "OUT Seal\n"
    "ENDRUNG\n"
    "\n"
    "; --- Rung 3: Pressing Stop unlatches Motor ---\n"
    "LD Stop\n"
    "RESET Motor\n"
    "ENDRUNG\n"
    "\n"
    "; --- Rung 4: Lamp turns on 2s after Start & !Stop ---\n"
    "LD Start\n"
    "ANDN Stop\n"
    "TON T1 PT=2000\n"
    "OUT Lamp\n"
    "ENDRUNG\n";

static void print_vars(void)
{
    printf("Vars: ");
    for (int i = 0; i < g_symbol_count; ++i)
    {
        printf("%s=%d ", g_symbols[i].name, g_symbols[i].value ? 1 : 0);
    }
    printf("\n");
}

int main(void)
{
    Program prog;
    if (!program_parse(&prog, demo_program))
    {
        fprintf(stderr, "Failed to parse program.\n");
        return 1;
    }

    int idxStart = sym_index("Start");
    int idxStop = sym_index("Stop");
    (void)idxStart;
    (void)idxStop;

    /* Simulate 3 seconds in 100 ms scans */
    uint32_t t = 0;
    for (int step = 0; step < 30; ++step)
    {
        /* Example input profile:
           - t=100ms: Start=1
           - t=1500ms: Stop=1 (forces Motor reset)
        */
        if (t >= 100 && t < 1500)
            sym_set(idxStart, true);
        else
            sym_set(idxStart, false);

        if (t >= 1500)
            sym_set(idxStop, true);
        else
            sym_set(idxStop, false);

        plc_scan(&prog, 100); /* dt = 100 ms scan time */

        printf("t=%4ums  ", t);
        print_vars();
        t += 100;
    }

    return 0;
}
