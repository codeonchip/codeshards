/* kestrel.c — Tiny scripting language with an embedded demo program.
 *
 * Features:
 *  - int (32-bit) and bool (true/false)
 *  - let bindings, assignments
 *  - if/else, while, blocks { ... }
 *  - print(expr);
 *
 * Design:
 *  - Single-pass lexer + Pratt parser for expressions
 *  - Statement execution without an AST (exec/skip walkers)
 *  - Fixed-size tables, no malloc
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---------- Tunables / Limits ---------- */
#define SRC_MAX 65536u
#define TOK_MAX 8192u
#define NAME_MAX 32u
#define VAR_MAX 512u

/* ---------- Tokenization ---------- */
typedef enum
{
    T_EOF = 0,
    T_INT,
    T_TRUE,
    T_FALSE,
    T_IDENT,
    /* keywords */
    T_LET,
    T_IF,
    T_ELSE,
    T_WHILE,
    T_PRINT,
    /* punctuation/operators */
    T_LPAREN,
    T_RPAREN,
    T_LBRACE,
    T_RBRACE,
    T_SEMI,
    T_ASSIGN, /* ;  = */
    T_BANG,
    T_PLUS,
    T_MINUS,
    T_STAR,
    T_SLASH,
    T_PERCENT, /* ! + - * / % */
    T_LT,
    T_LE,
    T_GT,
    T_GE, /* < <= > >= */
    T_EQ,
    T_NE, /* == != */
    T_AND,
    T_OR /* && || */
} TokKind;

typedef struct
{
    TokKind kind;
    const char *start; /* pointer into source buffer */
    uint32_t len;      /* length of lexeme */
    int32_t ival;      /* for T_INT, TRUE/FALSE */
} Token;

static const char *g_src = NULL;
static uint32_t g_len = 0u;
static uint32_t g_pos = 0u;
static Token g_toks[TOK_MAX];
static uint32_t g_ntok = 0u;

static void die(const char *msg)
{
    fprintf(stderr, "Error: %s\n", msg);
    exit(1);
}

static bool is_ident_start(char c)
{
    return (bool)(isalpha((unsigned char)c) || (c == '_'));
}
static bool is_ident_char(char c)
{
    return (bool)(isalnum((unsigned char)c) || (c == '_'));
}

static void skip_ws_and_comments(void)
{
    for (;;)
    {
        if (g_pos >= g_len)
        {
            return;
        }
        char c = g_src[g_pos];

        if ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n'))
        {
            g_pos++;
            continue;
        }
        /* // line comment */
        if ((c == '/') && (g_pos + 1u < g_len) && (g_src[g_pos + 1u] == '/'))
        {
            g_pos += 2u;
            while ((g_pos < g_len) && (g_src[g_pos] != '\n'))
            {
                g_pos++;
            }
            continue;
        }
        /* /* block comment * / */
        if ((c == '/') && (g_pos + 1u < g_len) && (g_src[g_pos + 1u] == '*'))
        {
            g_pos += 2u;
            while (g_pos + 1u < g_len)
            {
                if ((g_src[g_pos] == '*') && (g_src[g_pos + 1u] == '/'))
                {
                    g_pos += 2u;
                    break;
                }
                g_pos++;
            }
            continue;
        }
        break;
    }
}

static bool match2(char a, char b)
{
    if ((g_pos + 1u < g_len) && (g_src[g_pos] == a) && (g_src[g_pos + 1u] == b))
    {
        g_pos += 2u;
        return true;
    }
    return false;
}

static void emit(TokKind k, const char *s, uint32_t n, int32_t ival)
{
    if (g_ntok >= TOK_MAX)
    {
        die("too many tokens");
    }
    g_toks[g_ntok].kind = k;
    g_toks[g_ntok].start = s;
    g_toks[g_ntok].len = n;
    g_toks[g_ntok].ival = ival;
    g_ntok++;
}

