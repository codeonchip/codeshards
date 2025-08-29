/* plc_st.c - Structured Text (ST) interpreter (compact single-file)
 * Supports:
 *  - BOOL variables (VAR ... END_VAR), optional initializers (:= TRUE/FALSE)
 *  - Assignments (:=)
 *  - Expressions with precedence and parentheses:  NOT > AND > OR
 *  - Literals: TRUE/FALSE
 *  - IF ... THEN ... [ELSE ...] END_IF;
 *
 * Notes:
 *  - Extend easily with INT/REAL by expanding Value/lexer and arithmetic rules.
 *
 * Build:  gcc -std=c99 -O2 -Wall plc_st.c -o plc_st
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* -------------------- Value & Symbol Table -------------------- */

typedef enum
{
    VT_BOOL = 0
} VType;

typedef struct
{
    VType t;
    union
    {
        bool b;
    } v;
} Value;

static Value make_bool(bool b)
{
    Value x;
    x.t = VT_BOOL;
    x.v.b = b;
    return x;
}

#define MAX_VARS 256
#define NAME_LEN 64

typedef struct
{
    char name[NAME_LEN];
    VType t;
    Value val;
} Var;

static Var g_vars[MAX_VARS];
static int g_varc = 0;

static int sym_lookup(const char *name)
{
    for (int i = 0; i < g_varc; ++i)
        if (strcasecmp(g_vars[i].name, name) == 0)
            return i;
    return -1;
}
static int sym_ensure(const char *name, VType t, Value init)
{
    int i = sym_lookup(name);
    if (i >= 0)
    {
        g_vars[i].t = t;
        return i;
    }
    if (g_varc >= MAX_VARS)
    {
        fprintf(stderr, "symbol table full\n");
        exit(1);
    }
    strncpy(g_vars[g_varc].name, name, NAME_LEN - 1);
    g_vars[g_varc].name[NAME_LEN - 1] = '\0';
    g_vars[g_varc].t = t;
    g_vars[g_varc].val = init;
    return g_varc++;
}

/* -------------------- Lexer -------------------- */

typedef enum
{
    T_EOF = 0,
    T_ID,
    T_TRUE,
    T_FALSE,
    T_VAR,
    T_END_VAR,
    T_BOOL,
    T_IF,
    T_THEN,
    T_ELSE,
    T_END_IF,
    T_AND,
    T_OR,
    T_NOT,
    T_ASSIGN, /* := */
    T_SEMI,   /* ; */
    T_COLON,  /* : */
    T_LPAREN,
    T_RPAREN,
} Tok;

typedef struct
{
    Tok k;
    char lex[NAME_LEN];
} Token;

typedef struct
{
    const char *src;
    size_t i;
    size_t n;
    int line;
} Lexer;

static void lx_init(Lexer *L, const char *s)
{
    L->src = s;
    L->i = 0;
    L->n = strlen(s);
    L->line = 1;
}

static int lx_peek(Lexer *L) { return (L->i < L->n) ? (unsigned char)L->src[L->i] : -1; }
static int lx_get(Lexer *L)
{
    if (L->i >= L->n)
        return -1;
    int c = (unsigned char)L->src[L->i++];
    if (c == '\n')
        L->line++;
    return c;
}
static void lx_skip_ws_comm(Lexer *L)
{
    for (;;)
    {
        while (isspace(lx_peek(L)))
            lx_get(L);
        if (lx_peek(L) == '/' && L->i + 1 < L->n && L->src[L->i + 1] == '/')
        {
            while (lx_peek(L) != -1 && lx_get(L) != '\n')
                ;
            continue;
        }
        if (lx_peek(L) == '(' && L->i + 1 < L->n && L->src[L->i + 1] == '*')
        {
            L->i += 2; // skip '(*'
            while (L->i + 1 < L->n && !(L->src[L->i] == '*' && L->src[L->i + 1] == ')'))
            {
                if (lx_get(L) == -1)
                    break;
            }
            if (L->i + 1 < L->n)
                L->i += 2; // skip '*)'
            continue;
        }
        break;
    }
}

static int isid1(int c) { return isalpha(c) || c == '_'; }
static int isidn(int c) { return isalnum(c) || c == '_'; }

