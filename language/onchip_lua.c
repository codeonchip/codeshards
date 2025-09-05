// luette.c - tiny Lua-like interpreter in one C file, no external libraries.
// Subset: numbers, strings, booleans, nil, variables, assignment,
// if/then/else/end, while/do/end, function/end (no closures), return,
// calls, and builtin print(...). Comments: -- to end-of-line.
// This is a teaching toy, not production; no GC; very basic error checks.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>

/*======================== Utilities ========================*/
#define DIE(...)                                \
    do                                          \
    {                                           \
        fprintf(stderr, "error: " __VA_ARGS__); \
        fputc('\n', stderr);                    \
        exit(1);                                \
    } while (0)
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static char *sdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (!p)
        DIE("oom");
    memcpy(p, s, n);
    return p;
}
static char *sdupn(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p)
        DIE("oom");
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/*======================== Lexer ========================*/
typedef enum
{
    T_EOF = 0,
    T_ID,
    T_NUM,
    T_STR,
    T_KIF,
    T_KTHEN,
    T_KELSE,
    T_KEND,
    T_KWHILE,
    T_KDO,
    T_KFUNCTION,
    T_KRETURN,
    T_KTRUE,
    T_KFALSE,
    T_KNIL,
    T_KAND,
    T_KOR,
    T_KNOT,
    T_LP = '(',
    T_RP = ')',
    T_COMMA = ',',
    T_SEMI = ';',
    T_ASSIGN = '=',
    T_PLUS = '+',
    T_MINUS = '-',
    T_STAR = '*',
    T_SLASH = '/',
    T_PCT = '%',
    T_CARET = '^',
    T_EQ,
    T_NE,
    T_LT = '<',
    T_LE,
    T_GT = '>',
    T_GE
} Tok;

typedef struct
{
    const char *src;
    size_t pos, len;
    int line;
    Tok tok;
    char *lex;
    double num;
} Lex;

static int isid0(int c) { return isalpha(c) || c == '_'; }
static int isid(int c) { return isalnum(c) || c == '_'; }

static void lex_next(Lex *L);

static Lex lex_init(const char *s)
{
    Lex L = {s, 0, strlen(s), 1, T_EOF, NULL, 0.0};
    lex_next(&L);
    return L;
}

static void skip_ws(Lex *L)
{
    while (L->pos < L->len)
    {
        char c = L->src[L->pos];
        if (c == '\n')
        {
            L->line++;
            L->pos++;
        }
        else if (isspace((unsigned char)c))
            L->pos++;
        else if (c == '-' && L->pos + 1 < L->len && L->src[L->pos + 1] == '-')
        { // comment
            L->pos += 2;
            while (L->pos < L->len && L->src[L->pos] != '\n')
                L->pos++;
        }
        else
            break;
    }
}

static int match_str(Lex *L, const char *kw)
{
    size_t k = strlen(kw);
    if (L->pos + k <= L->len && strncmp(L->src + L->pos, kw, k) == 0)
    {
        if (L->pos + k == L->len || !isid(L->src[L->pos + k]))
        {
            L->pos += k;
            return 1;
        }
    }
    return 0;
}

static Tok kw_tok(const char *id)
{
    struct
    {
        const char *s;
        Tok t;
    } kws[] = {
        {"if", T_KIF},
        {"then", T_KTHEN},
        {"else", T_KELSE},
        {"end", T_KEND},
        {"while", T_KWHILE},
        {"do", T_KDO},
        {"function", T_KFUNCTION},
        {"return", T_KRETURN},
        {"true", T_KTRUE},
        {"false", T_KFALSE},
        {"nil", T_KNIL},
        {"and", T_KAND},
        {"or", T_KOR},
        {"not", T_KNOT},
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); ++i)
        if (strcmp(id, kws[i].s) == 0)
            return kws[i].t;
    return T_ID;
}

