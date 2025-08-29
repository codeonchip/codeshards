/* plc_fbd.c - Function Block Diagram (FBD) interpreter with embedded demo
 *
 * Features
 *  - Small DSL: VAR, BLOCK, CONNECT
 *  - Blocks: AND, OR, XOR, NOT, ADD, SUB, MUL, GT, LT, EQ, MOVE, TON, R_TRIG, SR
 *  - Parameters: N=<int> for variadic logic arity; PT=<ms> for TON preset time
 *  - DAG check via topological sort; executes one scan with dt_ms
 *  - Boolean and Real values with automatic coercion at sinks
 *
 * Build:  gcc -std=c99 -O2 -Wall plc_fbd.c -o plc_fbd -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* ---------- Limits ---------- */
#define NAME_LEN 32
#define MAX_VARS 256
#define MAX_BLOCKS 256
#define MAX_PORTS 8
#define MAX_EDGES 1024

/* ---------- Value type ---------- */
typedef enum
{
    VT_BOOL = 0,
    VT_REAL = 1
} VType;

typedef struct
{
    VType type;
    union
    {
        bool b;
        float r;
    } v;
} Value;

static Value make_bool(bool b)
{
    Value x;
    x.type = VT_BOOL;
    x.v.b = b;
    return x;
}
static Value make_real(float r)
{
    Value x;
    x.type = VT_REAL;
    x.v.r = r;
    return x;
}
static bool to_bool(Value x) { return (x.type == VT_BOOL) ? x.v.b : (x.v.r != 0.0f); }
static float to_real(Value x) { return (x.type == VT_REAL) ? x.v.r : (x.v.b ? 1.0f : 0.0f); }

/* ---------- Case-insensitive helpers ---------- */
static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        int ca = toupper((unsigned char)*a), cb = toupper((unsigned char)*b);
        if (ca != cb)
            return 0;
    }
    return *a == 0 && *b == 0;
}

typedef struct
{
    int is_var;
    int index;
    int out_port;
} SourceRef; /* out_port: 0=OUT, 1=Q alias */

/* ---------- Variables ---------- */
typedef struct
{
    char name[NAME_LEN];
    VType type;
    Value val;
    int has_sink; /* one driver allowed */
    SourceRef sink_src;
    /*
    struct
    {
        int is_var;
        int index;
        int out_port;
    } sink_src;
    */
} Var;

static Var g_vars[MAX_VARS];
static int g_var_count = 0;

static int var_index(const char *name)
{
    for (int i = 0; i < g_var_count; i++)
        if (ieq(g_vars[i].name, name))
            return i;
    if (g_var_count >= MAX_VARS)
    {
        fprintf(stderr, "Var table overflow: %s\n", name);
        return -1;
    }
    strncpy(g_vars[g_var_count].name, name, NAME_LEN - 1);
    g_vars[g_var_count].name[NAME_LEN - 1] = '\0';
    g_vars[g_var_count].type = VT_BOOL;
    g_vars[g_var_count].val = make_bool(false);
    g_vars[g_var_count].has_sink = 0;
    return g_var_count++;
}

/* ---------- Blocks ---------- */
typedef enum
{
    BT_AND,
    BT_OR,
    BT_XOR,
    BT_NOT,
    BT_ADD,
    BT_SUB,
    BT_MUL,
    BT_GT,
    BT_LT,
    BT_EQ,
    BT_MOVE,
    BT_TON,
    BT_RTRIG,
    BT_SR
} BlockType;

#if 0
typedef struct
{
    int is_var;
    int index;
    int out_port;
} SourceRef; /* out_port: 0=OUT, 1=Q alias */
#endif

typedef struct
{
    char name[NAME_LEN];
    BlockType type;
    int n_in;
    char in_names[MAX_PORTS][NAME_LEN];
    SourceRef inputs[MAX_PORTS];
    /* Params */
    uint32_t PT_ms; /* for TON */
    /* State */
    uint32_t ton_ET;
    bool ton_in_prev;
    bool ton_Q;  /* TON */
    bool r_prev; /* R_TRIG */
    bool sr_Q;   /* SR */
    /* Output */
    Value out;
} Block;

static Block g_blocks[MAX_BLOCKS];
static int g_block_count = 0;

