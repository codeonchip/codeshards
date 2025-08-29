#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/* ====== Embedded demo program ====== */
static const char *demo_program =
    "// C-like demo\n"
    "int i = 0;\n"
    "int sum = 0;\n"
    "while (i < 10) {\n"
    "    sum = sum + i;\n"
    "    i = i + 1;\n"
    "}\n"
    "print(sum);\n" // should print 45
    "int a = 3; int b = 7;\n"
    "if (a * 3 == b + a) {\n"
    "    print(111);\n"
    "} else {\n"
    "    print(222);\n"
    "}\n"
    "int x; x = 5; print((x + 2) * (x - 1));\n";

/* ====== Lexing ====== */

typedef enum
{
    T_EOF = 0,
    T_INTLIT,
    T_IDENT,
    T_KW_INT,
    T_KW_PRINT,
    T_KW_IF,
    T_KW_ELSE,
    T_KW_WHILE,
    T_LPAREN,
    T_RPAREN,
    T_LBRACE,
    T_RBRACE,
    T_SEMI,
    T_COMMA,
    T_ASSIGN,
    T_PLUS,
    T_MINUS,
    T_STAR,
    T_SLASH,
    T_MOD,
    T_EQ,
    T_NE,
    T_LT,
    T_LE,
    T_GT,
    T_GE,
    T_AND,
    T_OR,
    T_NOT
} TokenType;

typedef struct
{
    TokenType type;
    int value;      /* for T_INTLIT */
    char ident[64]; /* for T_IDENT  */
    int line, col;  /* for error reporting */
    const char *at; /* pointer into input (debug) */
} Token;

typedef struct
{
    const char *src;
    size_t pos, len;
    int line, col;
    Token cur;
} Lexer;

static void lex_init(Lexer *L, const char *src)
{
    L->src = src;
    L->pos = 0;
    L->len = strlen(src);
    L->line = 1;
    L->col = 1;
}

static char l_peek(Lexer *L)
{
    return (L->pos < L->len) ? L->src[L->pos] : '\0';
}
static char l_peek2(Lexer *L)
{
    return (L->pos + 1 < L->len) ? L->src[L->pos + 1] : '\0';
}
static char l_get(Lexer *L)
{
    char c = l_peek(L);
    if (c)
    {
        L->pos++;
        if (c == '\n')
        {
            L->line++;
            L->col = 1;
        }
        else
            L->col++;
    }
    return c;
}

static void skip_ws_comments(Lexer *L)
{
    for (;;)
    {
        while (isspace((unsigned char)l_peek(L)))
            l_get(L);
        if (l_peek(L) == '/' && l_peek2(L) == '/')
        {
            while (l_peek(L) && l_peek(L) != '\n')
                l_get(L);
            continue;
        }
        if (l_peek(L) == '/' && l_peek2(L) == '*')
        {
            l_get(L);
            l_get(L);
            while (l_peek(L))
            {
                if (l_peek(L) == '*' && l_peek2(L) == '/')
                {
                    l_get(L);
                    l_get(L);
                    break;
                }
                l_get(L);
            }
            continue;
        }
        break;
    }
}

static Token make_tok(TokenType t, Lexer *L, const char *at, int line, int col)
{
    Token tok;
    tok.type = t;
    tok.value = 0;
    tok.ident[0] = '\0';
    tok.line = line;
    tok.col = col;
    tok.at = at;
    return tok;
}