static void lex_next(Lex *L)
{
    if (L->lex)
    {
        free(L->lex);
        L->lex = NULL;
    }
    skip_ws(L);
    if (L->pos >= L->len)
    {
        L->tok = T_EOF;
        return;
    }
    char c = L->src[L->pos++];
    switch (c)
    {
    case '(':
    {
        L->tok = T_LP;
        return;
    }
    case ')':
    {
        L->tok = T_RP;
        return;
    }
    case ',':
    {
        L->tok = T_COMMA;
        return;
    }
    case ';':
    {
        L->tok = T_SEMI;
        return;
    }
    case '+':
    {
        L->tok = T_PLUS;
        return;
    }
    case '-':
    {
        L->tok = T_MINUS;
        return;
    }
    case '*':
    {
        L->tok = T_STAR;
        return;
    }
    case '/':
    {
        L->tok = T_SLASH;
        return;
    }
    case '%':
    {
        L->tok = T_PCT;
        return;
    }
    case '^':
    {
        L->tok = T_CARET;
        return;
    }
    case '=':
    {
        if (L->pos < L->len && L->src[L->pos] == '=')
        {
            L->pos++;
            L->tok = T_EQ;
            return;
        }
        L->tok = T_ASSIGN;
        return;
    }
    case '<':
    {
        if (L->pos < L->len && L->src[L->pos] == '=')
        {
            L->pos++;
            L->tok = T_LE;
            return;
        }
        L->tok = T_LT;
        return;
    }
    case '>':
    {
        if (L->pos < L->len && L->src[L->pos] == '=')
        {
            L->pos++;
            L->tok = T_GE;
            return;
        }
        L->tok = T_GT;
        return;
    }
    case '~':
    {
        if (L->pos < L->len && L->src[L->pos] == '=')
        {
            L->pos++;
            L->tok = T_NE;
            return;
        }
        DIE("line %d: unexpected '~'", L->line);
    }
    case '"':
    {
        size_t start = L->pos, i = L->pos;
        char *buf = (char *)malloc(1);
        size_t cap = 1, n = 0;
        if (!buf)
            DIE("oom");
        while (i < L->len)
        {
            char d = L->src[i++];
            if (d == '"')
                break;
            if (d == '\\' && i < L->len)
            {
                char e = L->src[i++];
                if (e == 'n')
                    d = '\n';
                else if (e == 't')
                    d = '\t';
                else
                    d = e;
            }
            if (n + 1 >= cap)
            {
                cap *= 2;
                buf = realloc(buf, cap);
                if (!buf)
                    DIE("oom");
            }
            buf[n++] = d;
        }
        if (i > L->len || L->src[i - 1] != '"')
            DIE("line %d: unterminated string", L->line);
        buf[n] = 0;
        L->lex = buf;
        L->tok = T_STR;
        L->pos = i;
        return;
    }
    default:
    {
        if (isdigit((unsigned char)c) || (c == '.' && L->pos < L->len && isdigit((unsigned char)L->src[L->pos])))
        {
            size_t s = L->pos - 1;
            while (L->pos < L->len && (isdigit((unsigned char)L->src[L->pos]) || L->src[L->pos] == '.' || L->src[L->pos] == 'e' || L->src[L->pos] == 'E' || L->src[L->pos] == '+' || L->src[L->pos] == '-'))
            {
                char k = L->src[L->pos];
                if (!(isdigit((unsigned char)k) || k == '.' || k == 'e' || k == 'E' || k == '+' || k == '-'))
                    break;
                L->pos++;
            }
            L->lex = sdupn(L->src + s, L->pos - s);
            L->num = strtod(L->lex, NULL);
            L->tok = T_NUM;
            return;
        }
        else if (isid0((unsigned char)c))
        {
            size_t s = L->pos - 1;
            while (L->pos < L->len && isid((unsigned char)L->src[L->pos]))
                L->pos++;
            L->lex = sdupn(L->src + s, L->pos - s);
            L->tok = kw_tok(L->lex);
            return;
        }
        DIE("line %d: bad char '%c'", L->line, c);
    }
    }
}