static BlockType block_type_from(const char *s)
{
    if (ieq(s, "AND"))
        return BT_AND;
    if (ieq(s, "OR"))
        return BT_OR;
    if (ieq(s, "XOR"))
        return BT_XOR;
    if (ieq(s, "NOT"))
        return BT_NOT;
    if (ieq(s, "ADD"))
        return BT_ADD;
    if (ieq(s, "SUB"))
        return BT_SUB;
    if (ieq(s, "MUL"))
        return BT_MUL;
    if (ieq(s, "GT"))
        return BT_GT;
    if (ieq(s, "LT"))
        return BT_LT;
    if (ieq(s, "EQ"))
        return BT_EQ;
    if (ieq(s, "MOVE"))
        return BT_MOVE;
    if (ieq(s, "TON"))
        return BT_TON;
    if (ieq(s, "R_TRIG") || ieq(s, "RTRIG"))
        return BT_RTRIG;
    if (ieq(s, "SR"))
        return BT_SR;
    return -1;
}

static int block_index(const char *name)
{
    for (int i = 0; i < g_block_count; i++)
        if (ieq(g_blocks[i].name, name))
            return i;
    return -1;
}

static void block_set_ports(Block *b)
{
    memset(b->in_names, 0, sizeof(b->in_names));
    b->n_in = 0;
    switch (b->type)
    {
    case BT_AND:
    case BT_OR:
    case BT_XOR:
    {
        if (b->n_in <= 0)
            b->n_in = 2;
        if (b->n_in > MAX_PORTS)
            b->n_in = MAX_PORTS;
        for (int i = 0; i < b->n_in; i++)
            snprintf(b->in_names[i], NAME_LEN, "IN%d", i + 1);
    }
    break;
    case BT_ADD:
    case BT_SUB:
    case BT_MUL:
        b->n_in = 2;
        strcpy(b->in_names[0], "IN1");
        strcpy(b->in_names[1], "IN2");
        break;
    case BT_GT:
    case BT_LT:
    case BT_EQ:
        b->n_in = 2;
        strcpy(b->in_names[0], "A");
        strcpy(b->in_names[1], "B");
        break;
    case BT_NOT:
    case BT_MOVE:
    case BT_TON:
    case BT_RTRIG:
        b->n_in = 1;
        strcpy(b->in_names[0], "IN");
        break;
    case BT_SR:
        b->n_in = 2;
        strcpy(b->in_names[0], "S");
        strcpy(b->in_names[1], "R");
        break;
    default:
        break;
    }
    for (int i = 0; i < b->n_in; i++)
    {
        b->inputs[i].is_var = 1;
        b->inputs[i].index = -1;
        b->inputs[i].out_port = 0;
    }
}

/* ---------- Parser helpers ---------- */
static void trim(char *s)
{
    size_t n = strlen(s), a = 0;
    while (a < n && isspace((unsigned char)s[a]))
        a++;
    size_t b = n;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    if (a > 0)
        memmove(s, s + a, b - a);
    s[b - a] = '\0';
}
static void strip_comment(char *s)
{
    for (char *p = s; *p; ++p)
    {
        if ((p == s && (*p == '#' || *p == ';')) || (p[0] == '/' && p[1] == '/'))
        {
            *p = '\0';
            break;
        }
    }
    trim(s);
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == ';')
    {
        s[n - 1] = '\0';
        trim(s);
    }
}

static void parse_kv(Block *b, const char *tok)
{
    char key[32] = {0}, val[32] = {0};
    const char *eq = strchr(tok, '=');
    if (!eq)
        return;
    size_t kl = (size_t)(eq - tok);
    if (kl >= sizeof(key))
        kl = sizeof(key) - 1;
    memcpy(key, tok, kl);
    key[kl] = '\0';
    strncpy(val, eq + 1, sizeof(val) - 1);
    for (char *q = key; *q; q++)
        *q = (char)toupper((unsigned char)*q);
    if (strcmp(key, "N") == 0)
    {
        int n = atoi(val);
        if (n < 1)
            n = 1;
        g_blocks[g_block_count - 1].n_in = n;
        block_set_ports(&g_blocks[g_block_count - 1]);
    }
    else if (strcmp(key, "PT") == 0)
    {
        b->PT_ms = (uint32_t)strtoul(val, NULL, 10);
    }
}