static int is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static void lex_next(Lexer *L)
{
    skip_ws_comments(L);
    int line = L->line, col = L->col;
    const char *at = L->src + L->pos;
    char c = l_peek(L);
    if (!c)
    {
        L->cur = make_tok(T_EOF, L, at, line, col);
        return;
    }

    /* identifiers / keywords */
    if (is_ident_start(c))
    {
        int n = 0;
        while (is_ident_char(l_peek(L)) && n < (int)sizeof(L->cur.ident) - 1)
        {
            L->cur.ident[n++] = l_get(L);
        }
        L->cur.ident[n] = '\0';
        L->cur.line = line;
        L->cur.col = col;
        L->cur.at = at;
        if (strcmp(L->cur.ident, "int") == 0)
            L->cur.type = T_KW_INT;
        else if (strcmp(L->cur.ident, "print") == 0)
            L->cur.type = T_KW_PRINT;
        else if (strcmp(L->cur.ident, "if") == 0)
            L->cur.type = T_KW_IF;
        else if (strcmp(L->cur.ident, "else") == 0)
            L->cur.type = T_KW_ELSE;
        else if (strcmp(L->cur.ident, "while") == 0)
            L->cur.type = T_KW_WHILE;
        else
            L->cur.type = T_IDENT;
        return;
    }

    /* numbers (decimal) */
    if (isdigit((unsigned char)c))
    {
        long v = 0;
        while (isdigit((unsigned char)l_peek(L)))
        {
            v = v * 10 + (l_get(L) - '0');
        }
        L->cur = make_tok(T_INTLIT, L, at, line, col);
        L->cur.value = (int)v;
        return;
    }

    /* multi-char operators */
    if (c == '=' && l_peek2(L) == '=')
    {
        l_get(L);
        l_get(L);
        L->cur = make_tok(T_EQ, L, at, line, col);
        return;
    }
    if (c == '!' && l_peek2(L) == '=')
    {
        l_get(L);
        l_get(L);
        L->cur = make_tok(T_NE, L, at, line, col);
        return;
    }
    if (c == '<' && l_peek2(L) == '=')
    {
        l_get(L);
        l_get(L);
        L->cur = make_tok(T_LE, L, at, line, col);
        return;
    }
    if (c == '>' && l_peek2(L) == '=')
    {
        l_get(L);
        l_get(L);
        L->cur = make_tok(T_GE, L, at, line, col);
        return;
    }
    if (c == '&' && l_peek2(L) == '&')
    {
        l_get(L);
        l_get(L);
        L->cur = make_tok(T_AND, L, at, line, col);
        return;
    }
    if (c == '|' && l_peek2(L) == '|')
    {
        l_get(L);
        l_get(L);
        L->cur = make_tok(T_OR, L, at, line, col);
        return;
    }

    /* single-char tokens */
    l_get(L);
    switch (c)
    {
    case '(':
        L->cur = make_tok(T_LPAREN, L, at, line, col);
        return;
    case ')':
        L->cur = make_tok(T_RPAREN, L, at, line, col);
        return;
    case '{':
        L->cur = make_tok(T_LBRACE, L, at, line, col);
        return;
    case '}':
        L->cur = make_tok(T_RBRACE, L, at, line, col);
        return;
    case ';':
        L->cur = make_tok(T_SEMI, L, at, line, col);
        return;
    case ',':
        L->cur = make_tok(T_COMMA, L, at, line, col);
        return;
    case '+':
        L->cur = make_tok(T_PLUS, L, at, line, col);
        return;
    case '-':
        L->cur = make_tok(T_MINUS, L, at, line, col);
        return;
    case '*':
        L->cur = make_tok(T_STAR, L, at, line, col);
        return;
    case '/':
        L->cur = make_tok(T_SLASH, L, at, line, col);
        return;
    case '%':
        L->cur = make_tok(T_MOD, L, at, line, col);
        return;
    case '=':
        L->cur = make_tok(T_ASSIGN, L, at, line, col);
        return;
    case '<':
        L->cur = make_tok(T_LT, L, at, line, col);
        return;
    case '>':
        L->cur = make_tok(T_GT, L, at, line, col);
        return;
    case '!':
        L->cur = make_tok(T_NOT, L, at, line, col);
        return;
    default:
        break;
    }

    fprintf(stderr, "Lex error at %d:%d: unexpected character '%c'\n", line, col, c);
    L->cur = make_tok(T_EOF, L, at, line, col);
}

/* ====== AST ====== */

typedef enum
{
    EX_INT,
    EX_VAR,
    EX_UNARY,
    EX_BINARY
} ExprKind;

typedef struct Expr
{
    ExprKind kind;
    int op;                 /* for UNARY/BINARY */
    int value;              /* for INT */
    char *var;              /* for VAR */
    struct Expr *lhs, *rhs; /* for BIN; lhs used by UNARY too */
} Expr;

typedef enum
{
    ST_BLOCK,
    ST_VARDECL,
    ST_ASSIGN,
    ST_PRINT,
    ST_IF,
    ST_WHILE,
    ST_EMPTY
} StmtKind;