static void lex(void)
{
    g_ntok = 0u;
    while (g_pos < g_len)
    {
        skip_ws_and_comments();
        if (g_pos >= g_len)
        {
            break;
        }
        const char *st = &g_src[g_pos];
        char c = *st;

        /* numbers */
        if (isdigit((unsigned char)c))
        {
            int32_t v = 0;
            uint32_t n = 0u;
            while ((g_pos < g_len) && isdigit((unsigned char)g_src[g_pos]))
            {
                int d = (int)(g_src[g_pos] - '0');
                v = (v * 10) + d;
                g_pos++;
                n++;
            }
            emit(T_INT, st, n, v);
            continue;
        }

        /* identifiers / keywords */
        if (is_ident_start(c))
        {
            uint32_t n = 0u;
            while ((g_pos < g_len) && is_ident_char(g_src[g_pos]))
            {
                g_pos++;
                n++;
            }
            if ((n == 3u) && (strncmp(st, "let", 3) == 0))
            {
                emit(T_LET, st, n, 0);
                continue;
            }
            if ((n == 2u) && (strncmp(st, "if", 2) == 0))
            {
                emit(T_IF, st, n, 0);
                continue;
            }
            if ((n == 4u) && (strncmp(st, "else", 4) == 0))
            {
                emit(T_ELSE, st, n, 0);
                continue;
            }
            if ((n == 5u) && (strncmp(st, "while", 5) == 0))
            {
                emit(T_WHILE, st, n, 0);
                continue;
            }
            if ((n == 5u) && (strncmp(st, "print", 5) == 0))
            {
                emit(T_PRINT, st, n, 0);
                continue;
            }
            if ((n == 4u) && (strncmp(st, "true", 4) == 0))
            {
                emit(T_TRUE, st, n, 1);
                continue;
            }
            if ((n == 5u) && (strncmp(st, "false", 5) == 0))
            {
                emit(T_FALSE, st, n, 0);
                continue;
            }
            emit(T_IDENT, st, n, 0);
            continue;
        }

        /* operators & punctuation */
        if (match2('&', '&'))
        {
            emit(T_AND, st, 2u, 0);
            continue;
        }
        if (match2('|', '|'))
        {
            emit(T_OR, st, 2u, 0);
            continue;
        }
        if (match2('=', '='))
        {
            emit(T_EQ, st, 2u, 0);
            continue;
        }
        if (match2('!', '='))
        {
            emit(T_NE, st, 2u, 0);
            continue;
        }
        if (match2('<', '='))
        {
            emit(T_LE, st, 2u, 0);
            continue;
        }
        if (match2('>', '='))
        {
            emit(T_GE, st, 2u, 0);
            continue;
        }

        g_pos++; /* single-char tokens */
        switch (c)
        {
        case '(':
            emit(T_LPAREN, st, 1u, 0);
            break;
        case ')':
            emit(T_RPAREN, st, 1u, 0);
            break;
        case '{':
            emit(T_LBRACE, st, 1u, 0);
            break;
        case '}':
            emit(T_RBRACE, st, 1u, 0);
            break;
        case ';':
            emit(T_SEMI, st, 1u, 0);
            break;
        case '=':
            emit(T_ASSIGN, st, 1u, 0);
            break;
        case '!':
            emit(T_BANG, st, 1u, 0);
            break;
        case '+':
            emit(T_PLUS, st, 1u, 0);
            break;
        case '-':
            emit(T_MINUS, st, 1u, 0);
            break;
        case '*':
            emit(T_STAR, st, 1u, 0);
            break;
        case '/':
            emit(T_SLASH, st, 1u, 0);
            break;
        case '%':
            emit(T_PERCENT, st, 1u, 0);
            break;
        case '<':
            emit(T_LT, st, 1u, 0);
            break;
        case '>':
            emit(T_GT, st, 1u, 0);
            break;
        default:
            die("unknown character");
        }
    }
    emit(T_EOF, "", 0u, 0);
}

/* ---------- Parser / Evaluator (Pratt for expressions) ---------- */

static uint32_t g_ix = 0u;
static Token *cur(void) { return &g_toks[g_ix]; }
static bool accept(TokKind k)
{
    if (cur()->kind == k)
    {
        g_ix++;
        return true;
    }
    return false;
}
static void expect(TokKind k, const char *msg)
{
    if (!accept(k))
    {
        die(msg);
    }
}

/* Variables */
typedef struct
{
    char name[NAME_MAX];
    int32_t value;
    bool used;
} Var;
static Var g_vars[VAR_MAX];

static int find_var(const char *s, uint32_t n)
{
    for (int i = 0; i < (int)VAR_MAX; ++i)
    {
        if (g_vars[i].used)
        {
            size_t ln = strlen(g_vars[i].name);
            if ((ln == (size_t)n) && (strncmp(g_vars[i].name, s, (size_t)n) == 0))
            {
                return i;
            }
        }
    }
    return -1;
}
static int ensure_var(const char *s, uint32_t n)
{
    int idx = find_var(s, n);
    if (idx >= 0)
    {
        return idx;
    }
    for (int i = 0; i < (int)VAR_MAX; ++i)
    {
        if (!g_vars[i].used)
        {
            size_t cpy = (n < (NAME_MAX - 1u)) ? (size_t)n : (size_t)(NAME_MAX - 1u);
            memcpy(g_vars[i].name, s, cpy);
            g_vars[i].name[cpy] = '\0';
            g_vars[i].value = 0;
            g_vars[i].used = true;
            return i;
        }
    }
    die("too many variables");
    return -1;
}