/* ---------- Program building ---------- */
static int add_block(const char *name, const char *type, const char *params_line)
{
    if (g_block_count >= MAX_BLOCKS)
    {
        fprintf(stderr, "Too many blocks\n");
        return -1;
    }
    Block *b = &g_blocks[g_block_count++];
    memset(b, 0, sizeof(*b));
    strncpy(b->name, name, NAME_LEN - 1);
    b->name[NAME_LEN - 1] = '\0';
    b->type = block_type_from(type);
    if (b->type < 0)
    {
        fprintf(stderr, "Unknown block type: %s\n", type);
        return -1;
    }
    b->PT_ms = 0;
    b->ton_ET = 0;
    b->ton_in_prev = false;
    b->ton_Q = false;
    b->r_prev = false;
    b->sr_Q = false;
    b->out = make_bool(false);
    b->n_in = 0;
    block_set_ports(b);
    char tmp[256];
    strncpy(tmp, params_line ? params_line : "", 255);
    tmp[255] = '\0';
    char *p = tmp;
    while (*p)
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        char tok[64] = {0};
        int ti = 0;
        while (*p && !isspace((unsigned char)*p) && ti < 63)
            tok[ti++] = *p++;
        tok[ti] = '\0';
        parse_kv(b, tok);
    }
    return g_block_count - 1;
}

static int ensure_var_decl(const char *name, VType t, const char *init)
{
    int idx = var_index(name);
    g_vars[idx].type = t;
    if (init && *init)
    {
        if (t == VT_BOOL)
            g_vars[idx].val = make_bool(ieq(init, "1") || ieq(init, "TRUE"));
        else
            g_vars[idx].val = make_real((float)atof(init));
    }
    return idx;
}

static int port_index_for(const Block *b, const char *port)
{
    for (int i = 0; i < b->n_in; i++)
        if (ieq(b->in_names[i], port))
            return i;
    return -1;
}

static int parse_endpoint_src(const char *tok, SourceRef *src)
{
    char name[NAME_LEN] = {0}, port[NAME_LEN] = {0};
    const char *dot = strchr(tok, '.');
    if (dot)
    {
        size_t nl = (size_t)(dot - tok);
        if (nl >= NAME_LEN)
            nl = NAME_LEN - 1;
        memcpy(name, tok, nl);
        name[nl] = '\0';
        strncpy(port, dot + 1, NAME_LEN - 1);
    }
    else
    {
        strncpy(name, tok, NAME_LEN - 1);
        port[0] = '\0';
    }
    int bi = block_index(name);
    if (bi >= 0)
    {
        int outp = 0;
        if (port[0])
        {
            if (ieq(port, "OUT"))
                outp = 0;
            else if (ieq(port, "Q"))
                outp = 1;
            else
            {
                fprintf(stderr, "Unknown block output port: %s.%s\n", name, port);
                return 0;
            }
        }
        src->is_var = 0;
        src->index = bi;
        src->out_port = outp;
        return 1;
    }
    int vi = var_index(name);
    (void)vi;
    src->is_var = 1;
    src->index = vi;
    src->out_port = 0;
    return 1;
}

static int connect_src_to_block_port(const char *src_tok, const char *dst_tok)
{
    const char *dot = strchr(dst_tok, '.');
    if (!dot)
    {
        fprintf(stderr, "Destination must be Block.Port: %s\n", dst_tok);
        return 0;
    }
    char bname[NAME_LEN] = {0}, pname[NAME_LEN] = {0};
    size_t nl = (size_t)(dot - dst_tok);
    if (nl >= NAME_LEN)
        nl = NAME_LEN - 1;
    memcpy(bname, dst_tok, nl);
    bname[nl] = '\0';
    strncpy(pname, dot + 1, NAME_LEN - 1);
    int bi = block_index(bname);
    if (bi < 0)
    {
        fprintf(stderr, "Unknown block in CONNECT: %s\n", bname);
        return 0;
    }
    Block *b = &g_blocks[bi];
    int pi = port_index_for(b, pname);
    if (pi < 0)
    {
        fprintf(stderr, "Unknown input port %s on %s\n", pname, bname);
        return 0;
    }
    SourceRef s;
    if (!parse_endpoint_src(src_tok, &s))
        return 0;
    b->inputs[pi] = s;
    return 1;
}

