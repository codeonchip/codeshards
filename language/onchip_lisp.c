#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ================= Embedded demo program ================= */
static const char *demo_program =
    "; Lisp demo\n"
    "; factorial, higher-order map, and closures\n"
    "(define (fact n)\n"
    "  (if (<= n 1) 1 (* n (fact (- n 1)))))\n"
    "(print (fact 5))                 ; 120\n"
    "\n"
    "(define (map f xs)\n"
    "  (if (null? xs)\n"
    "      '()\n"
    "      (cons (f (car xs)) (map f (cdr xs)))))\n"
    "(print (map (lambda (x) (* x x)) '(1 2 3 4)))\n"
    "\n"
    "(define (make-counter)\n"
    "  (let ((x 0))\n"
    "    (lambda ()\n"
    "      (set! x (+ x 1))\n"
    "      x)))\n"
    "(define c (make-counter))\n"
    "(print (c)) ; 1\n"
    "(print (c)) ; 2\n"
    "(print (equal? '(a (b c)) '(a (b c)))) ; #t\n";

/* =============== Core Value Types =============== */
typedef struct LVal LVal;
typedef struct Env Env;
typedef LVal *(*CFn)(Env *, LVal *args);

typedef enum
{
    T_NIL,
    T_NUM,
    T_SYM,
    T_STR,
    T_CONS,
    T_FUNC,
    T_LAMBDA
} LType;

struct LVal
{
    LType t;
    union
    {
        double num;
        char *sym;
        char *str;
        struct
        {
            LVal *car, *cdr;
        } cons;
        struct
        {
            CFn fn;
            const char *name;
        } func;
        struct
        {
            LVal *params;
            LVal *body;
            Env *env;
        } lam;
    } u;
};

struct Env
{
    struct Env *parent;
    char **names;
    LVal **vals;
    int count, cap;
};