/* Expression precedence */
typedef enum
{
    PREC_LOWEST = 0,
    PREC_OR,    /* || */
    PREC_AND,   /* && */
    PREC_EQ,    /* == != */
    PREC_REL,   /* < <= > >= */
    PREC_ADD,   /* + - */
    PREC_MUL,   /* * / % */
    PREC_UNARY, /* ! - */
    PREC_PRIMARY
} Prec;

static int32_t parse_expr_prec(Prec prec);

static int32_t eval_primary(void)
{
    Token *t = cur();
    if (accept(T_INT))
    {
        return t->ival;
    }
    if (accept(T_TRUE))
    {
        return 1;
    }
    if (accept(T_FALSE))
    {
        return 0;
    }

    if (accept(T_LPAREN))
    {
        int32_t v = parse_expr_prec(PREC_LOWEST);
        expect(T_RPAREN, "missing )");
        return v;
    }

    if (accept(T_IDENT))
    {
        int idx = ensure_var(t->start, t->len);
        return g_vars[idx].value;
    }

    die("expected primary expression");
    return 0;
}

static int32_t eval_unary(void)
{
    if (accept(T_BANG))
    {
        int32_t v = eval_unary();
        return (v == 0) ? 1 : 0;
    }
    if (accept(T_MINUS))
    {
        int32_t v = eval_unary();
        return -v;
    }
    return eval_primary();
}

static int precedence_of(TokKind k)
{
    switch (k)
    {
    case T_OR:
        return (int)PREC_OR;
    case T_AND:
        return (int)PREC_AND;
    case T_EQ:
    case T_NE:
        return (int)PREC_EQ;
    case T_LT:
    case T_LE:
    case T_GT:
    case T_GE:
        return (int)PREC_REL;
    case T_PLUS:
    case T_MINUS:
        return (int)PREC_ADD;
    case T_STAR:
    case T_SLASH:
    case T_PERCENT:
        return (int)PREC_MUL;
    default:
        return -1;
    }
}

static int32_t parse_expr_prec(Prec prec)
{
    int32_t lhs = eval_unary();

    for (;;)
    {
        TokKind k = cur()->kind;
        int p = precedence_of(k);
        if ((p < 0) || (p < (int)prec))
        {
            break;
        }

        Token *op = cur();
        g_ix++; /* consume operator */
        int32_t rhs = parse_expr_prec((Prec)(p + 1));

        switch (op->kind)
        {
        case T_PLUS:
            lhs = lhs + rhs;
            break;
        case T_MINUS:
            lhs = lhs - rhs;
            break;
        case T_STAR:
            lhs = lhs * rhs;
            break;
        case T_SLASH:
            if (rhs == 0)
            {
                die("division by zero");
            }
            lhs = lhs / rhs;
            break;
        case T_PERCENT:
            if (rhs == 0)
            {
                die("mod by zero");
            }
            lhs = lhs % rhs;
            break;

        case T_LT:
            lhs = (lhs < rhs) ? 1 : 0;
            break;
        case T_LE:
            lhs = (lhs <= rhs) ? 1 : 0;
            break;
        case T_GT:
            lhs = (lhs > rhs) ? 1 : 0;
            break;
        case T_GE:
            lhs = (lhs >= rhs) ? 1 : 0;
            break;
        case T_EQ:
            lhs = (lhs == rhs) ? 1 : 0;
            break;
        case T_NE:
            lhs = (lhs != rhs) ? 1 : 0;
            break;
        case T_AND:
            lhs = ((lhs != 0) && (rhs != 0)) ? 1 : 0;
            break;
        case T_OR:
            lhs = ((lhs != 0) || (rhs != 0)) ? 1 : 0;
            break;
        default:
            die("unexpected binary operator");
        }
    }
    return lhs;
}

static int32_t parse_expr(void)
{
    return parse_expr_prec(PREC_LOWEST);
}

/* ---------- Statement execution without AST ---------- */

static void exec_or_skip_block(bool execute);