static int connect_src_to_var(const char *src_tok, const char *dst_tok)
{
    char vname[NAME_LEN] = {0};
    const char *dot = strchr(dst_tok, '.');
    if (dot)
    {
        size_t nl = (size_t)(dot - dst_tok);
        if (nl >= NAME_LEN)
            nl = NAME_LEN - 1;
        memcpy(vname, dst_tok, nl);
        vname[nl] = '\0';
    }
    else
    {
        strncpy(vname, dst_tok, NAME_LEN - 1);
    }
    int vi = var_index(vname);
    SourceRef s;
    if (!parse_endpoint_src(src_tok, &s))
        return 0;
    if (g_vars[vi].has_sink)
    {
        fprintf(stderr, "Variable %s already driven\n", vname);
        return 0;
    }
    g_vars[vi].has_sink = 1;
    g_vars[vi].sink_src = s;
    return 1;
}

/* ---------- Topological sort ---------- */
static int topo_order[MAX_BLOCKS];
static int build_topology(void)
{
    int indeg[MAX_BLOCKS] = {0};
    int adj_head[MAX_BLOCKS];
    for (int i = 0; i < MAX_BLOCKS; i++)
        adj_head[i] = -1;
    int adj_next[MAX_EDGES] = {0}, adj_to[MAX_EDGES] = {0}, ecount = 0;
    for (int bi = 0; bi < g_block_count; ++bi)
    {
        for (int pi = 0; pi < g_blocks[bi].n_in; ++pi)
        {
            SourceRef s = g_blocks[bi].inputs[pi];
            if (!s.is_var && s.index >= 0)
            {
                int from = s.index, to = bi;
                adj_next[ecount] = adj_head[from];
                adj_to[ecount] = to;
                adj_head[from] = ecount++;
                indeg[to]++;
            }
        }
    }
    int q[MAX_BLOCKS], qh = 0, qt = 0, outc = 0;
    for (int i = 0; i < g_block_count; i++)
        if (indeg[i] == 0)
            q[qt++] = i;
    while (qh < qt)
    {
        int u = q[qh++];
        topo_order[outc++] = u;
        for (int e = adj_head[u]; e != -1; e = adj_next[e])
        {
            int v = adj_to[e];
            if (--indeg[v] == 0)
                q[qt++] = v;
        }
    }
    if (outc != g_block_count)
    {
        fprintf(stderr, "Cycle detected in block graph (combinatorial loop). Use SR/TON/etc. to break cycles.\n");
        return 0;
    }
    return 1;
}

/* ---------- Execution ---------- */
static Value read_source(const SourceRef *s)
{
    if (s->is_var)
        return g_vars[s->index].val;
    else
        return g_blocks[s->index].out;
}

static void eval_block(Block *b, uint32_t dt_ms)
{
    Value in[MAX_PORTS];
    for (int i = 0; i < b->n_in; i++)
        in[i] = read_source(&b->inputs[i]);
    switch (b->type)
    {
    case BT_AND:
    {
        bool acc = true;
        for (int i = 0; i < b->n_in; i++)
            acc = acc && to_bool(in[i]);
        b->out = make_bool(acc);
    }
    break;
    case BT_OR:
    {
        bool acc = false;
        for (int i = 0; i < b->n_in; i++)
            acc = acc || to_bool(in[i]);
        b->out = make_bool(acc);
    }
    break;
    case BT_XOR:
    {
        int cnt = 0;
        for (int i = 0; i < b->n_in; i++)
            if (to_bool(in[i]))
                cnt++;
        b->out = make_bool((cnt % 2) == 1);
    }
    break;
    case BT_NOT:
        b->out = make_bool(!to_bool(in[0]));
        break;
    case BT_MOVE:
        b->out = in[0];
        break;
    case BT_ADD:
        b->out = make_real(to_real(in[0]) + to_real(in[1]));
        break;
    case BT_SUB:
        b->out = make_real(to_real(in[0]) - to_real(in[1]));
        break;
    case BT_MUL:
        b->out = make_real(to_real(in[0]) * to_real(in[1]));
        break;
    case BT_GT:
        b->out = make_bool(to_real(in[0]) > to_real(in[1]));
        break;
    case BT_LT:
        b->out = make_bool(to_real(in[0]) < to_real(in[1]));
        break;
    case BT_EQ:
        b->out = make_bool(fabsf(to_real(in[0]) - to_real(in[1])) < 1e-6f);
        break;
    case BT_RTRIG:
    {
        bool v = to_bool(in[0]);
        bool q = (v && !b->r_prev);
        b->r_prev = v;
        b->out = make_bool(q);
    }
    break;
    case BT_SR:
    {
        bool S = to_bool(in[0]);
        bool R = to_bool(in[1]);
        if (R)
            b->sr_Q = false;
        else if (S)
            b->sr_Q = true;
        b->out = make_bool(b->sr_Q);
    }
    break;
    case BT_TON:
    {
        bool IN = to_bool(in[0]);
        if (IN)
        {
            if (b->ton_in_prev)
            {
                uint64_t sum = (uint64_t)b->ton_ET + dt_ms;
                b->ton_ET = (sum > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)sum;
            }
            else
            {
                b->ton_ET = dt_ms;
            }
        }
        else
        {
            b->ton_ET = 0;
        }
        b->ton_in_prev = IN;
        bool Q = (b->PT_ms > 0) && (b->ton_ET >= b->PT_ms);
        b->ton_Q = Q;
        b->out = make_bool(Q);
    }
    break;
    default:
        b->out = make_bool(false);
        break;
    }
}

