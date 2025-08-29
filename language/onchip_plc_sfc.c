/* plc_sfc.c — Sequential Function Chart (SFC) interpreter (single file)
 *
 * Features
 *  - Tiny SFC DSL parser (VAR/END_VAR, STEP, INITIAL, ACTION ... DO ..., TRANS A -> B IF expr)
 *  - Multiple active steps supported
 *  - Boolean variables only (TRUE/FALSE)
 *  - Boolean expressions: NOT, AND, OR, parentheses
 *  - Actions execute every scan while the owning step is active ("DO")
 *  - Deterministic scan: evaluate all transitions, then apply, then run actions
 *
 * Build:  gcc -std=c99 -O2 -Wall plc_sfc.c -o plc_sfc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ---------------- Limits ---------------- */
#define NAME_LEN 64
#define MAX_VARS 256
#define MAX_STEPS 64
#define MAX_TRANS 256
#define MAX_ACTIONS 512
#define EXPR_LEN 256

/* ---------------- Utilities ---------------- */
static int ieq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}
static void trim(char *s)
{
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n && isspace((unsigned char)s[i]))
        i++;
    size_t j = n;
    while (j > i && isspace((unsigned char)s[j - 1]))
        j--;
    if (i > 0)
        memmove(s, s + i, j - i);
    s[j - i] = '\0';
}
static void strip_comment(char *s)
{
    for (size_t i = 0; s[i]; ++i)
    {
        if ((s[i] == '/' && s[i + 1] == '/') || s[i] == ';')
        {
            s[i] = '\0';
            break;
        }
    }
    trim(s);
}

/* ---------------- Variables ---------------- */

typedef struct
{
    char name[NAME_LEN];
    bool val;
} Var;
static Var g_vars[MAX_VARS];
static int g_varc = 0;

static int var_lookup(const char *name)
{
    for (int i = 0; i < g_varc; i++)
        if (ieq(g_vars[i].name, name))
            return i;
    return -1;
}
static int var_ensure(const char *name, bool init)
{
    int i = var_lookup(name);
    if (i >= 0)
        return i;
    if (g_varc >= MAX_VARS)
    {
        fprintf(stderr, "Var table full\n");
        exit(1);
    }
    strncpy(g_vars[g_varc].name, name, NAME_LEN - 1);
    g_vars[g_varc].name[NAME_LEN - 1] = '\0';
    g_vars[g_varc].val = init;
    return g_varc++;
}

/* ---------------- Steps & Transitions & Actions ---------------- */

typedef struct
{
    char name[NAME_LEN];
    bool active;
} Step;
static Step g_steps[MAX_STEPS];
static int g_stepc = 0;
static int step_lookup(const char *name)
{
    for (int i = 0; i < g_stepc; i++)
        if (ieq(g_steps[i].name, name))
            return i;
    return -1;
}
static int step_ensure(const char *name)
{
    int i = step_lookup(name);
    if (i >= 0)
        return i;
    if (g_stepc >= MAX_STEPS)
    {
        fprintf(stderr, "Too many steps\n");
        exit(1);
    }
    strncpy(g_steps[g_stepc].name, name, NAME_LEN - 1);
    g_steps[g_stepc].name[NAME_LEN - 1] = '\0';
    g_steps[g_stepc].active = false;
    return g_stepc++;
}

typedef struct
{
    int from, to;
    char expr[EXPR_LEN];
} Transition;
static Transition g_trans[MAX_TRANS];
static int g_transc = 0;

typedef struct
{
    int step;
    int var;
    char expr[EXPR_LEN];
} Action;
static Action g_actions[MAX_ACTIONS];
static int g_actionc = 0;

/* ---------------- Expression Parser ---------------- */

typedef enum
{
    TK_EOF = 0,
    TK_ID,
    TK_TRUE,
    TK_FALSE,
    TK_NOT,
    TK_AND,
    TK_OR,
    TK_LP,
    TK_RP
} Tok;