/*======================== Values ========================*/
typedef enum
{
    V_NIL,
    V_NUM,
    V_BOOL,
    V_STR,
    V_FUNC
} VTag;

struct AST; // forward

typedef struct Value
{
    VTag t;
    union
    {
        double num;
        int boolean;
        char *str;
        struct AST *func; // function node pointer
    } u;
} Value;

static Value V_nil(void)
{
    Value v;
    v.t = V_NIL;
    return v;
}
static Value V_num(double x)
{
    Value v;
    v.t = V_NUM;
    v.u.num = x;
    return v;
}
static Value V_bool(int b)
{
    Value v;
    v.t = V_BOOL;
    v.u.boolean = b ? 1 : 0;
    return v;
}
static Value V_str(char *s)
{
    Value v;
    v.t = V_STR;
    v.u.str = s;
    return v;
}
static Value V_func(struct AST *f)
{
    Value v;
    v.t = V_FUNC;
    v.u.func = f;
    return v;
}

static const char *vtag(VTag t)
{
    switch (t)
    {
    case V_NIL:
        return "nil";
    case V_NUM:
        return "number";
    case V_BOOL:
        return "boolean";
    case V_STR:
        return "string";
    case V_FUNC:
        return "function";
    }
    return "?";
}

/*======================== Environment ========================*/
typedef struct Binding
{
    char *name;
    Value v;
    struct Binding *next;
} Binding;

typedef struct Env
{
    struct Env *parent;
    Binding *head;
} Env;

static Env *env_new(Env *parent)
{
    Env *e = (Env *)calloc(1, sizeof(Env));
    if (!e)
        DIE("oom");
    e->parent = parent;
    return e;
}
static void env_set(Env *e, const char *name, Value v)
{
    for (Binding *b = e->head; b; b = b->next)
        if (strcmp(b->name, name) == 0)
        {
            b->v = v;
            return;
        }
    Binding *b = (Binding *)malloc(sizeof(Binding));
    if (!b)
        DIE("oom");
    b->name = sdup(name);
    b->v = v;
    b->next = e->head;
    e->head = b;
}
static int env_get(Env *e, const char *name, Value *out)
{
    for (Env *p = e; p; p = p->parent)
        for (Binding *b = p->head; b; b = b->next)
            if (strcmp(b->name, name) == 0)
            {
                *out = b->v;
                return 1;
            }
    return 0;
}

/*======================== AST ========================*/
typedef enum
{
    N_BLOCK,
    N_NUM,
    N_STR,
    N_BOOL,
    N_NIL,
    N_VAR,
    N_ASSIGN,
    N_BIN,
    N_UN,
    N_CALL,
    N_IF,
    N_WHILE,
    N_FUNDEF,
    N_RETURN
} NTag;

typedef struct Vec
{
    void **d;
    int n, cap;
} Vec;

static void vec_push(Vec *v, void *p)
{
    if (v->n + 1 > v->cap)
    {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->d = realloc(v->d, v->cap * sizeof(void *));
        if (!v->d)
            DIE("oom");
    }
    v->d[v->n++] = p;
}

typedef struct AST
{
    NTag t;
    int line;
    union
    {
        struct
        {
            Vec stmts;
        } block;
        struct
        {
            double num;
        } num;
        struct
        {
            char *s;
        } str;
        struct
        {
            int b;
        } boolean;
        struct
        {
            char *name;
        } var;
        struct
        {
            char *name;
            struct AST *expr;
        } assign;
        struct
        {
            int op;
            struct AST *a, *b;
        } bin;
        struct
        {
            int op;
            struct AST *a;
        } un;
        struct
        {
            char *name;
            Vec args;
        } call;
        struct
        {
            struct AST *cond;
            struct AST *thn;
            struct AST *els;
        } ifs;
        struct
        {
            struct AST *cond;
            struct AST *body;
        } whil;
        struct
        {
            char *name;
            Vec params;
            struct AST *body;
        } fundef;
        struct
        {
            Vec exprs;
        } ret;
    } u;
} AST;