typedef struct Stmt Stmt;
struct Stmt
{
    StmtKind kind;
    union
    {
        struct
        {
            char *name;
            Expr *init;
        } vardecl;
        struct
        {
            char *name;
            Expr *value;
        } assign;
        struct
        {
            Expr *expr;
        } print;
        struct
        {
            Expr *cond;
            Stmt *then_branch;
            Stmt *else_branch;
        } ifs;
        struct
        {
            Expr *cond;
            Stmt *body;
        } whil;
        struct
        {
            Stmt **items;
            int count, cap;
        } block;
    } u;
};

/* ====== Parser ====== */

typedef struct
{
    Lexer L;
    int had_error;
} Parser;

static void p_err(Parser *P, const char *msg)
{
    fprintf(stderr, "Parse error at %d:%d: %s\n", P->L.cur.line, P->L.cur.col, msg);
    P->had_error = 1;
}

static int accept(Parser *P, TokenType t)
{
    if (P->L.cur.type == t)
    {
        lex_next(&P->L);
        return 1;
    }
    return 0;
}
static void expect(Parser *P, TokenType t, const char *msg)
{
    if (!accept(P, t))
    {
        p_err(P, msg);
    }
}

static char *strdup2(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* Forward decls */
static Stmt *parse_stmt(Parser *P);
static Expr *parse_expr(Parser *P);

static Expr *mk_int(int v)
{
    Expr *e = (Expr *)calloc(1, sizeof(*e));
    e->kind = EX_INT;
    e->value = v;
    return e;
}
static Expr *mk_var(const char *n)
{
    Expr *e = (Expr *)calloc(1, sizeof(*e));
    e->kind = EX_VAR;
    e->var = strdup2(n);
    return e;
}
static Expr *mk_un(int op, Expr *a)
{
    Expr *e = (Expr *)calloc(1, sizeof(*e));
    e->kind = EX_UNARY;
    e->op = op;
    e->lhs = a;
    return e;
}
static Expr *mk_bin(int op, Expr *a, Expr *b)
{
    Expr *e = (Expr *)calloc(1, sizeof(*e));
    e->kind = EX_BINARY;
    e->op = op;
    e->lhs = a;
    e->rhs = b;
    return e;
}

static Stmt *mk_stmt(StmtKind k)
{
    Stmt *s = (Stmt *)calloc(1, sizeof(*s));
    s->kind = k;
    return s;
}

static Stmt *mk_block(void)
{
    Stmt *s = mk_stmt(ST_BLOCK);
    s->u.block.cap = 8;
    s->u.block.items = (Stmt **)calloc((size_t)s->u.block.cap, sizeof(Stmt *));
    return s;
}
static void block_add(Stmt *blk, Stmt *s)
{
    if (blk->u.block.count >= blk->u.block.cap)
    {
        blk->u.block.cap *= 2;
        blk->u.block.items = (Stmt **)realloc(blk->u.block.items, (size_t)blk->u.block.cap * sizeof(Stmt *));
    }
    blk->u.block.items[blk->u.block.count++] = s;
}

/* expression precedence:
   1. unary: + - !
   2. * / %
   3. + -
   4. < <= > >=
   5. == !=
   6. &&
   7. ||
*/

static Expr *parse_primary(Parser *P)
{
    Token t = P->L.cur;
    if (accept(P, T_INTLIT))
        return mk_int(t.value);
    if (accept(P, T_IDENT))
        return mk_var(t.ident);
    if (accept(P, T_LPAREN))
    {
        Expr *e = parse_expr(P);
        expect(P, T_RPAREN, "expected ')'");
        return e;
    }
    p_err(P, "expected primary expression");
    return mk_int(0);
}

static Expr *parse_unary(Parser *P)
{
    if (accept(P, T_PLUS))
        return mk_un('+', parse_unary(P));
    if (accept(P, T_MINUS))
        return mk_un('-', parse_unary(P));
    if (accept(P, T_NOT))
        return mk_un('!', parse_unary(P));
    return parse_primary(P);
}

static Expr *parse_bin_rhs(Parser *P, Expr *lhs, int min_prec);

static int tok_prec(TokenType t)
{
    switch (t)
    {
    case T_STAR:
    case T_SLASH:
    case T_MOD:
        return 5;
    case T_PLUS:
    case T_MINUS:
        return 4;
    case T_LT:
    case T_LE:
    case T_GT:
    case T_GE:
        return 3;
    case T_EQ:
    case T_NE:
        return 2;
    case T_AND:
        return 1;
    case T_OR:
        return 0;
    default:
        return -1;
    }
}
static int tok_to_op(TokenType t)
{
    switch (t)
    {
    case T_STAR:
        return '*';
    case T_SLASH:
        return '/';
    case T_MOD:
        return '%';
    case T_PLUS:
        return '+';
    case T_MINUS:
        return '-';
    case T_LT:
        return '<';
    case T_LE:
        return 256 + 'l'; /* custom codes beyond ASCII */
    case T_GT:
        return '>';
    case T_GE:
        return 256 + 'g';
    case T_EQ:
        return 256 + 'e';
    case T_NE:
        return 256 + 'n';
    case T_AND:
        return 256 + '&';
    case T_OR:
        return 256 + '|';
    default:
        return 0;
    }
}

static Expr *parse_mul(Parser *P) { return parse_unary(P); }

static Expr *parse_bin(Parser *P, int min_prec)
{
    Expr *lhs = parse_mul(P);
    return parse_bin_rhs(P, lhs, min_prec);
}

static Expr *parse_bin_rhs(Parser *P, Expr *lhs, int min_prec)
{
    for (;;)
    {
        int prec = tok_prec(P->L.cur.type);
        if (prec < min_prec)
            return lhs;
        TokenType op_t = P->L.cur.type;
        lex_next(&P->L);
        Expr *rhs = parse_unary(P);
        int next_prec = tok_prec(P->L.cur.type);
        if (next_prec > prec)
            rhs = parse_bin_rhs(P, rhs, prec + 1);
        lhs = mk_bin(tok_to_op(op_t), lhs, rhs);
    }
}

static Expr *parse_expr(Parser *P) { return parse_bin(P, 0); }

static Stmt *parse_block(Parser *P)
{
    expect(P, T_LBRACE, "expected '{'");
    Stmt *blk = mk_block();
    while (P->L.cur.type != T_RBRACE && P->L.cur.type != T_EOF)
    {
        block_add(blk, parse_stmt(P));
    }
    expect(P, T_RBRACE, "expected '}'");
    return blk;
}

static Stmt *parse_stmt(Parser *P)
{
    if (accept(P, T_SEMI))
    {
        return mk_stmt(ST_EMPTY);
    }

    if (accept(P, T_KW_INT))
    {
        Token id = P->L.cur;
        expect(P, T_IDENT, "expected identifier after 'int'");
        Stmt *s = mk_stmt(ST_VARDECL);
        s->u.vardecl.name = strdup2(id.ident);
        if (accept(P, T_ASSIGN))
        {
            s->u.vardecl.init = parse_expr(P);
        }
        else
        {
            s->u.vardecl.init = NULL; /* defaults to 0, uninitialized flag */
        }
        expect(P, T_SEMI, "expected ';' after declaration");
        return s;
    }

    if (accept(P, T_KW_PRINT))
    {
        Stmt *s = mk_stmt(ST_PRINT);
        expect(P, T_LPAREN, "expected '(' after print");
        s->u.print.expr = parse_expr(P);
        expect(P, T_RPAREN, "expected ')'");
        expect(P, T_SEMI, "expected ';'");
        return s;
    }

    if (accept(P, T_KW_IF))
    {
        Stmt *s = mk_stmt(ST_IF);
        expect(P, T_LPAREN, "expected '(' after if");
        s->u.ifs.cond = parse_expr(P);
        expect(P, T_RPAREN, "expected ')'");
        s->u.ifs.then_branch = (P->L.cur.type == T_LBRACE) ? parse_block(P) : parse_stmt(P);
        if (accept(P, T_KW_ELSE))
        {
            s->u.ifs.else_branch = (P->L.cur.type == T_LBRACE) ? parse_block(P) : parse_stmt(P);
        }
        else
        {
            s->u.ifs.else_branch = mk_stmt(ST_EMPTY);
        }
        return s;
    }

    if (accept(P, T_KW_WHILE))
    {
        Stmt *s = mk_stmt(ST_WHILE);
        expect(P, T_LPAREN, "expected '(' after while");
        s->u.whil.cond = parse_expr(P);
        expect(P, T_RPAREN, "expected ')'");
        s->u.whil.body = (P->L.cur.type == T_LBRACE) ? parse_block(P) : parse_stmt(P);
        return s;
    }

    if (accept(P, T_LBRACE))
    {
        /* put back (we already consumed), but we can't unget; handle as block */
        P->L.pos--;
        P->L.col--;      /* crude but safe since last char was '{' */
        lex_next(&P->L); /* re-read '{' */
        return parse_block(P);
    }

    /* assignment: IDENT '=' expr ';' */
    if (P->L.cur.type == T_IDENT)
    {
        Token id = P->L.cur;
        lex_next(&P->L);
        expect(P, T_ASSIGN, "expected '=' in assignment");
        Stmt *s = mk_stmt(ST_ASSIGN);
        s->u.assign.name = strdup2(id.ident);
        s->u.assign.value = parse_expr(P);
        expect(P, T_SEMI, "expected ';'");
        return s;
    }

    p_err(P, "unknown statement");
    /* try to recover */
    while (P->L.cur.type != T_SEMI && P->L.cur.type != T_RBRACE && P->L.cur.type != T_EOF)
        lex_next(&P->L);
    accept(P, T_SEMI);
    return mk_stmt(ST_EMPTY);
}

/* ====== Runtime (symbol table + evaluator) ====== */

typedef struct
{
    char *name;
    int value;
    int initialized;
} Var;

typedef struct
{
    Var *vars;
    int count, cap;
} Env;

static void env_init(Env *E)
{
    E->cap = 16;
    E->count = 0;
    E->vars = (Var *)calloc((size_t)E->cap, sizeof(Var));
}
static void env_free(Env *E)
{
    for (int i = 0; i < E->count; i++)
        free(E->vars[i].name);
    free(E->vars);
}
static int env_find(Env *E, const char *name)
{
    for (int i = 0; i < E->count; i++)
        if (strcmp(E->vars[i].name, name) == 0)
            return i;
    return -1;
}
static int env_add(Env *E, const char *name)
{
    if (E->count >= E->cap)
    {
        E->cap *= 2;
        E->vars = (Var *)realloc(E->vars, (size_t)E->cap * sizeof(Var));
    }
    int i = E->count++;
    E->vars[i].name = strdup2(name);
    E->vars[i].value = 0;
    E->vars[i].initialized = 0;
    return i;
}

static int eval_expr(Env *E, Expr *e, int *ok);

static int eval_binop(int op, int a, int b, int *ok)
{
    switch (op)
    {
    case '+':
        return a + b;
    case '-':
        return a - b;
    case '*':
        return a * b;
    case '/':
        if (b == 0)
        {
            fprintf(stderr, "Runtime error: division by zero\n");
            *ok = 0;
            return 0;
        }
        return a / b;
    case '%':
        if (b == 0)
        {
            fprintf(stderr, "Runtime error: modulo by zero\n");
            *ok = 0;
            return 0;
        }
        return a % b;
    case '<':
        return a < b;
    case '>':
        return a > b;
    case 256 + 'l':
        return a <= b;
    case 256 + 'g':
        return a >= b;
    case 256 + 'e':
        return a == b;
    case 256 + 'n':
        return a != b;
    case 256 + '&':
        return (a != 0 && b != 0);
    case 256 + '|':
        return (a != 0 || b != 0);
    default:
        fprintf(stderr, "Runtime error: unknown binop %d\n", op);
        *ok = 0;
        return 0;
    }
}

static int eval_expr(Env *E, Expr *e, int *ok)
{
    if (!*ok)
        return 0;
    switch (e->kind)
    {
    case EX_INT:
        return e->value;
    case EX_VAR:
    {
        int idx = env_find(E, e->var);
        if (idx < 0)
        {
            fprintf(stderr, "Runtime error: undefined variable '%s'\n", e->var);
            *ok = 0;
            return 0;
        }
        if (!E->vars[idx].initialized)
        {
            fprintf(stderr, "Runtime error: uninitialized variable '%s'\n", e->var);
            *ok = 0;
            return 0;
        }
        return E->vars[idx].value;
    }
    case EX_UNARY:
    {
        int v = eval_expr(E, e->lhs, ok);
        if (!*ok)
            return 0;
        if (e->op == '+')
            return v;
        if (e->op == '-')
            return -v;
        if (e->op == '!')
            return (v == 0);
        fprintf(stderr, "Runtime error: unknown unary %d\n", e->op);
        *ok = 0;
        return 0;
    }
    case EX_BINARY:
    {
        int a = eval_expr(E, e->lhs, ok);
        if (!*ok)
            return 0;
        /* short-circuit AND/OR */
        if (e->op == 256 + '&')
        {
            if (a == 0)
                return 0;
            int b = eval_expr(E, e->rhs, ok);
            return (a != 0 && b != 0);
        }
        if (e->op == 256 + '|')
        {
            if (a != 0)
                return 1;
            int b = eval_expr(E, e->rhs, ok);
            return (a != 0 || b != 0);
        }
        int b = eval_expr(E, e->rhs, ok);
        if (!*ok)
            return 0;
        return eval_binop(e->op, a, b, ok);
    }
    default:
        *ok = 0;
        return 0;
    }
}

static void exec_stmt(Env *E, Stmt *s, int *ok);

static void exec_block(Env *E, Stmt *blk, int *ok)
{
    for (int i = 0; i < blk->u.block.count && *ok; i++)
        exec_stmt(E, blk->u.block.items[i], ok);
}

static void exec_stmt(Env *E, Stmt *s, int *ok)
{
    if (!*ok)
        return;
    switch (s->kind)
    {
    case ST_EMPTY:
        return;
    case ST_BLOCK:
        exec_block(E, s, ok);
        return;
    case ST_VARDECL:
    {
        int idx = env_find(E, s->u.vardecl.name);
        if (idx >= 0)
        {
            fprintf(stderr, "Runtime error: redeclaration of '%s'\n", s->u.vardecl.name);
            *ok = 0;
            return;
        }
        idx = env_add(E, s->u.vardecl.name);
        if (s->u.vardecl.init)
        {
            int v = eval_expr(E, s->u.vardecl.init, ok);
            if (!*ok)
                return;
            E->vars[idx].value = v;
            E->vars[idx].initialized = 1;
        }
        else
        {
            E->vars[idx].value = 0;
            E->vars[idx].initialized = 0;
        }
        return;
    }
    case ST_ASSIGN:
    {
        int idx = env_find(E, s->u.assign.name);
        if (idx < 0)
        {
            fprintf(stderr, "Runtime error: assignment to undeclared '%s'\n", s->u.assign.name);
            *ok = 0;
            return;
        }
        int v = eval_expr(E, s->u.assign.value, ok);
        if (!*ok)
            return;
        E->vars[idx].value = v;
        E->vars[idx].initialized = 1;
        return;
    }
    case ST_PRINT:
    {
        int v = eval_expr(E, s->u.print.expr, ok);
        if (!*ok)
            return;
        printf("%d\n", v);
        return;
    }
    case ST_IF:
    {
        int c = eval_expr(E, s->u.ifs.cond, ok);
        if (!*ok)
            return;
        if (c)
            exec_stmt(E, s->u.ifs.then_branch, ok);
        else
            exec_stmt(E, s->u.ifs.else_branch, ok);
        return;
    }
    case ST_WHILE:
    {
        for (;;)
        {
            int c = eval_expr(E, s->u.whil.cond, ok);
            if (!*ok)
                return;
            if (!c)
                break;
            exec_stmt(E, s->u.whil.body, ok);
            if (!*ok)
                return;
        }
        return;
    }
    default:
        *ok = 0;
        fprintf(stderr, "Runtime error: unknown stmt kind\n");
        return;
    }
}

/* ====== Build AST from source ====== */

static Stmt *parse_program(Parser *P)
{
    Stmt *top = mk_block();
    while (P->L.cur.type != T_EOF)
    {
        block_add(top, parse_stmt(P));
    }
    return top;
}

/* ====== Cleanup (optional: free AST) ======
   For brevity we skip deep-freeing in this tiny demo.
*/

/* ====== Driver ====== */

static char *load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    const char *source = demo_program;
    char *heap_source = NULL;
    if (argc > 1)
    {
        heap_source = load_file(argv[1]);
        if (!heap_source)
        {
            fprintf(stderr, "Could not read '%s'\n", argv[1]);
            return 1;
        }
        source = heap_source;
    }

    Parser P;
    lex_init(&P.L, source);
    lex_next(&P.L);
    P.had_error = 0;
    Stmt *program = parse_program(&P);
    if (P.had_error)
    {
        fprintf(stderr, "Aborting due to parse errors.\n");
        free(heap_source);
        return 2;
    }

    Env env;
    env_init(&env);
    int ok = 1;
    exec_stmt(&env, program, &ok);
    env_free(&env);

    free(heap_source);
    return ok ? 0 : 3;
}