typedef struct
{
    const char *p;
} ExprLex;
static void el_init(ExprLex *L, const char *s) { L->p = s; }
static void el_skip(ExprLex *L)
{
    while (*L->p && isspace((unsigned char)*L->p))
        L->p++;
}
static Tok el_next(ExprLex *L, char *lex)
{
    el_skip(L);
    if (!*L->p)
    {
        lex[0] = '\0';
        return TK_EOF;
    }
    const char *s = L->p;
    char c = *s;
    if (isalpha((unsigned char)c) || c == '_')
    {
        int j = 0;
        while ((isalnum((unsigned char)*L->p) || *L->p == '_') && j < NAME_LEN - 1)
            lex[j++] = *L->p++;
        lex[j] = '\0';
        char U[NAME_LEN];
        for (int k = 0; k <= j; k++)
            U[k] = (char)toupper((unsigned char)lex[k]);
        if (!strcmp(U, "TRUE"))
            return TK_TRUE;
        if (!strcmp(U, "FALSE"))
            return TK_FALSE;
        if (!strcmp(U, "NOT"))
            return TK_NOT;
        if (!strcmp(U, "AND"))
            return TK_AND;
        if (!strcmp(U, "OR"))
            return TK_OR;
        return TK_ID;
    }
    L->p++;
    switch (c)
    {
    case '(':
        lex[0] = '(';
        lex[1] = '\0';
        return TK_LP;
    case ')':
        lex[0] = ')';
        lex[1] = '\0';
        return TK_RP;
    default:
        lex[0] = c;
        lex[1] = '\0';
        return TK_EOF;
    }
}

typedef struct
{
    ExprLex L;
    Tok cur;
    char lex[NAME_LEN];
} ExprP;
static void ep_init(ExprP *P, const char *s)
{
    el_init(&P->L, s);
    P->cur = el_next(&P->L, P->lex);
}
static void ep_eat(ExprP *P, Tok k)
{
    if (P->cur != k)
    {
        fprintf(stderr, "Expr parse error near '%s'\n", P->lex);
        exit(1);
    }
    P->cur = el_next(&P->L, P->lex);
}

static bool parse_expr_or(ExprP *P);
static bool parse_expr_primary(ExprP *P)
{
    if (P->cur == TK_TRUE)
    {
        ep_eat(P, TK_TRUE);
        return true;
    }
    if (P->cur == TK_FALSE)
    {
        ep_eat(P, TK_FALSE);
        return false;
    }
    if (P->cur == TK_ID)
    {
        int vi = var_ensure(P->lex, false);
        bool v = g_vars[vi].val;
        ep_eat(P, TK_ID);
        return v;
    }
    if (P->cur == TK_LP)
    {
        ep_eat(P, TK_LP);
        bool v = parse_expr_or(P);
        ep_eat(P, TK_RP);
        return v;
    }
    fprintf(stderr, "Expr expected primary near '%s'\n", P->lex);
    exit(1);
}
static bool parse_expr_unary(ExprP *P)
{
    if (P->cur == TK_NOT)
    {
        ep_eat(P, TK_NOT);
        return !parse_expr_unary(P);
    }
    return parse_expr_primary(P);
}
static bool parse_expr_and(ExprP *P)
{
    bool v = parse_expr_unary(P);
    while (P->cur == TK_AND)
    {
        ep_eat(P, TK_AND);
        v = v && parse_expr_unary(P);
    }
    return v;
}
static bool parse_expr_or(ExprP *P)
{
    bool v = parse_expr_and(P);
    while (P->cur == TK_OR)
    {
        ep_eat(P, TK_OR);
        v = v || parse_expr_and(P);
    }
    return v;
}
static bool eval_expr(const char *s)
{
    ExprP P;
    ep_init(&P, s);
    bool v = parse_expr_or(&P);
    if (P.cur != TK_EOF)
    { /* trailing tokens OK if only spaces */
    }
    return v;
}

/* ---------------- Parser for SFC DSL ---------------- */