static AST *node_new(NTag t, int line)
{
    AST *n = (AST *)calloc(1, sizeof(AST));
    if (!n)
        DIE("oom");
    n->t = t;
    n->line = line;
    return n;
}

/*======================== Parser ========================*/
typedef struct
{
    Lex L;
} Parser;

static void expect(Parser *P, Tok t)
{
    if (P->L.tok != t)
        DIE("line %d: expected token %d", P->L.line, t);
    lex_next(&P->L);
}

static int accept(Parser *P, Tok t)
{
    if (P->L.tok == t)
    {
        lex_next(&P->L);
        return 1;
    }
    return 0;
}

static AST *parse_block(Parser *P); // fwd
static AST *parse_exp(Parser *P);

static AST *parse_primary(Parser *P)
{
    Lex *L = &P->L;
    if (L->tok == T_NUM)
    {
        AST *n = node_new(N_NUM, L->line);
        n->u.num.num = L->num;
        lex_next(L);
        return n;
    }
    if (L->tok == T_STR)
    {
        AST *n = node_new(N_STR, L->line);
        n->u.str.s = sdup(L->lex);
        lex_next(L);
        return n;
    }
    if (L->tok == T_KTRUE)
    {
        AST *n = node_new(N_BOOL, L->line);
        n->u.boolean.b = 1;
        lex_next(L);
        return n;
    }
    if (L->tok == T_KFALSE)
    {
        AST *n = node_new(N_BOOL, L->line);
        n->u.boolean.b = 0;
        lex_next(L);
        return n;
    }
    if (L->tok == T_KNIL)
    {
        AST *n = node_new(N_NIL, L->line);
        lex_next(L);
        return n;
    }
    if (L->tok == T_ID)
    {
        char *id = sdup(L->lex);
        lex_next(L);
        if (accept(P, T_LP))
        { // call
            AST *n = node_new(N_CALL, L->line);
            n->u.call.name = id;
            Vec args = {0};
            if (P->L.tok != T_RP)
            {
                for (;;)
                {
                    AST *e = parse_exp(P);
                    vec_push(&args, (void *)e);
                    if (!accept(P, T_COMMA))
                        break;
                }
            }
            expect(P, T_RP);
            n->u.call.args = args;
            return n;
        }
        else
        {
            AST *n = node_new(N_VAR, L->line);
            n->u.var.name = id;
            return n;
        }
    }
    if (accept(P, T_LP))
    {
        AST *e = parse_exp(P);
        expect(P, T_RP);
        return e;
    }
    DIE("line %d: bad expression", L->line);
    return NULL;
}

static AST *parse_unary(Parser *P)
{
    if (accept(P, T_KNOT) || accept(P, T_MINUS))
    {
        int op = (P->L.tok == T_MINUS) ? 0 : 0; // dummy: we consumed already using accept
    }
    // re-implement properly:
    if (P->L.tok == T_KNOT)
    {
        int line = P->L.line;
        lex_next(&P->L);
        AST *a = parse_unary(P);
        AST *n = node_new(N_UN, line);
        n->u.un.op = T_KNOT;
        n->u.un.a = a;
        return n;
    }
    if (P->L.tok == T_MINUS)
    {
        int line = P->L.line;
        lex_next(&P->L);
        AST *a = parse_unary(P);
        AST *n = node_new(N_UN, line);
        n->u.un.op = T_MINUS;
        n->u.un.a = a;
        return n;
    }
    return parse_primary(P);
}

static int bin_prec(Tok t)
{
    switch (t)
    {
    case T_CARET:
        return 8; // right-assoc
    case T_STAR:
    case T_SLASH:
    case T_PCT:
        return 7;
    case T_PLUS:
    case T_MINUS:
        return 6;
    case T_LT:
    case T_LE:
    case T_GT:
    case T_GE:
        return 5;
    case T_EQ:
    case T_NE:
        return 4;
    case T_KAND:
        return 3;
    case T_KOR:
        return 2;
    default:
        return -1;
    }
}
static int is_right_assoc(Tok t) { return t == T_CARET; }