static LVal *NIL;      /* singleton */
static LVal *TRUE_SYM; /* prints as #t */
static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p)
    {
        fprintf(stderr, "OOM\n");
        exit(1);
    }
    return p;
}
static char *strdup2(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* constructors */
static LVal *l_nil(void) { return NIL; }
static LVal *l_num(double v)
{
    LVal *x = (LVal *)xmalloc(sizeof *x);
    x->t = T_NUM;
    x->u.num = v;
    return x;
}
static LVal *l_sym(const char *s)
{
    LVal *x = (LVal *)xmalloc(sizeof *x);
    x->t = T_SYM;
    x->u.sym = strdup2(s);
    return x;
}
static LVal *l_str(const char *s)
{
    LVal *x = (LVal *)xmalloc(sizeof *x);
    x->t = T_STR;
    x->u.str = strdup2(s);
    return x;
}
static LVal *l_cons(LVal *a, LVal *d)
{
    LVal *x = (LVal *)xmalloc(sizeof *x);
    x->t = T_CONS;
    x->u.cons.car = a;
    x->u.cons.cdr = d;
    return x;
}
static LVal *l_func(CFn f, const char *name)
{
    LVal *x = (LVal *)xmalloc(sizeof *x);
    x->t = T_FUNC;
    x->u.func.fn = f;
    x->u.func.name = name;
    return x;
}
static LVal *l_lambda(LVal *params, LVal *body, Env *env)
{
    LVal *x = (LVal *)xmalloc(sizeof *x);
    x->t = T_LAMBDA;
    x->u.lam.params = params;
    x->u.lam.body = body;
    x->u.lam.env = env;
    return x;
}

/* list helpers */
static int is_nil(LVal *v) { return v == NIL; }
static LVal *car(LVal *v) { return (v->t == T_CONS) ? v->u.cons.car : NIL; }
static LVal *cdr(LVal *v) { return (v->t == T_CONS) ? v->u.cons.cdr : NIL; }
static int is_list(LVal *v)
{
    while (v->t == T_CONS)
        v = v->u.cons.cdr;
    return is_nil(v);
}
static int list_len(LVal *v)
{
    int n = 0;
    while (v->t == T_CONS)
    {
        n++;
        v = v->u.cons.cdr;
    }
    return n;
}

/* =============== Printing =============== */
static void print_val(LVal *v);
static void print_list(LVal *v)
{
    putchar('(');
    int first = 1;
    while (v->t == T_CONS)
    {
        if (!first)
            putchar(' ');
        print_val(v->u.cons.car);
        first = 0;
        v = v->u.cons.cdr;
    }
    if (!is_nil(v))
    {
        printf(" . ");
        print_val(v);
    }
    putchar(')');
}
static void print_val(LVal *v)
{
    switch (v->t)
    {
    case T_NIL:
        printf("()");
        break;
    case T_NUM:
    {
        double z = v->u.num;
        if (z == (long long)z)
            printf("%lld", (long long)z);
        else
            printf("%.15g", z);
        break;
    }
    case T_SYM:
    {
        if (v == TRUE_SYM)
            printf("#t");
        else
            printf("%s", v->u.sym);
        break;
    }
    case T_STR:
    {
        putchar('"');
        const char *s = v->u.str;
        for (; *s; ++s)
        {
            if (*s == '"' || *s == '\\')
                printf("\\%c", *s);
            else if (*s == '\n')
                printf("\\n");
            else if (*s == '\t')
                printf("\\t");
            else
                putchar(*s);
        }
        putchar('"');
        break;
    }
    case T_CONS:
        print_list(v);
        break;
    case T_FUNC:
        printf("#<builtin:%s>", v->u.func.name ? v->u.func.name : "?");
        break;
    case T_LAMBDA:
        printf("#<procedure>");
        break;
    }
}

/* =============== Environment =============== */
static Env *env_new(Env *parent)
{
    Env *e = (Env *)xmalloc(sizeof *e);
    e->parent = parent;
    e->names = NULL;
    e->vals = NULL;
    e->count = 0;
    e->cap = 0;
    return e;
}
static void env_def(Env *e, const char *name, LVal *val)
{
    for (int i = 0; i < e->count; i++)
        if (!strcmp(e->names[i], name))
        {
            e->vals[i] = val;
            return;
        }
    if (e->count >= e->cap)
    {
        e->cap = e->cap ? e->cap * 2 : 16;
        e->names = (char **)realloc(e->names, (size_t)e->cap * sizeof(char *));
        e->vals = (LVal **)realloc(e->vals, (size_t)e->cap * sizeof(LVal *));
    }
    e->names[e->count] = strdup2(name);
    e->vals[e->count] = val;
    e->count++;
}
static int env_set(Env *e, const char *name, LVal *val)
{
    for (Env *p = e; p; p = p->parent)
    {
        for (int i = 0; i < p->count; i++)
            if (!strcmp(p->names[i], name))
            {
                p->vals[i] = val;
                return 1;
            }
    }
    return 0;
}
static LVal *env_get(Env *e, const char *name)
{
    for (Env *p = e; p; p = p->parent)
    {
        for (int i = 0; i < p->count; i++)
            if (!strcmp(p->names[i], name))
                return p->vals[i];
    }
    fprintf(stderr, "unbound symbol: %s\n", name);
    exit(1);
}

/* =============== Reader (tokenizer + parser) =============== */
typedef enum
{
    TK_EOF = 0,
    TK_LP,
    TK_RP,
    TK_QUOTE,
    TK_STR,
    TK_NUM,
    TK_SYM
} Tok;
typedef struct
{
    Tok t;
    char text[256];
    double num;
    int line, col;
} Token;
typedef struct
{
    const char *s;
    size_t i, n;
    int line, col;
    Token cur;
} Lexer;

static int l_peek(Lexer *L) { return (L->i < L->n) ? (unsigned char)L->s[L->i] : 0; }
static int l_get(Lexer *L)
{
    int c = l_peek(L);
    if (c)
    {
        L->i++;
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
static void l_skip(Lexer *L)
{
    for (;;)
    {
        int c = l_peek(L);
        if (!c)
            return;
        if (c == ';')
        {
            while (l_peek(L) && l_peek(L) != '\n')
                l_get(L);
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            l_get(L);
        else
            break;
    }
}
static int is_sym1(int c) { return isalpha(c) || strchr("+-*/<>=!?_.$:%", c) != NULL; }
static int is_symn(int c) { return isalnum(c) || strchr("+-*/<>=!?_.$:%", c) != NULL; }

static void next_tok(Lexer *L)
{
    l_skip(L);
    Token t;
    t.t = TK_EOF;
    t.text[0] = '\0';
    t.num = 0;
    t.line = L->line;
    t.col = L->col;
    int c = l_peek(L);
    if (!c)
    {
        L->cur = t;
        return;
    }
    if (c == '(')
    {
        l_get(L);
        t.t = TK_LP;
        L->cur = t;
        return;
    }
    if (c == ')')
    {
        l_get(L);
        t.t = TK_RP;
        L->cur = t;
        return;
    }
    if (c == '\'')
    {
        l_get(L);
        t.t = TK_QUOTE;
        L->cur = t;
        return;
    }
    if (c == '"')
    {
        l_get(L);
        int n = 0;
        while (l_peek(L) && l_peek(L) != '"')
        {
            int d = l_get(L);
            if (d == '\\' && l_peek(L))
            {
                int e = l_get(L);
                if (e == 'n')
                    d = '\n';
                else if (e == 't')
                    d = '\t';
                else
                    d = e;
            }
            if (n < 255)
                t.text[n++] = (char)d;
        }
        if (l_peek(L) == '"')
            l_get(L);
        t.text[n] = '\0';
        t.t = TK_STR;
        L->cur = t;
        return;
    }
    if (isdigit(c) || (c == '-' && isdigit((unsigned char)L->s[L->i + 1])))
    {
        char buf[256];
        int n = 0, dot = 0;
        if (c == '-')
        {
            buf[n++] = (char)l_get(L);
        }
        while (isdigit(l_peek(L)) || l_peek(L) == '.')
        {
            int d = l_get(L);
            if (d == '.')
            {
                if (dot)
                    break;
                dot = 1;
            }
            if (n < 255)
                buf[n++] = (char)d;
        }
        buf[n] = '\0';
        t.t = TK_NUM;
        t.num = strtod(buf, NULL);
        L->cur = t;
        return;
    }
    if (is_sym1(c))
    {
        int n = 0;
        while (is_symn(l_peek(L)) && n < 255)
            t.text[n++] = (char)l_get(L);
        t.text[n] = '\0';
        t.t = TK_SYM;
        L->cur = t;
        return;
    }
    fprintf(stderr, "lex error %d:%d char '%c'\n", L->line, L->col, c);
    exit(1);
}

static LVal *read_expr(Lexer *L);

static LVal *read_list(Lexer *L)
{
    next_tok(L); /* move past '(' already consumed by caller? No: caller consumes it; this is helper if needed */
    return NIL;
}
static LVal *read_after_lp(Lexer *L)
{ /* list items until ')' */
    LVal *head = NIL, *tail = NULL;
    for (next_tok(L); L->cur.t != TK_RP; next_tok(L))
    {
        if (L->cur.t == TK_EOF)
        {
            fprintf(stderr, "parse error: unexpected EOF in list\n");
            exit(1);
        }
        /* don't consume token here; read_expr uses current token */
        /* fake "push back": we pass control to read_expr with current token */
        /* To do that, create a copy of lexer state ... instead we write read_expr that consumes current token directly */
        /* So: call read_expr that expects current token to be the first of an expression */
        /* But our next_tok already advanced. We'll need a small wrapper: keep current token as-is and let read_expr consume it. */
        /* Implement read_expr to accept the current token. */
        /* Here, we just build list by repeatedly calling read_expr with existing current token. */

        /* read one expression from current token */
        /* Save token, but read_expr will use L->cur directly. */
        LVal *it;
        /* call read_expr using current token (no extra next_tok) */
        /* Implement read_expr accordingly. */
        /* fall-through handled below */
        /* We'll implement read_expr so that it does NOT call next_tok first; it expects L->cur already set. */
        /* So we must NOT call next_tok here at loop head; we've already done one. Adjust: move next_tok outside. */
        fprintf(stderr, "internal parser sync error\n");
        exit(1);
        (void)it;
    }
    /* never reached */
    return head;
}

/* read_expr: assumes L->cur already holds the next token to read */
static LVal *parse_list_items(Lexer *L); /* fwd */

static LVal *read_expr(Lexer *L)
{
    switch (L->cur.t)
    {
    case TK_NUM:
    {
        LVal *v = l_num(L->cur.num);
        next_tok(L);
        return v;
    }
    case TK_STR:
    {
        LVal *v = l_str(L->cur.text);
        next_tok(L);
        return v;
    }
    case TK_SYM:
    {
        if (strcmp(L->cur.text, "nil") == 0 || strcmp(L->cur.text, "()") == 0)
        {
            next_tok(L);
            return NIL;
        }
        LVal *v = l_sym(L->cur.text);
        next_tok(L);
        return v;
    }
    case TK_QUOTE:
    {
        next_tok(L); /* read quoted expr */
        LVal *q = read_expr(L);
        return l_cons(l_sym("quote"), l_cons(q, NIL));
    }
    case TK_LP:
    {
        next_tok(L); /* move to first item or ')' */
        /* read items until ')' */
        LVal *head = NIL, *tail = NULL;
        while (L->cur.t != TK_RP)
        {
            if (L->cur.t == TK_EOF)
            {
                fprintf(stderr, "parse error: unexpected EOF in list\n");
                exit(1);
            }
            LVal *it = read_expr(L); /* read_expr advances tokens */
            if (is_nil(head))
            {
                head = tail = l_cons(it, NIL);
            }
            else
            {
                tail->u.cons.cdr = l_cons(it, NIL);
                tail = tail->u.cons.cdr;
            }
        }
        next_tok(L); /* consume ')' */
        return head;
    }
    default:
        fprintf(stderr, "parse error %d:%d: unexpected token\n", L->cur.line, L->cur.col);
        exit(1);
    }
}

/* =============== Eval =============== */
static LVal *eval(Env *e, LVal *v);

static LVal *evlist(Env *e, LVal *lst)
{ /* evaluate each arg into a new list */
    if (is_nil(lst))
        return NIL;
    LVal *h = l_cons(eval(e, car(lst)), NIL);
    LVal *t = h;
    for (lst = cdr(lst); !is_nil(lst); lst = cdr(lst))
    {
        t->u.cons.cdr = l_cons(eval(e, car(lst)), NIL);
        t = t->u.cons.cdr;
    }
    return h;
}

/* helper predicates */
static int truthy(LVal *v) { return !is_nil(v); }
static int is_symbol(LVal *v, const char *s) { return v->t == T_SYM && strcmp(v->u.sym, s) == 0; }

/* forward for builtins registration */
static void install_builtins(Env *g);

/* apply */
static LVal *apply(Env *e, LVal *f, LVal *args)
{
    if (f->t == T_FUNC)
        return f->u.func.fn(e, args);
    if (f->t == T_LAMBDA)
    {
        /* bind parameters to args in new env */
        Env *call = env_new(f->u.lam.env);
        LVal *ps = f->u.lam.params;
        LVal *as = args;
        while (!is_nil(ps) && ps->t == T_CONS)
        {
            if (is_nil(as))
            {
                fprintf(stderr, "arity mismatch (too few args)\n");
                exit(1);
            }
            if (car(ps)->t != T_SYM)
            {
                fprintf(stderr, "lambda param must be symbol\n");
                exit(1);
            }
            env_def(call, car(ps)->u.sym, car(as));
            ps = cdr(ps);
            as = cdr(as);
        }
        if (!is_nil(ps))
        { /* variadic: (x y . rest) -> not implemented; simple check */
            fprintf(stderr, "variadic params not supported in this tiny interpreter\n");
            exit(1);
        }
        if (!is_nil(as))
        {
            fprintf(stderr, "arity mismatch (too many args)\n");
            exit(1);
        }

        /* body is (begin ...), or sequence; evaluate sequentially */
        LVal *last = NIL;
        for (LVal *b = f->u.lam.body; !is_nil(b); b = cdr(b))
        {
            last = eval(call, car(b));
        }
        return last;
    }
    fprintf(stderr, "attempt to call non-callable value\n");
    exit(1);
}

/* eval */
static LVal *eval(Env *e, LVal *v)
{
    /* atoms */
    switch (v->t)
    {
    case T_NUM:
    case T_STR:
    case T_FUNC:
    case T_LAMBDA:
    case T_NIL:
        return v;
    case T_SYM:
        return env_get(e, v->u.sym);
    default:
        break;
    }
    /* list */
    if (is_nil(v))
        return v;

    LVal *op = car(v);
    LVal *args = cdr(v);

    /* special forms */
    if (op->t == T_SYM)
    {
        /* quote */
        if (is_symbol(op, "quote"))
            return car(args);

        /* if */
        if (is_symbol(op, "if"))
        {
            LVal *cond = eval(e, car(args));
            LVal *thenb = car(cdr(args));
            LVal *elseb = car(cdr(cdr(args)));
            return truthy(cond) ? eval(e, thenb) : eval(e, elseb ? elseb : NIL);
        }

        /* begin */
        if (is_symbol(op, "begin"))
        {
            LVal *last = NIL;
            for (LVal *it = args; !is_nil(it); it = cdr(it))
                last = eval(e, car(it));
            return last;
        }

        /* define: (define name expr) or (define (f x y) body...) */
        if (is_symbol(op, "define"))
        {
            LVal *head = car(args);
            if (head && head->t == T_CONS && car(head)->t == T_SYM)
            {
                /* function form */
                LVal *fname = car(head);
                LVal *params = cdr(head);
                LVal *body = cdr(args);
                LVal *lam = l_lambda(params, body, e);
                env_def(e, fname->u.sym, lam);
                return fname;
            }
            else
            {
                /* variable form */
                if (head->t != T_SYM)
                {
                    fprintf(stderr, "define name must be symbol\n");
                    exit(1);
                }
                LVal *val = eval(e, car(cdr(args)));
                env_def(e, head->u.sym, val);
                return head;
            }
        }

        /* set! */
        if (is_symbol(op, "set!"))
        {
            LVal *name = car(args);
            if (name->t != T_SYM)
            {
                fprintf(stderr, "set!: first arg must be symbol\n");
                exit(1);
            }
            LVal *val = eval(e, car(cdr(args)));
            if (!env_set(e, name->u.sym, val))
            {
                fprintf(stderr, "set!: unbound variable %s\n", name->u.sym);
                exit(1);
            }
            return val;
        }

        /* lambda */
        if (is_symbol(op, "lambda"))
        {
            LVal *params = car(args);
            LVal *body = cdr(args);
            return l_lambda(params, body, e);
        }

        /* let (simple sugar): (let ((x e1) (y e2)) body...) */
        if (is_symbol(op, "let"))
        {
            LVal *bindings = car(args);
            LVal *body = cdr(args);
            /* transform into ((lambda (vars...) body...) vals...) */
            LVal *vars = NIL, *vars_t = NULL, *vals = NIL, *vals_t = NULL;
            for (; !is_nil(bindings); bindings = cdr(bindings))
            {
                LVal *pair = car(bindings);
                LVal *nm = car(pair);
                LVal *ex = car(cdr(pair));
                if (is_nil(vars))
                {
                    vars = vars_t = l_cons(nm, NIL);
                }
                else
                {
                    vars_t->u.cons.cdr = l_cons(nm, NIL);
                    vars_t = vars_t->u.cons.cdr;
                }
                LVal *ev = eval(e, ex);
                if (is_nil(vals))
                {
                    vals = vals_t = l_cons(ev, NIL);
                }
                else
                {
                    vals_t->u.cons.cdr = l_cons(ev, NIL);
                    vals_t = vals_t->u.cons.cdr;
                }
            }
            LVal *lam = l_lambda(vars, body, e);
            return apply(e, lam, vals);
        }

        /* and/or (short-circuit) */
        if (is_symbol(op, "and"))
        {
            LVal *last = TRUE_SYM;
            for (LVal *it = args; !is_nil(it); it = cdr(it))
            {
                last = eval(e, car(it));
                if (!truthy(last))
                    return NIL;
            }
            return last;
        }
        if (is_symbol(op, "or"))
        {
            for (LVal *it = args; !is_nil(it); it = cdr(it))
            {
                LVal *v2 = eval(e, car(it));
                if (truthy(v2))
                    return v2;
            }
            return NIL;
        }
    }

    /* normal application */
    LVal *fn = eval(e, op);
    LVal *ev = evlist(e, args);
    return apply(e, fn, ev);
}

/* =============== Built-ins =============== */
#define ENSURE_NUM(x, where)                                 \
    do                                                       \
    {                                                        \
        if ((x)->t != T_NUM)                                 \
        {                                                    \
            fprintf(stderr, "%s: expected number\n", where); \
            exit(1);                                         \
        }                                                    \
    } while (0)
#define ENSURE_PAIR(x, where)                                   \
    do                                                          \
    {                                                           \
        if ((x)->t != T_CONS)                                   \
        {                                                       \
            fprintf(stderr, "%s: expected pair/list\n", where); \
            exit(1);                                            \
        }                                                       \
    } while (0)

static LVal *b_add(Env *e, LVal *a)
{
    (void)e;
    double s = 0;
    for (; !is_nil(a); a = cdr(a))
    {
        ENSURE_NUM(car(a), "+");
        s += car(a)->u.num;
    }
    return l_num(s);
}
static LVal *b_sub(Env *e, LVal *a)
{
    (void)e;
    if (is_nil(a))
        return l_num(0);
    ENSURE_NUM(car(a), "-");
    double s = car(a)->u.num;
    if (is_nil(cdr(a)))
        return l_num(-s);
    for (a = cdr(a); !is_nil(a); a = cdr(a))
    {
        ENSURE_NUM(car(a), "-");
        s -= car(a)->u.num;
    }
    return l_num(s);
}
static LVal *b_mul(Env *e, LVal *a)
{
    (void)e;
    double p = 1;
    for (; !is_nil(a); a = cdr(a))
    {
        ENSURE_NUM(car(a), "*");
        p *= car(a)->u.num;
    }
    return l_num(p);
}
static LVal *b_div(Env *e, LVal *a)
{
    (void)e;
    ENSURE_NUM(car(a), "/");
    double v = car(a)->u.num;
    for (a = cdr(a); !is_nil(a); a = cdr(a))
    {
        ENSURE_NUM(car(a), "/");
        double d = car(a)->u.num;
        if (d == 0)
        {
            fprintf(stderr, "/: divide by zero\n");
            exit(1);
        }
        v /= d;
    }
    return l_num(v);
}

static LVal *bool_ret(int b) { return b ? TRUE_SYM : NIL; }
static LVal *cmp_core(const char *who, LVal *a, int (*pred)(double, double))
{
    if (is_nil(a) || is_nil(cdr(a)))
    {
        fprintf(stderr, "%s: need at least 2 args\n", who);
        exit(1);
    }
    ENSURE_NUM(car(a), who);
    double prev = car(a)->u.num;
    a = cdr(a);
    while (!is_nil(a))
    {
        ENSURE_NUM(car(a), who);
        double x = car(a)->u.num;
        if (!pred(prev, x))
            return NIL;
        prev = x;
        a = cdr(a);
    }
    return TRUE_SYM;
}
static int p_eq(double a, double b) { return a == b; }
static int p_lt(double a, double b) { return a < b; }
static int p_le(double a, double b) { return a <= b; }
static int p_gt(double a, double b) { return a > b; }
static int p_ge(double a, double b) { return a >= b; }

static LVal *b_num_eq(Env *e, LVal *a)
{
    (void)e;
    return cmp_core("=", a, p_eq);
}
static LVal *b_lt(Env *e, LVal *a)
{
    (void)e;
    return cmp_core("<", a, p_lt);
}
static LVal *b_le(Env *e, LVal *a)
{
    (void)e;
    return cmp_core("<=", a, p_le);
}
static LVal *b_gt(Env *e, LVal *a)
{
    (void)e;
    return cmp_core(">", a, p_gt);
}
static LVal *b_ge(Env *e, LVal *a)
{
    (void)e;
    return cmp_core(">=", a, p_ge);
}

static LVal *b_cons(Env *e, LVal *a)
{
    (void)e;
    if (is_nil(a) || is_nil(cdr(a)))
    {
        fprintf(stderr, "cons: need 2 args\n");
        exit(1);
    }
    return l_cons(car(a), car(cdr(a)));
}
static LVal *b_car(Env *e, LVal *a)
{
    (void)e;
    ENSURE_PAIR(car(a), "car");
    return car(car(a));
}
static LVal *b_cdr(Env *e, LVal *a)
{
    (void)e;
    ENSURE_PAIR(car(a), "cdr");
    return cdr(car(a));
}

static LVal *b_pairp(Env *e, LVal *a)
{
    (void)e;
    return bool_ret(car(a)->t == T_CONS);
}
static LVal *b_nullp(Env *e, LVal *a)
{
    (void)e;
    return bool_ret(is_nil(car(a)));
}
static LVal *b_listp(Env *e, LVal *a)
{
    (void)e;
    return bool_ret(is_list(car(a)));
}
static LVal *b_numberp(Env *e, LVal *a)
{
    (void)e;
    return bool_ret(car(a)->t == T_NUM);
}
static LVal *b_symbolp(Env *e, LVal *a)
{
    (void)e;
    return bool_ret(car(a)->t == T_SYM);
}
static LVal *b_procp(Env *e, LVal *a)
{
    (void)e;
    return bool_ret(car(a)->t == T_FUNC || car(a)->t == T_LAMBDA);
}

static int equal_rec(LVal *x, LVal *y)
{
    if (x->t != y->t)
        return 0;
    switch (x->t)
    {
    case T_NIL:
        return 1;
    case T_NUM:
        return x->u.num == y->u.num;
    case T_SYM:
        return strcmp(x->u.sym, y->u.sym) == 0;
    case T_STR:
        return strcmp(x->u.str, y->u.str) == 0;
    case T_CONS:
        return equal_rec(x->u.cons.car, y->u.cons.car) && equal_rec(x->u.cons.cdr, y->u.cons.cdr);
    case T_FUNC:
        return x->u.func.fn == y->u.func.fn;
    case T_LAMBDA:
        return x == y; /* simplistic */
    }
    return 0;
}
static LVal *b_eq(Env *e, LVal *a)
{
    (void)e;
    if (is_nil(a) || is_nil(cdr(a)))
    {
        fprintf(stderr, "eq?: need 2 args\n");
        exit(1);
    }
    return bool_ret(car(a) == car(cdr(a)));
}
static LVal *b_equal(Env *e, LVal *a)
{
    (void)e;
    if (is_nil(a) || is_nil(cdr(a)))
    {
        fprintf(stderr, "equal?: need 2 args\n");
        exit(1);
    }
    return bool_ret(equal_rec(car(a), car(cdr(a))));
}

static LVal *b_list(Env *e, LVal *a)
{
    (void)e;
    return a;
}
static LVal *b_display(Env *e, LVal *a)
{
    (void)e;
    for (; !is_nil(a); a = cdr(a))
    {
        LVal *v = car(a);
        if (v->t == T_STR)
            printf("%s", v->u.str);
        else
            print_val(v);
    }
    return NIL;
}
static LVal *b_print(Env *e, LVal *a)
{
    (void)e;
    for (; !is_nil(a); a = cdr(a))
    {
        if (a != NULL && !is_nil(a) && a != a)
        {
        }
        print_val(car(a));
        if (!is_nil(cdr(a)))
            putchar(' ');
    }
    putchar('\n');
    return NIL;
}
static LVal *b_newline(Env *e, LVal *a)
{
    (void)e;
    (void)a;
    putchar('\n');
    return NIL;
}

static void install_builtins(Env *g)
{
    env_def(g, "+", l_func(b_add, "+"));
    env_def(g, "-", l_func(b_sub, "-"));
    env_def(g, "*", l_func(b_mul, "*"));
    env_def(g, "/", l_func(b_div, "/"));
    env_def(g, "=", l_func(b_num_eq, "="));
    env_def(g, "<", l_func(b_lt, "<"));
    env_def(g, "<=", l_func(b_le, "<="));
    env_def(g, ">", l_func(b_gt, ">"));
    env_def(g, ">=", l_func(b_ge, ">="));
    env_def(g, "cons", l_func(b_cons, "cons"));
    env_def(g, "car", l_func(b_car, "car"));
    env_def(g, "cdr", l_func(b_cdr, "cdr"));
    env_def(g, "pair?", l_func(b_pairp, "pair?"));
    env_def(g, "null?", l_func(b_nullp, "null?"));
    env_def(g, "list?", l_func(b_listp, "list?"));
    env_def(g, "number?", l_func(b_numberp, "number?"));
    env_def(g, "symbol?", l_func(b_symbolp, "symbol?"));
    env_def(g, "procedure?", l_func(b_procp, "procedure?"));
    env_def(g, "eq?", l_func(b_eq, "eq?"));
    env_def(g, "equal?", l_func(b_equal, "equal?"));
    env_def(g, "list", l_func(b_list, "list"));
    env_def(g, "display", l_func(b_display, "display"));
    env_def(g, "print", l_func(b_print, "print"));
    env_def(g, "newline", l_func(b_newline, "newline"));
    env_def(g, "#t", TRUE_SYM);
}

/* =============== Driver =============== */
static char *load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)xmalloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    /* init singletons */
    NIL = (LVal *)xmalloc(sizeof *NIL);
    NIL->t = T_NIL;
    TRUE_SYM = l_sym("#t");

    /* global env */
    Env *G = env_new(NULL);
    install_builtins(G);

    /* load program */
    const char *src = demo_program;
    char *heap = NULL;
    if (argc > 1)
    {
        heap = load_file(argv[1]);
        if (!heap)
        {
            fprintf(stderr, "could not read '%s'\n", argv[1]);
            return 1;
        }
        src = heap;
    }

    /* lex + parse + eval top-level forms */
    Lexer L = {.s = src, .i = 0, .n = strlen(src), .line = 1, .col = 1};
    next_tok(&L);
    while (L.cur.t != TK_EOF)
    {
        LVal *expr = read_expr(&L);
        LVal *res = eval(G, expr);
        /* print result of top-level evaluations that aren't function/define returning symbols */
        if (res != NIL && res->t != T_LAMBDA)
        { /* keep it friendly and REPL-like */
            print_val(res);
            putchar('\n');
        }
    }

    free(heap);
    return 0;
}