static void parse_var_line(char *line)
{
    // Format:  Name : BOOL := TRUE|FALSE
    //          Name : BOOL            (defaults to FALSE)
    char name[NAME_LEN] = {0};
    char type[NAME_LEN] = {0};
    char init[NAME_LEN] = {0};
    // Replace ":=" with space to ease sscanf
    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char *p = buf; *p; ++p)
    {
        if (p[0] == ':' && p[1] == '=')
        {
            p[0] = ' ';
            p[1] = ' ';
        }
    }
    int n = sscanf(buf, "%63s : %63s %63s", name, type, init);
    if (n >= 2)
    {
        bool b = false;
        if (n == 3)
        {
            b = (ieq(init, "TRUE") || !strcmp(init, "1"));
        }
        if (!ieq(type, "BOOL"))
        {
            fprintf(stderr, "Only BOOL supported in VAR: %s\n", line);
            exit(1);
        }
        var_ensure(name, b);
    }
}

static void parse_program(const char *src)
{
    char line[512];
    const char *p = src;
    int in_var = 0;
    while (*p)
    {
        // read line
        size_t k = 0;
        while (p[k] && p[k] != '\n' && k < sizeof(line) - 1)
        {
            line[k] = p[k];
            k++;
        }
        line[k] = '\0';
        p += (p[k] == '\n' ? k + 1 : k);
        strip_comment(line);
        if (!*line)
            continue;

        if (in_var)
        {
            if (ieq(line, "END_VAR"))
            {
                in_var = 0;
                continue;
            }
            parse_var_line(line);
            continue;
        }
        if (ieq(line, "VAR"))
        {
            in_var = 1;
            continue;
        }

        if (!strncasecmp(line, "STEP", 4))
        {
            char nm[NAME_LEN] = {0};
            if (sscanf(line + 4, " %63[^;]", nm) == 1)
            {
                trim(nm);
                step_ensure(nm);
            }
            continue;
        }
        if (!strncasecmp(line, "INITIAL", 7))
        {
            char nm[NAME_LEN] = {0};
            if (sscanf(line + 7, " %63[^;]", nm) == 1)
            {
                trim(nm);
                int si = step_ensure(nm);
                g_steps[si].active = true;
            }
            continue;
        }
        if (!strncasecmp(line, "ACTION", 6))
        {
            // ACTION <step> DO <lhs> := <expr>
            char step[NAME_LEN] = {0};
            const char *q = strstr(line, "DO");
            if (!q)
            {
                fprintf(stderr, "ACTION missing DO: %s\n", line);
                exit(1);
            }
            if (sscanf(line + 6, " %63s", step) != 1)
            {
                fprintf(stderr, "ACTION missing step: %s\n", line);
                exit(1);
            }
            char assign[EXPR_LEN] = {0};
            strncpy(assign, q + 2, EXPR_LEN - 1);
            trim(assign);
            // split around ':='
            char *c = strstr(assign, ":=");
            if (!c)
            {
                fprintf(stderr, "ACTION requires assignment: %s\n", line);
                exit(1);
            }
            *c = '\0';
            char lhs[NAME_LEN] = {0};
            strncpy(lhs, assign, NAME_LEN - 1);
            trim(lhs);
            char rhs[EXPR_LEN] = {0};
            strncpy(rhs, c + 2, EXPR_LEN - 1);
            trim(rhs);
            int si = step_ensure(step);
            int vi = var_ensure(lhs, false);
            if (g_actionc >= MAX_ACTIONS)
            {
                fprintf(stderr, "Too many actions\n");
                exit(1);
            }
            g_actions[g_actionc].step = si;
            g_actions[g_actionc].var = vi;
            strncpy(g_actions[g_actionc].expr, rhs, EXPR_LEN - 1);
            g_actionc++;
            continue;
        }
        if (!strncasecmp(line, "TRANS", 5))
        {
            // TRANS A -> B IF <expr>
            char from[NAME_LEN] = {0}, to[NAME_LEN] = {0};
            const char *arrow = strstr(line, "->");
            const char *iff = strcasestr(line, "IF");
            if (!arrow || !iff || arrow < line + 5)
            {
                fprintf(stderr, "TRANS syntax: %s\n", line);
                exit(1);
            }
            strncpy(from, line + 5, (size_t)(arrow - (line + 5)));
            from[arrow - (line + 5)] = '\0';
            trim(from);
            strncpy(to, arrow + 2, (size_t)(iff - (arrow + 2)));
            to[iff - (arrow + 2)] = '\0';
            trim(to);
            char cond[EXPR_LEN] = {0};
            strncpy(cond, iff + 2, EXPR_LEN - 1);
            trim(cond);
            int fi = step_ensure(from), ti = step_ensure(to);
            if (g_transc >= MAX_TRANS)
            {
                fprintf(stderr, "Too many transitions\n");
                exit(1);
            }
            g_trans[g_transc].from = fi;
            g_trans[g_transc].to = ti;
            strncpy(g_trans[g_transc].expr, cond, EXPR_LEN - 1);
            g_transc++;
            continue;
        }
        fprintf(stderr, "Unknown directive: %s\n", line);
        exit(1);
    }
}