static AST *parse_bin_rhs(Parser *P, int minp, AST *lhs)
{
    for (;;)
    {
        Tok t = P->L.tok;
        int p = bin_prec(t);
        if (p < minp)
            return lhs;
        int line = P->L.line;
        lex_next(&P->L);
        AST *rhs = parse_unary(P);
        Tok t2 = P->L.tok;
        int p2 = bin_prec(t2);
        if (p2 > p || (p2 == p && is_right_assoc(t2)))
        {
            rhs = parse_bin_rhs(P, p + 1, rhs);
        }
        AST *n = node_new(N_BIN, line);
        n->u.bin.op = t;
        n->u.bin.a = lhs;
        n->u.bin.b = rhs;
        lhs = n;
    }
}

static AST *parse_exp(Parser *P)
{
    // assignment or expression
    if (P->L.tok == T_ID)
    {
        // lookahead for '='
        size_t save_pos = P->L.pos;
        int save_line = P->L.line;
        char *id = sdup(P->L.lex);
        lex_next(&P->L);
        if (P->L.tok == T_ASSIGN)
        {
            int line = save_line;
            lex_next(&P->L);
            AST *e = parse_exp(P);
            AST *n = node_new(N_ASSIGN, line);
            n->u.assign.name = id;
            n->u.assign.expr = e;
            return n;
        }
        // rollback
        P->L.pos = save_pos;
        P->L.line = save_line;
        if (P->L.lex)
        {
            free(P->L.lex);
        }
        P->L.lex = sdup(id);
        P->L.tok = T_ID;
        free(id);
    }
    AST *u = parse_unary(P);
    return parse_bin_rhs(P, 1, u);
}

static AST *parse_stmt(Parser *P)
{
    Lex *L = &P->L;
    if (accept(P, T_SEMI))
    {
        AST *n = node_new(N_BLOCK, L->line);
        return n;
    }
    if (L->tok == T_KIF)
    {
        int line = L->line;
        lex_next(L);
        AST *cond = parse_exp(P);
        expect(P, T_KTHEN);
        AST *thn = parse_block(P);
        AST *els = NULL;
        if (accept(P, T_KELSE))
        {
            els = parse_block(P);
        }
        expect(P, T_KEND);
        AST *n = node_new(N_IF, line);
        n->u.ifs.cond = cond;
        n->u.ifs.thn = thn;
        n->u.ifs.els = els;
        return n;
    }
    if (L->tok == T_KWHILE)
    {
        int line = L->line;
        lex_next(L);
        AST *cond = parse_exp(P);
        expect(P, T_KDO);
        AST *body = parse_block(P);
        expect(P, T_KEND);
        AST *n = node_new(N_WHILE, line);
        n->u.whil.cond = cond;
        n->u.whil.body = body;
        return n;
    }
    if (L->tok == T_KFUNCTION)
    {
        int line = L->line;
        lex_next(L);
        if (L->tok != T_ID)
            DIE("line %d: function name expected", L->line);
        char *name = sdup(L->lex);
        lex_next(L);
        expect(P, T_LP);
        Vec ps = {0};
        if (L->tok != T_RP)
        {
            for (;;)
            {
                if (L->tok != T_ID)
                    DIE("line %d: param name expected", L->line);
                vec_push(&ps, sdup(L->lex));
                lex_next(L);
                if (!accept(P, T_COMMA))
                    break;
            }
        }
        expect(P, T_RP);
        AST *body = parse_block(P);
        expect(P, T_KEND);
        AST *n = node_new(N_FUNDEF, line);
        n->u.fundef.name = name;
        n->u.fundef.params = ps;
        n->u.fundef.body = body;
        return n;
    }
    if (L->tok == T_KRETURN)
    {
        int line = L->line;
        lex_next(L);
        Vec es = {0};
        if (L->tok != T_SEMI && L->tok != T_KEND && L->tok != T_EOF)
        {
            // parse one expression list (we'll return only first value in this tiny impl)
            vec_push(&es, parse_exp(P));
            while (accept(P, T_COMMA))
                vec_push(&es, parse_exp(P));
        }
        AST *n = node_new(N_RETURN, line);
        n->u.ret.exprs = es;
        return n;
    }
    // expression (incl assignment/call)
    AST *e = parse_exp(P);
    // optional semicolon
    accept(P, T_SEMI);
    return e;
}