static Token lx_next(Lexer *L)
{
    lx_skip_ws_comm(L);
    Token t;
    t.k = T_EOF;
    t.lex[0] = '\0';
    int c = lx_peek(L);
    if (c == -1)
        return t;
    if (isid1(c))
    {
        int j = 0;
        while (isidn(lx_peek(L)) && j < NAME_LEN - 1)
        {
            t.lex[j++] = (char)lx_get(L);
        }
        t.lex[j] = '\0';
        char U[NAME_LEN];
        for (int k = 0; k <= j; k++)
            U[k] = (char)toupper((unsigned char)t.lex[k]);
        if (!strcmp(U, "VAR"))
            t.k = T_VAR;
        else if (!strcmp(U, "END_VAR"))
            t.k = T_END_VAR;
        else if (!strcmp(U, "BOOL"))
            t.k = T_BOOL;
        else if (!strcmp(U, "IF"))
            t.k = T_IF;
        else if (!strcmp(U, "THEN"))
            t.k = T_THEN;
        else if (!strcmp(U, "ELSE"))
            t.k = T_ELSE;
        else if (!strcmp(U, "END_IF"))
            t.k = T_END_IF;
        else if (!strcmp(U, "AND"))
            t.k = T_AND;
        else if (!strcmp(U, "OR"))
            t.k = T_OR;
        else if (!strcmp(U, "NOT"))
            t.k = T_NOT;
        else if (!strcmp(U, "TRUE"))
            t.k = T_TRUE;
        else if (!strcmp(U, "FALSE"))
            t.k = T_FALSE;
        else
            t.k = T_ID;
        return t;
    }
    c = lx_get(L);
    switch (c)
    {
    case ':':
        if (lx_peek(L) == '=')
        {
            lx_get(L);
            t.k = T_ASSIGN;
            strcpy(t.lex, ":=");
        }
        else
        {
            t.k = T_COLON;
            t.lex[0] = ':';
            t.lex[1] = '\0';
        }
        break;
    case ';':
        t.k = T_SEMI;
        t.lex[0] = ';';
        t.lex[1] = '\0';
        break;
    case '(':
        t.k = T_LPAREN;
        t.lex[0] = '(';
        t.lex[1] = '\0';
        break;
    case ')':
        t.k = T_RPAREN;
        t.lex[0] = ')';
        t.lex[1] = '\0';
        break;
    default:
        fprintf(stderr, "Lex error line %d: unexpected '%c'\n", L->line, c);
        exit(1);
    }
    return t;
}

/* -------------------- Parser -------------------- */

typedef struct
{
    Lexer L;
    Token cur;
} Parser;

static void ps_init(Parser *P, const char *src)
{
    lx_init(&P->L, src);
    P->cur = lx_next(&P->L);
}
static void ps_eat(Parser *P, Tok k)
{
    if (P->cur.k != k)
    {
        fprintf(stderr, "Parse error line %d: expected %d got %d ('%s')\n", P->L.line, k, P->cur.k, P->cur.lex);
        exit(1);
    }
    P->cur = lx_next(&P->L);
}

/* Forward decls */
static Value parse_expr(Parser *P);

/* Primary: TRUE/FALSE/ID/(expr) */
static Value parse_primary(Parser *P)
{
    if (P->cur.k == T_TRUE)
    {
        ps_eat(P, T_TRUE);
        return make_bool(true);
    }
    if (P->cur.k == T_FALSE)
    {
        ps_eat(P, T_FALSE);
        return make_bool(false);
    }
    if (P->cur.k == T_ID)
    {
        char name[NAME_LEN];
        strncpy(name, P->cur.lex, NAME_LEN);
        ps_eat(P, T_ID);
        int i = sym_lookup(name);
        if (i < 0)
        {
            fprintf(stderr, "Undeclared identifier '%s'\n", name);
            exit(1);
        }
        return g_vars[i].val;
    }
    if (P->cur.k == T_LPAREN)
    {
        ps_eat(P, T_LPAREN);
        Value v = parse_expr(P);
        ps_eat(P, T_RPAREN);
        return v;
    }
    fprintf(stderr, "Parse error line %d: expected primary\n", P->L.line);
    exit(1);
}

/* Unary: NOT unary | primary */
static Value parse_unary(Parser *P)
{
    if (P->cur.k == T_NOT)
    {
        ps_eat(P, T_NOT);
        Value v = parse_unary(P);
        return make_bool(!v.v.b);
    }
    return parse_primary(P);
}

/* AND-chain */
static Value parse_and(Parser *P)
{
    Value v = parse_unary(P);
    while (P->cur.k == T_AND)
    {
        ps_eat(P, T_AND);
        Value r = parse_unary(P);
        v.v.b = v.v.b && r.v.b;
    }
    return v;
}

/* OR-chain */
static Value parse_or(Parser *P)
{
    Value v = parse_and(P);
    while (P->cur.k == T_OR)
    {
        ps_eat(P, T_OR);
        Value r = parse_and(P);
        v.v.b = v.v.b || r.v.b;
    }
    return v;
}

static Value parse_expr(Parser *P) { return parse_or(P); }

/* Statements */
static void parse_var_block(Parser *P)
{
    ps_eat(P, T_VAR);
    while (P->cur.k != T_END_VAR)
    {
        if (P->cur.k != T_ID)
        {
            fprintf(stderr, "VAR block expects identifier, line %d\n", P->L.line);
            exit(1);
        }
        char name[NAME_LEN];
        strncpy(name, P->cur.lex, NAME_LEN);
        ps_eat(P, T_ID);
        ps_eat(P, T_COLON);
        if (P->cur.k != T_BOOL)
        {
            fprintf(stderr, "Only BOOL supported (line %d)\n", P->L.line);
            exit(1);
        }
        ps_eat(P, T_BOOL);
        Value init = make_bool(false);
        if (P->cur.k == T_ASSIGN)
        {
            ps_eat(P, T_ASSIGN);
            Value v = parse_expr(P);
            init = v;
        }
        ps_eat(P, T_SEMI);
        sym_ensure(name, VT_BOOL, init);
    }
    ps_eat(P, T_END_VAR);
}