/* ---------------- Execution ---------------- */

static void sfc_scan(void)
{
    bool act[MAX_STEPS] = {0};
    bool deact[MAX_STEPS] = {0};
    // Evaluate transitions from currently active steps
    for (int i = 0; i < g_transc; i++)
    {
        int from = g_trans[i].from, to = g_trans[i].to;
        if (g_steps[from].active)
        {
            if (eval_expr(g_trans[i].expr))
            {
                act[to] = true;
                deact[from] = true;
            }
        }
    }
    // Apply changes
    for (int i = 0; i < g_stepc; i++)
    {
        if (deact[i])
            g_steps[i].active = false;
        if (act[i])
            g_steps[i].active = true;
    }
    // Execute actions for active steps
    for (int i = 0; i < g_actionc; i++)
    {
        if (g_steps[g_actions[i].step].active)
        {
            bool v = eval_expr(g_actions[i].expr);
            g_vars[g_actions[i].var].val = v;
        }
    }
}

static void print_state(int t)
{
    printf("t=%d  Steps:", t);
    for (int i = 0; i < g_stepc; i++)
        if (g_steps[i].active)
            printf(" %s", g_steps[i].name);
    printf("  |  Vars:");
    for (int i = 0; i < g_varc; i++)
        printf(" %s=%s", g_vars[i].name, g_vars[i].val ? "TRUE" : "FALSE");
    printf("\n");
}

/* ---------------- Embedded Demo Program ---------------- */
static const char *demo_program =
    "// Demo SFC: Idle -> Heat -> Cook -> Done based on Start/TempOK/TimerDone\n"
    "VAR\n"
    "  Start : BOOL := FALSE;\n"
    "  TempOK : BOOL := FALSE;\n"
    "  TimerDone : BOOL := FALSE;\n"
    "  Heater : BOOL := FALSE;\n"
    "  Motor  : BOOL := FALSE;\n"
    "  Valve  : BOOL := FALSE;\n"
    "END_VAR\n"
    "\n"
    "STEP Idle;\n"
    "STEP Heat;\n"
    "STEP Cook;\n"
    "STEP Done;\n"
    "INITIAL Idle;\n"
    "\n"
    "TRANS Idle -> Heat IF Start;\n"
    "TRANS Heat -> Cook IF TempOK;\n"
    "TRANS Cook -> Done IF TimerDone;\n"
    "\n"
    "ACTION Heat DO Heater := TRUE;\n"
    "ACTION Heat DO Motor  := FALSE;\n"
    "ACTION Heat DO Valve  := FALSE;\n"
    "\n"
    "ACTION Cook DO Heater := FALSE;\n"
    "ACTION Cook DO Motor  := TRUE;\n"
    "\n"
    "ACTION Done DO Motor  := FALSE;\n"
    "ACTION Done DO Valve  := TRUE;\n";

int main(void)
{
    parse_program(demo_program);

    // Simulate 10 scans; toggle inputs over time
    int idxStart = var_ensure("Start", false);
    int idxTemp = var_ensure("TempOK", false);
    int idxTimer = var_ensure("TimerDone", false);

    for (int t = 0; t < 10; t++)
    {
        // Drive inputs
        if (t == 1)
            g_vars[idxStart].val = true; // Start pressed
        if (t == 3)
            g_vars[idxTemp].val = true; // Temperature ok
        if (t == 6)
            g_vars[idxTimer].val = true; // Timer elapsed

        sfc_scan();
        print_state(t);
    }
    return 0;
}