static AST *parse_block(Parser *P)
{
    AST *n = node_new(N_BLOCK, P->L.line);
    Vec ss = {0};
    while (P->L.tok != T_EOF && P->L.tok != T_KEND && P->L.tok != T_KELSE && P->L.tok != T_KTHEN && P->L.tok != T_KDO && P->L.tok != T_RP)
    {
        AST *s = parse_stmt(P);
        vec_push(&ss, s);
    }
    n->u.block.stmts = ss;
    return n;
}

static AST *parse_chunk(const char *code)
{
    Parser P = {lex_init(code)};
    AST *b = parse_block(&P);
    if (P.L.tok != T_EOF)
        DIE("line %d: unexpected tokens remain", P.L.line);
    return b;
}

/*======================== VM / Evaluator ========================*/
typedef struct
{
    jmp_buf jb;
    int active; // inside function
    Value retv;
} RetJump;

typedef struct
{
    Env *G; // global
    RetJump rj;
} VM;

static int is_truthy(Value v)
{
    if (v.t == V_NIL)
        return 0;
    if (v.t == V_BOOL)
        return v.u.boolean != 0;
    return 1;
}

static Value eval(VM *vm, Env *env, AST *n);

static double as_num(AST *n, Value v)
{
    if (v.t != V_NUM)
        DIE("line %d: expected number, got %s", n->line, vtag(v.t));
    return v.u.num;
}

static int as_bool(AST *n, Value v)
{
    if (v.t == V_BOOL)
        return v.u.boolean;
    if (v.t == V_NIL)
        return 0;
    if (v.t == V_NUM)
        return v.u.num != 0.0;
    return 1;
}

static Value call_function(VM *vm, Env *env, AST *fndef, int argc, Value *argv)
{
    // create local env with params
    Env *E = env_new(env);
    int np = fndef->u.fundef.params.n;
    for (int i = 0; i < np; i++)
    {
        char *pname = (char *)fndef->u.fundef.params.d[i];
        Value v = (i < argc) ? argv[i] : V_nil();
        env_set(E, pname, v);
    }
    // enable return jump
    vm->rj.active = 1;
    if (!setjmp(vm->rj.jb))
    {
        (void)eval(vm, E, fndef->u.fundef.body);
        vm->rj.active = 0;
        return V_nil();
    }
    else
    {
        vm->rj.active = 0;
        return vm->rj.retv;
    }
}

static Value builtin_print(int argc, Value *argv)
{
    for (int i = 0; i < argc; i++)
    {
        Value v = argv[i];
        if (i)
            printf("\t");
        switch (v.t)
        {
        case V_NIL:
            printf("nil");
            break;
        case V_BOOL:
            printf(v.u.boolean ? "true" : "false");
            break;
        case V_NUM:
            printf("%.17g", v.u.num);
            break;
        case V_STR:
            printf("%s", v.u.str);
            break;
        case V_FUNC:
            printf("function:%p", (void *)v.u.func);
            break;
        }
    }
    printf("\n");
    return V_nil();
}