static void exec_or_skip_stmt(bool execute)
{
    /* let IDENT = expr ; */
    if (accept(T_LET))
    {
        Token *id = cur();
        expect(T_IDENT, "expected identifier after let");
        int idx = ensure_var(id->start, id->len);
        expect(T_ASSIGN, "missing '=' after identifier");
        int32_t val = parse_expr();
        expect(T_SEMI, "missing ';' after expression");
        if (execute)
        {
            g_vars[idx].value = val;
        }
        return;
    }

    /* if (...) block [ else block ] */
    if (accept(T_IF))
    {
        expect(T_LPAREN, "missing '(' after if");
        int32_t cond = parse_expr();
        expect(T_RPAREN, "missing ')' after if condition");

        if (cond != 0)
        {
            exec_or_skip_block(execute);
        }
        else
        {
            exec_or_skip_block(false);
        }

        if (accept(T_ELSE))
        {
            if (cond == 0)
            {
                exec_or_skip_block(execute);
            }
            else
            {
                exec_or_skip_block(false);
            }
        }
        return;
    }

    /* while (...) { ... } */
    if (accept(T_WHILE))
    {
        expect(T_LPAREN, "missing '(' after while");
        uint32_t cond_pos = g_ix;
        int32_t cond = parse_expr();
        expect(T_RPAREN, "missing ')' after while condition");
        expect(T_LBRACE, "missing '{' after while(...)");

        /* find end of block */
        int depth = 1;
        uint32_t body_start = g_ix;
        uint32_t scan = g_ix;
        while ((scan < g_ntok) && (depth > 0))
        {
            TokKind k = g_toks[scan].kind;
            if (k == T_LBRACE)
            {
                depth++;
            }
            else if (k == T_RBRACE)
            {
                depth--;
            }
            scan++;
        }
        if (depth != 0)
        {
            die("unclosed { in while");
        }
        uint32_t body_end = scan - 1u;

        if (execute)
        {
            while (cond != 0)
            {
                /* execute body once */
                uint32_t save_ix = g_ix;
                g_ix = body_start;
                while (g_ix < body_end)
                {
                    exec_or_skip_stmt(true);
                }
                g_ix = save_ix;

                /* re-evaluate condition */
                uint32_t save_ix2 = g_ix;
                g_ix = cond_pos;
                cond = parse_expr();
                g_ix = save_ix2;
            }
        }

        /* move past body */
        g_ix = body_end + 1u;
        return;
    }

    /* print(expr); */
    if (accept(T_PRINT))
    {
        expect(T_LPAREN, "missing '(' after print");
        int32_t v = parse_expr();
        expect(T_RPAREN, "missing ')' after print(expr)");
        expect(T_SEMI, "missing ';' after print(...)");
        if (execute)
        {
            if (v == 0)
            {
                printf("false\n");
            }
            else if (v == 1)
            {
                printf("true\n");
            }
            else
            {
                printf("%d\n", v);
            }
        }
        return;
    }

    /* block or single statement as block */
    if (accept(T_LBRACE))
    {
        while (!accept(T_RBRACE))
        {
            exec_or_skip_stmt(execute);
        }
        return;
    }

    /* IDENT = expr ; */
    if (cur()->kind == T_IDENT)
    {
        Token *id = cur();
        g_ix++;
        int idx = ensure_var(id->start, id->len);
        expect(T_ASSIGN, "missing '=' in assignment");
        int32_t val = parse_expr();
        expect(T_SEMI, "missing ';' after assignment");
        if (execute)
        {
            g_vars[idx].value = val;
        }
        return;
    }

    die("unexpected statement");
}

static void exec_or_skip_block(bool execute)
{
    if (accept(T_LBRACE))
    {
        while (!accept(T_RBRACE))
        {
            exec_or_skip_stmt(execute);
        }
    }
    else
    {
        exec_or_skip_stmt(execute);
    }
}

/* ---------- Embedded demo program ---------- */

static const char *demo_program =
    "// Kestrel demo: basic control logic\n"
    "let x = 0;\n"
    "let sum = 0;\n"
    "while (x < 10) {\n"
    "  sum = sum + x;\n"
    "  x = x + 1;\n"
    "}\n"
    "print(sum);         // 45\n"
    "if (sum >= 45) { print(true); } else { print(false); }\n"
    "let a = 3; let b = 4; print(a*a + b*b); // 25\n"
    "let t = (1 < 2) && (3 < 1); print(t);   // false\n";

/* ---------- Frontend ---------- */

static char g_buf[SRC_MAX];

static void run_source(const char *src)
{
    g_src = src;
    g_len = (uint32_t)strlen(src);
    g_pos = 0u;
    lex();
    g_ix = 0u;
    memset(g_vars, 0, sizeof(g_vars));
    while (cur()->kind != T_EOF)
    {
        exec_or_skip_stmt(true);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2)
    {
        FILE *f = fopen(argv[1], "rb");
        if (f == NULL)
        {
            fprintf(stderr, "cannot open %s\n", argv[1]);
            return 1;
        }
        size_t n = fread(g_buf, 1, (size_t)(SRC_MAX - 1u), f);
        fclose(f);
        g_buf[n] = '\0';
        run_source(g_buf);
    }
    else
    {
        puts("== Kestrel demo ==");
        run_source(demo_program);
    }
    return 0;
}