static void fbd_scan(uint32_t dt_ms)
{
    for (int i = 0; i < g_block_count; i++)
    {
        int bi = topo_order[i];
        eval_block(&g_blocks[bi], dt_ms);
    }
    for (int vi = 0; vi < g_var_count; ++vi)
    {
        if (g_vars[vi].has_sink)
        {
            Value v = read_source(&g_vars[vi].sink_src);
            if (g_vars[vi].type == VT_BOOL)
                g_vars[vi].val = make_bool(to_bool(v));
            else
                g_vars[vi].val = make_real(to_real(v));
        }
    }
}

/* ---------- Pretty printing ---------- */
static void print_vars(void)
{
    printf("Vars: ");
    for (int i = 0; i < g_var_count; i++)
    {
        printf("%s=", g_vars[i].name);
        if (g_vars[i].type == VT_BOOL)
            printf("%d ", g_vars[i].val.v.b ? 1 : 0);
        else
            printf("%.3f ", g_vars[i].val.v.r);
    }
    printf("\n");
}

/* ---------- Parser for the small DSL ---------- */
/*
Grammar (line-based):
  VAR <TYPE> <NAME> [= <LIT>];
  BLOCK <NAME> <TYPE> [PARAMS...];
    - Params: N=<int> (AND/OR/XOR arity), PT=<ms> (TON)
  CONNECT <SRC> -> <DST>;
    - SRC: <VarName> | <BlockName>[.(OUT|Q)]
    - DST: <BlockName>.<Port> | <VarName>[.IN]
*/