static Value eval(VM *vm, Env *env, AST *n)
{
    switch (n->t)
    {
    case N_BLOCK:
    {
        for (int i = 0; i < n->u.block.stmts.n; i++)
        {
            eval(vm, env, (AST *)n->u.block.stmts.d[i]);
        }
        return V_nil();
    }
    case N_NUM:
        return V_num(n->u.num.num);
    case N_STR:
        return V_str(n->u.str.s); // leaks on long runs; fine for demo
    case N_BOOL:
        return V_bool(n->u.boolean.b);
    case N_NIL:
        return V_nil();
    case N_VAR:
    {
        Value v;
        if (!env_get(env, n->u.var.name, &v))
        {
            // builtin 'print'
            if (strcmp(n->u.var.name, "print") == 0)
                return V_func(NULL);
            DIE("line %d: undefined variable '%s'", n->line, n->u.var.name);
        }
        return v;
    }
    case N_ASSIGN:
    {
        Value v = eval(vm, env, n->u.assign.expr);
        env_set(env, n->u.assign.name, v);
        return v;
    }
    case N_UN:
    {
        Value a = eval(vm, env, n->u.un.a);
        if (n->u.un.op == T_KNOT)
            return V_bool(!is_truthy(a));
        if (n->u.un.op == T_MINUS)
            return V_num(-as_num(n, a));
        DIE("line %d: bad unary op", n->line);
    }
    case N_BIN:
    {
        // short-circuit for and/or
        if (n->u.bin.op == T_KAND)
        {
            Value a = eval(vm, env, n->u.bin.a);
            if (!is_truthy(a))
                return a;
            return eval(vm, env, n->u.bin.b);
        }
        if (n->u.bin.op == T_KOR)
        {
            Value a = eval(vm, env, n->u.bin.a);
            if (is_truthy(a))
                return a;
            return eval(vm, env, n->u.bin.b);
        }
        Value A = eval(vm, env, n->u.bin.a);
        Value B = eval(vm, env, n->u.bin.b);
        switch (n->u.bin.op)
        {
        case T_PLUS:
            return V_num(as_num(n, A) + as_num(n, B));
        case T_MINUS:
            return V_num(as_num(n, A) - as_num(n, B));
        case T_STAR:
            return V_num(as_num(n, A) * as_num(n, B));
        case T_SLASH:
            return V_num(as_num(n, A) / as_num(n, B));
        case T_PCT:
            return V_num(fmod(as_num(n, A), as_num(n, B)));
        case T_CARET:
            return V_num(pow(as_num(n, A), as_num(n, B)));
        case T_EQ:
            return V_bool(A.t == B.t && ((A.t == V_NUM && A.u.num == B.u.num) ||
                                         (A.t == V_BOOL && A.u.boolean == B.u.boolean) ||
                                         (A.t == V_NIL) ||
                                         (A.t == V_STR && strcmp(A.u.str, B.u.str) == 0) ||
                                         (A.t == V_FUNC && A.u.func == B.u.func)));
        case T_NE:
        {
            Value eq = eval(vm, env, n); /* unreachable */
            (void)eq;
            DIE("internal");
        }
        case T_LT:
            return V_bool(as_num(n, A) < as_num(n, B));
        case T_LE:
            return V_bool(as_num(n, A) <= as_num(n, B));
        case T_GT:
            return V_bool(as_num(n, A) > as_num(n, B));
        case T_GE:
            return V_bool(as_num(n, A) >= as_num(n, B));
        default:
            DIE("line %d: bad binop", n->line);
        }
    }
    case N_CALL:
    {
        // builtin print(...)
        if (strcmp(n->u.call.name, "print") == 0)
        {
            int m = n->u.call.args.n;
            Value *argv = (Value *)alloca(sizeof(Value) * m);
            for (int i = 0; i < m; i++)
                argv[i] = eval(vm, env, (AST *)n->u.call.args.d[i]);
            return builtin_print(m, argv);
        }
        // user function
        Value f;
        if (!env_get(env, n->u.call.name, &f) || f.t != V_FUNC)
            DIE("line %d: attempt to call non-function '%s'", n->line, n->u.call.name);
        int m = n->u.call.args.n;
        Value *argv = (Value *)alloca(sizeof(Value) * m);
        for (int i = 0; i < m; i++)
            argv[i] = eval(vm, env, (AST *)n->u.call.args.d[i]);
        // function env is current env (no closures), typical simple dynamic env
        return call_function(vm, env, f.u.func, m, argv);
    }
    case N_IF:
    {
        Value c = eval(vm, env, n->u.ifs.cond);
        if (is_truthy(c))
            return eval(vm, env, n->u.ifs.thn);
        if (n->u.ifs.els)
            return eval(vm, env, n->u.ifs.els);
        return V_nil();
    }
    case N_WHILE:
    {
        while (is_truthy(eval(vm, env, n->u.whil.cond)))
            eval(vm, env, n->u.whil.body);
        return V_nil();
    }
    case N_FUNDEF:
    {
        // store function node in environment
        env_set(env, n->u.fundef.name, V_func(n));
        return V_nil();
    }
    case N_RETURN:
    {
        Value r = V_nil();
        if (n->u.ret.exprs.n > 0)
            r = eval(vm, env, (AST *)n->u.ret.exprs.d[0]);
        if (!vm->rj.active)
            DIE("line %d: 'return' outside function", n->line);
        vm->rj.retv = r;
        longjmp(vm->rj.jb, 1);
    }
    }
    DIE("line %d: unhandled node", n->line);
    return V_nil();
}