static void do_assign(const char *lhs, Value v)
{
    int i = sym_lookup(lhs);
    if (i < 0)
    {
        fprintf(stderr, "Assignment to undeclared '%s'\n", lhs);
        exit(1);
    }
    if (g_vars[i].t != VT_BOOL)
    {
        fprintf(stderr, "Type mismatch on '%s'\n", lhs);
        exit(1);
    }
    g_vars[i].val = v;
}

static void parse_if(Parser *P)
{
    ps_eat(P, T_IF);
    Value cond = parse_expr(P);
    ps_eat(P, T_THEN);
    int then_exec = cond.v.b ? 1 : 0;
    /* parse simple THEN part: either a single assignment or multiple separated by semicolons until ELSE/END_IF */
    while (P->cur.k != T_ELSE && P->cur.k != T_END_IF)
    {
        if (P->cur.k == T_ID)
        {
            char lhs[NAME_LEN];
            strncpy(lhs, P->cur.lex, NAME_LEN);
            ps_eat(P, T_ID);
            ps_eat(P, T_ASSIGN);
            Value rhs = parse_expr(P);
            ps_eat(P, T_SEMI);
            if (then_exec)
                do_assign(lhs, rhs);
        }
        else
        {
            fprintf(stderr, "Expected statement in THEN at line %d\n", P->L.line);
            exit(1);
        }
    }
    int else_exec = !then_exec;
    if (P->cur.k == T_ELSE)
    {
        ps_eat(P, T_ELSE);
        while (P->cur.k != T_END_IF)
        {
            if (P->cur.k == T_ID)
            {
                char lhs[NAME_LEN];
                strncpy(lhs, P->cur.lex, NAME_LEN);
                ps_eat(P, T_ID);
                ps_eat(P, T_ASSIGN);
                Value rhs = parse_expr(P);
                ps_eat(P, T_SEMI);
                if (else_exec)
                    do_assign(lhs, rhs);
            }
            else
            {
                fprintf(stderr, "Expected statement in ELSE at line %d\n", P->L.line);
                exit(1);
            }
        }
    }
    ps_eat(P, T_END_IF);
    ps_eat(P, T_SEMI);
}

static void parse_stmt(Parser *P)
{
    if (P->cur.k == T_VAR)
    {
        parse_var_block(P);
        return;
    }
    if (P->cur.k == T_IF)
    {
        parse_if(P);
        return;
    }
    if (P->cur.k == T_ID)
    {
        char lhs[NAME_LEN];
        strncpy(lhs, P->cur.lex, NAME_LEN);
        ps_eat(P, T_ID);
        ps_eat(P, T_ASSIGN);
        Value rhs = parse_expr(P);
        ps_eat(P, T_SEMI);
        do_assign(lhs, rhs);
        return;
    }
    if (P->cur.k == T_EOF)
        return;
    fprintf(stderr, "Unexpected token at line %d ('%s')\n", P->L.line, P->cur.lex);
    exit(1);
}

static void parse_program(Parser *P)
{
    while (P->cur.k != T_EOF)
    {
        parse_stmt(P);
    }
}

/* -------------------- Demo Program -------------------- */
static const char *demo_program =
    "// Minimal ST demo with BOOLs and IF logic\n"
    "VAR\n"
    "    Start : BOOL := FALSE;\n"
    "    Stop  : BOOL := FALSE;\n"
    "    Motor : BOOL := FALSE;\n"
    "    Lamp  : BOOL := FALSE;\n"
    "END_VAR\n"
    "\n"
    "IF Start AND NOT Stop THEN\n"
    "    Motor := TRUE;\n"
    "ELSE\n"
    "    Motor := FALSE;\n"
    "END_IF;\n"
    "\n"
    "Lamp := Motor;\n";

static void print_vars(void)
{
    printf("Vars: ");
    for (int i = 0; i < g_varc; i++)
    {
        if (g_vars[i].t == VT_BOOL)
            printf("%s=%s ", g_vars[i].name, g_vars[i].val.v.b ? "TRUE" : "FALSE");
    }
    printf("\n");
}

int main(void)
{
    /* Simulate several cycles where inputs change, re-running the program each cycle */
    for (int t = 0; t < 10; t++)
    {
        /* Re-parse each scan so expressions use current inputs */
        Parser P;
        ps_init(&P, demo_program);
        parse_program(&P);
        // Modify inputs after parse (simulate external IO updates)
        int iStart = sym_lookup("Start");
        int iStop = sym_lookup("Stop");
        if (iStart < 0 || iStop < 0)
        {
            fprintf(stderr, "demo variables missing\n");
            return 1;
        }
        g_vars[iStart].val = make_bool(t >= 2 && t < 7); // Start pressed between scans 2..6
        g_vars[iStop].val = make_bool(t >= 7);           // Stop pressed from scan 7
        // Re-run logic with updated IO
        Parser P2;
        ps_init(&P2, demo_program);
        parse_program(&P2);
        printf("t=%d  ", t);
        print_vars();
    }
    return 0;
}