static int parse_line(char *line)
{
    strip_comment(line);
    if (!*line)
        return 1;
    char head[16] = {0};
    int ncons = 0;
    if (sscanf(line, "%15s%n", head, &ncons) != 1)
        return 1;
    for (char *p = head; *p; ++p)
        *p = (char)toupper((unsigned char)*p);
    const char *rest = line + ncons;
    if (ieq(head, "VAR"))
    {
        char tbuf[16] = {0}, nbuf[NAME_LEN] = {0}, init[64] = {0};
        const char *p = rest;
        while (*p && isspace((unsigned char)*p))
            p++;
        int c1 = 0;
        if (sscanf(p, "%15s%n", tbuf, &c1) != 1)
        {
            fprintf(stderr, "VAR: missing type/name\n");
            return 0;
        }
        p += c1;
        while (*p && isspace((unsigned char)*p))
            p++;
        int c2 = 0;
        if (sscanf(p, "%31s%n", nbuf, &c2) != 1)
        {
            fprintf(stderr, "VAR: missing name\n");
            return 0;
        }
        p += c2;
        while (*p && isspace((unsigned char)*p))
            p++;
        VType t = (ieq(tbuf, "REAL")) ? VT_REAL : VT_BOOL;
        if (*p == '=')
        {
            p++;
            while (*p && isspace((unsigned char)*p))
                p++;
            int c3 = 0;
            if (sscanf(p, "%63s%n", init, &c3) == 1)
            {
            }
        }
        ensure_var_decl(nbuf, t, init[0] ? init : NULL);
        return 1;
    }
    else if (ieq(head, "BLOCK"))
    {
        char name[NAME_LEN] = {0}, tbuf[16] = {0};
        int c1 = 0;
        if (sscanf(rest, " %31s %15s%n", name, tbuf, &c1) < 2)
        {
            fprintf(stderr, "BLOCK syntax error\n");
            return 0;
        }
        const char *param_line = rest + c1;
        if (add_block(name, tbuf, param_line) < 0)
            return 0;
        return 1;
    }
    else if (ieq(head, "CONNECT"))
    {
        char left[64] = {0}, right[64] = {0};
        const char *arrow = strstr(rest, "->");
        if (!arrow)
        {
            fprintf(stderr, "CONNECT missing '->'\n");
            return 0;
        }
        size_t ll = (size_t)(arrow - rest);
        if (ll >= sizeof(left))
            ll = sizeof(left) - 1;
        memcpy(left, rest, ll);
        left[ll] = '\0';
        const char *rp = arrow + 2;
        while (ll > 0 && isspace((unsigned char)left[ll - 1]))
            left[--ll] = '\0';
        while (*rp && isspace((unsigned char)*rp))
            rp++;
        strncpy(right, rp, sizeof(right) - 1);
        trim(left);
        trim(right);
        if (strchr(right, '.'))
        {
            if (!connect_src_to_block_port(left, right))
                return 0;
        }
        else
        {
            if (!connect_src_to_var(left, right))
                return 0;
        }
        return 1;
    }
    fprintf(stderr, "Unknown directive: %s\n", head);
    return 0;
}

static int parse_program(const char *src)
{
    g_var_count = 0;
    g_block_count = 0;
    char buf[512];
    const char *p = src;
    int line_no = 1;
    while (*p)
    {
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n < sizeof(buf) - 1)
            n++;
        memcpy(buf, p, n);
        buf[n] = '\0';
        if (p[n] == '\n')
            n++;
        p += n;
        if (!parse_line(buf))
        {
            fprintf(stderr, "Parse error at line %d: %s\n", line_no, buf);
            return 0;
        }
        line_no++;
    }
    if (!build_topology())
        return 0;
    return 1;
}

/* ---------- Demo network (seal-in motor + TON lamp) ---------- */
static const char *demo_fbd =
    "// Variables\n"
    "VAR BOOL Start = 0;\n"
    "VAR BOOL Stop  = 0;\n"
    "VAR BOOL Motor = 0;\n"
    "VAR BOOL Lamp  = 0;\n"
    "\n"
    "BLOCK not1 NOT;\n"
    "BLOCK and1 AND N=2;\n"
    "BLOCK sr1  SR;\n"
    "BLOCK t1   TON PT=2000;\n"
    "\n"
    "CONNECT Stop -> not1.IN;\n"
    "CONNECT Start -> and1.IN1;\n"
    "CONNECT not1.OUT -> and1.IN2;\n"
    "CONNECT and1.OUT -> sr1.S;\n"
    "CONNECT Stop -> sr1.R;\n"
    "CONNECT sr1.Q -> Motor;\n"
    "\n"
    "CONNECT and1.OUT -> t1.IN;\n"
    "CONNECT t1.Q -> Lamp;\n";

/* ---------- Main: simulate a few seconds ---------- */
int main(void)
{
    if (!parse_program(demo_fbd))
    {
        fprintf(stderr, "Failed to parse FBD program.\n");
        return 1;
    }
    int idxStart = var_index("Start");
    int idxStop = var_index("Stop");

    uint32_t t = 0;
    for (int step = 0; step < 30; ++step)
    {
        if (t >= 100 && t < 1500)
            g_vars[idxStart].val = make_bool(true);
        else
            g_vars[idxStart].val = make_bool(false);
        if (t >= 1500)
            g_vars[idxStop].val = make_bool(true);
        else
            g_vars[idxStop].val = make_bool(false);
        fbd_scan(100); /* 100 ms scan time */
        printf("t=%4ums  ", t);
        print_vars();
        t += 100;
    }
    return 0;
}