/*======================== Runner / REPL ========================*/
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        DIE("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(n + 1);
    if (!buf)
        DIE("oom");
    if (fread(buf, 1, n, f) != (size_t)n)
        DIE("read fail");
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void run_code(const char *code)
{
    AST *prog = parse_chunk(code);
    VM vm = {0};
    vm.G = env_new(NULL);
    eval(&vm, vm.G, prog);
}

static void repl(void)
{
    printf("luette — tiny Lua-like (subset) interpreter. Type Ctrl-D to exit.\n");
    Env *G = env_new(NULL);
    VM vm = {0};
    vm.G = G;
    for (;;)
    {
        printf(">> ");
        fflush(stdout);
        char line[4096];
        if (!fgets(line, sizeof(line), stdin))
        {
            printf("\n");
            break;
        }
        // allow multi-line via trailing backslash
        size_t L = strlen(line);
        char *code = sdup(line);
        while (L > 0 && code[L - 1] == '\\')
        {
            code[L - 1] = '\n';
            printf(".. ");
            char cont[4096];
            if (!fgets(cont, sizeof(cont), stdin))
                break;
            size_t cL = strlen(cont);
            char *old = code;
            code = (char *)malloc(L + cL + 1);
            if (!code)
                DIE("oom");
            memcpy(code, old, L);
            memcpy(code + L, cont, cL + 1);
            L += cL;
            free(old);
        }
        // parse & eval
        AST *prog = parse_chunk(code);
        eval(&vm, G, prog);
        free(code);
    }
}

/*======================== Embedded Demo Script ========================*/
/*======================== Embedded Demo Script ========================*/
static const char DEMO_LUA[] =
"-- factorial\n"
"function fact(n)\n"
"  if n == 0 then return 1 end\n"
"  return n * fact(n-1)\n"
"end\n"
"\n"
"x = 5\n"
"print(\"fact:\", fact(x))\n"
"\n"
"-- loop\n"
"i = 1\n"
"while i <= 5 do\n"
"  print(i, i^2)\n"
"  i = i + 1\n"
"end\n";


/*======================== Main ========================*/

int main(int argc, char **argv)
{
    // Usage:
    //   ./luette                 -> run embedded demo (or change to REPL below)
    //   ./luette --demo          -> run embedded demo
    //   ./luette --repl          -> interactive REPL
    //   ./luette script.lu       -> run file

    if (argc == 1)
    {
        // Default behavior: run the embedded demo.
        run_code(DEMO_LUA);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--demo") == 0)
    {
        run_code(DEMO_LUA);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--repl") == 0)
    {
        repl();
        return 0;
    }

    // Otherwise, treat argv[1] as a file path
    char *code = read_file(argv[1]);
    run_code(code);
    free(code);
    return 0;
}
