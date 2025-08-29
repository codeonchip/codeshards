#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============ Embedded demo program ============ */
static const char *demo_program =
    "% Prolog-like demo\n"
    "parent(alice, bob).\n"
    "parent(bob, carol).\n"
    "parent(alice, dana).\n"
    "male(bob).\n"
    "female(alice).\n"
    "\n"
    "ancestor(X,Y) :- parent(X,Y).\n"
    "ancestor(X,Y) :- parent(X,Z), ancestor(Z,Y).\n"
    "\n"
    "write_list([]).\n"
    "write_list([H|T]) :- write(H), write(' '), write_list(T).\n"
    "\n"
    "?- ancestor(A, carol), write('A='), write(A), nl.\n";

/* ============ Utilities ============ */
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

/* ============ Lexer ============ */

typedef enum
{
    TK_EOF = 0,
    TK_DOT,
    TK_COMMA,
    TK_LP,
    TK_RP,
    TK_LB,
    TK_RB,
    TK_BAR,
    TK_NECK,
    TK_QUERY,
    TK_ATOM,
    TK_VAR,
    TK_NUM,
    TK_QUOTED
} Tok;

typedef struct
{
    Tok t;
    char text[128];
    double num;
    int line, col;
} Token;

typedef struct
{
    const char *src;
    size_t pos, len;
    int line, col;
    Token cur;
} Lexer;

static int lx_peek(Lexer *L) { return (L->pos < L->len) ? (unsigned char)L->src[L->pos] : 0; }
static int lx_peek2(Lexer *L) { return (L->pos + 1 < L->len) ? (unsigned char)L->src[L->pos + 1] : 0; }
static int lx_get(Lexer *L)
{
    int c = lx_peek(L);
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

static void lx_skip_ws_and_comments(Lexer *L)
{
    for (;;)
    {
        int c = lx_peek(L);
        if (!c)
            return;
        if (c == '%')
        {
            while ((c = lx_peek(L)) && c != '\n')
                lx_get(L);
            continue;
        } /* % line comment */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            lx_get(L);
            continue;
        }
        break;
    }
}

static void lx_next(Lexer *L)
{
    lx_skip_ws_and_comments(L);
    int line = L->line, col = L->col;
    Token t;
    memset(&t, 0, sizeof t);
    t.line = line;
    t.col = col;
    t.t = TK_EOF;

    int c = lx_peek(L);
    if (!c)
    {
        L->cur = t;
        return;
    }

    /* punctuation & two-char tokens */
    if (c == '.')
    {
        lx_get(L);
        t.t = TK_DOT;
        L->cur = t;
        return;
    }
    if (c == ',')
    {
        lx_get(L);
        t.t = TK_COMMA;
        L->cur = t;
        return;
    }
    if (c == '(')
    {
        lx_get(L);
        t.t = TK_LP;
        L->cur = t;
        return;
    }
    if (c == ')')
    {
        lx_get(L);
        t.t = TK_RP;
        L->cur = t;
        return;
    }
    if (c == '[')
    {
        lx_get(L);
        t.t = TK_LB;
        L->cur = t;
        return;
    }
    if (c == ']')
    {
        lx_get(L);
        t.t = TK_RB;
        L->cur = t;
        return;
    }
    if (c == '|')
    {
        lx_get(L);
        t.t = TK_BAR;
        L->cur = t;
        return;
    }
    if (c == ':' && lx_peek2(L) == '-')
    {
        lx_get(L);
        lx_get(L);
        t.t = TK_NECK;
        L->cur = t;
        return;
    }
    if (c == '?' && lx_peek2(L) == '-')
    {
        lx_get(L);
        lx_get(L);
        t.t = TK_QUERY;
        L->cur = t;
        return;
    }

    /* quoted atom '...' */
    if (c == '\'')
    {
        lx_get(L);
        int n = 0;
        while (lx_peek(L) && lx_peek(L) != '\'')
        {
            int d = lx_get(L);
            if (d == '\\' && lx_peek(L))
            { /* minimal escapes */
                int e = lx_get(L);
                if (e == 'n')
                    d = '\n';
                else if (e == 't')
                    d = '\t';
                else
                    d = e;
            }
            if (n < 127)
                t.text[n++] = (char)d;
        }
        if (lx_peek(L) == '\'')
            lx_get(L);
        t.text[n] = '\0';
        t.t = TK_QUOTED;
        L->cur = t;
        return;
    }

    /* number (allow leading '-') */
    if (isdigit(c) || (c == '-' && isdigit(lx_peek2(L))))
    {
        char buf[128];
        int n = 0;
        int dot = 0;
        if (c == '-')
        {
            buf[n++] = (char)lx_get(L);
        }
        while (isdigit(lx_peek(L)) || lx_peek(L) == '.')
        {
            int d = lx_get(L);
            if (d == '.')
            {
                if (dot)
                    break;
                dot = 1;
            }
            if (n < 127)
                buf[n++] = (char)d;
        }
        buf[n] = '\0';
        t.t = TK_NUM;
        t.num = strtod(buf, NULL);
        L->cur = t;
        return;
    }

    /* identifier (atom or var) */
    if (isalpha(c) || c == '_')
    {
        char buf[128];
        int n = 0;
        while (isalnum(lx_peek(L)) || lx_peek(L) == '_')
        {
            int d = lx_get(L);
            if (n < 127)
                buf[n++] = (char)d;
        }
        buf[n] = '\0';
        if (isupper((unsigned char)buf[0]) || buf[0] == '_')
        {
            t.t = TK_VAR;
        }
        else
            t.t = TK_ATOM;
        strncpy(t.text, buf, 127);
        t.text[127] = '\0';
        L->cur = t;
        return;
    }

    fprintf(stderr, "Lex error %d:%d: unexpected '%c'\n", line, col, c);
    lx_get(L);
    L->cur = t; /* TK_EOF */
}

/* ============ Terms, Clauses, KB ============ */

typedef enum
{
    TM_VAR,
    TM_NUM,
    TM_STRUC
} TermKind;

typedef struct Term Term;
typedef struct ArgVec
{
    Term **a;
    int n, cap;
} ArgVec;

struct Term
{
    TermKind k;
    union
    {
        struct
        {
            int id;
            Term *ref;
            char *name;
            int anonymous;
        } v;        /* var */
        double num; /* number */
        struct
        {
            char *name;
            int arity;
            ArgVec args;
        } s; /* struct / atom (arity 0) */
    } u;
};

/* Clause: head :- body[0], body[1], ... */
typedef struct
{
    Term *head;
    Term **body;
    int body_n;
} Clause;

/* Knowledge base */
typedef struct
{
    Clause *c;
    int n, cap;
} KB;

static void args_init(ArgVec *av)
{
    av->a = NULL;
    av->n = 0;
    av->cap = 0;
}
static void args_push(ArgVec *av, Term *t)
{
    if (av->n >= av->cap)
    {
        av->cap = av->cap ? av->cap * 2 : 4;
        av->a = (Term **)realloc(av->a, (size_t)av->cap * sizeof(Term *));
    }
    av->a[av->n++] = t;
}

static Term *mk_var(const char *name)
{
    Term *t = (Term *)xmalloc(sizeof *t);
    t->k = TM_VAR;
    t->u.v.id = 0;
    t->u.v.ref = NULL;
    t->u.v.name = name ? strdup2(name) : NULL;
    t->u.v.anonymous = (name && name[0] == '_') ? 1 : 0;
    return t;
}
static Term *mk_num(double v)
{
    Term *t = (Term *)xmalloc(sizeof *t);
    t->k = TM_NUM;
    t->u.num = v;
    return t;
}
static Term *mk_atom(const char *name)
{
    Term *t = (Term *)xmalloc(sizeof *t);
    t->k = TM_STRUC;
    t->u.s.name = strdup2(name);
    t->u.s.arity = 0;
    args_init(&t->u.s.args);
    return t;
}
static Term *mk_struct(const char *name, int arity)
{
    Term *t = mk_atom(name);
    t->u.s.arity = arity;
    for (int i = 0; i < arity; i++)
        args_push(&t->u.s.args, NULL);
    return t;
}
static Term *mk_list_nil(void) { return mk_atom("[]"); }
static Term *mk_list_cons(Term *H, Term *T)
{
    Term *t = mk_struct(".", 2);
    t->u.s.args.a[0] = H;
    t->u.s.args.a[1] = T;
    return t;
}

/* ============ Parser ============ */

typedef struct
{
    Lexer L;
    int had_error;
} Parser;

static int accept(Parser *P, Tok t)
{
    if (P->L.cur.t == t)
    {
        lx_next(&P->L);
        return 1;
    }
    return 0;
}
static void expect(Parser *P, Tok t, const char *msg)
{
    if (!accept(P, t))
    {
        fprintf(stderr, "Parse error %d:%d: %s\n", P->L.cur.line, P->L.cur.col, msg);
        P->had_error = 1;
    }
}

typedef struct
{
    Term **ptrs;
    int n, cap;
} TermVec;
static void tvec_push(TermVec *v, Term *t)
{
    if (v->n >= v->cap)
    {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->ptrs = (Term **)realloc(v->ptrs, (size_t)v->cap * sizeof(Term *));
    }
    v->ptrs[v->n++] = t;
}

/* For variables inside a single clause or query, re-use the same node for repeated names */
typedef struct
{
    char **names;
    Term **vars;
    int n, cap;
} VarEnv;
static void ve_init(VarEnv *E)
{
    E->names = NULL;
    E->vars = NULL;
    E->n = 0;
    E->cap = 0;
}
static Term *ve_get(VarEnv *E, const char *name)
{
    for (int i = 0; i < E->n; i++)
        if (!strcmp(E->names[i], name))
            return E->vars[i];
    if (E->n >= E->cap)
    {
        E->cap = E->cap ? E->cap * 2 : 8;
        E->names = (char **)realloc(E->names, (size_t)E->cap * sizeof(char *));
        E->vars = (Term **)realloc(E->vars, (size_t)E->cap * sizeof(Term *));
    }
    E->names[E->n] = strdup2(name);
    E->vars[E->n] = mk_var(name);
    return E->vars[E->n++];
}

static Term *parse_term(Parser *P, VarEnv *V);

static Term *parse_list(Parser *P, VarEnv *V)
{
    if (accept(P, TK_RB))
        return mk_list_nil();
    Term *head = parse_term(P, V);
    if (accept(P, TK_BAR))
    {
        Term *tail = parse_term(P, V);
        expect(P, TK_RB, "expected ']' after list tail");
        return mk_list_cons(head, tail);
    }
    /* comma-separated */
    Term *list = mk_list_cons(head, mk_list_nil());
    Term *tail = list;
    while (accept(P, TK_COMMA))
    {
        Term *h2 = parse_term(P, V);
        Term *cell = mk_list_cons(h2, mk_list_nil());
        tail->u.s.args.a[1] = cell;
        tail = cell;
    }
    expect(P, TK_RB, "expected ']'");
    return list;
}

static Term *parse_term(Parser *P, VarEnv *V)
{
    if (P->L.cur.t == TK_VAR)
    {
        Term *r = ve_get(V, P->L.cur.text);
        lx_next(&P->L);
        return r;
    }
    if (P->L.cur.t == TK_NUM)
    {
        double v = P->L.cur.num;
        lx_next(&P->L);
        return mk_num(v);
    }
    if (P->L.cur.t == TK_QUOTED)
    {
        char nm[128];
        strncpy(nm, P->L.cur.text, 127);
        nm[127] = '\0';
        lx_next(&P->L);
        return mk_atom(nm);
    }
    if (P->L.cur.t == TK_ATOM)
    {
        char fun[128];
        strncpy(fun, P->L.cur.text, 127);
        fun[127] = '\0';
        lx_next(&P->L);
        if (accept(P, TK_LP))
        {
            /* functor with args */
            TermVec args = {0};
            if (!accept(P, TK_RP))
            {
                do
                {
                    tvec_push(&args, parse_term(P, V));
                } while (accept(P, TK_COMMA));
                expect(P, TK_RP, "expected ')'");
            }
            Term *t = mk_struct(fun, args.n);
            for (int i = 0; i < args.n; i++)
                t->u.s.args.a[i] = args.ptrs[i];
            free(args.ptrs);
            return t;
        }
        else
        {
            return mk_atom(fun);
        }
    }
    if (accept(P, TK_LB))
        return parse_list(P, V);

    fprintf(stderr, "Parse error %d:%d: expected term\n", P->L.cur.line, P->L.cur.col);
    P->had_error = 1;
    return mk_atom("error");
}

static void parse_goal_list(Parser *P, VarEnv *V, TermVec *goals)
{
    do
    {
        tvec_push(goals, parse_term(P, V));
    } while (accept(P, TK_COMMA));
}

/* parse a clause or a query; returns:
   - if is query: sets *is_query=1 and fills goals
   - else: returns Clause* and *is_query=0
*/
static Clause *parse_one(Parser *P, int *is_query, TermVec *q_goals)
{
    *is_query = 0;
    if (accept(P, TK_QUERY))
    {
        VarEnv V;
        ve_init(&V);
        TermVec goals = {0};
        parse_goal_list(P, &V, &goals);
        expect(P, TK_DOT, "expected '.' after query");
        *is_query = 1;
        *q_goals = goals; /* transfer */
        return NULL;
    }

    VarEnv V;
    ve_init(&V);
    Term *head = parse_term(P, &V);
    Clause *cl = (Clause *)xmalloc(sizeof *cl);
    cl->head = head;
    cl->body = NULL;
    cl->body_n = 0;

    if (accept(P, TK_NECK))
    {
        TermVec goals = {0};
        parse_goal_list(P, &V, &goals);
        cl->body = (Term **)xmalloc((size_t)goals.n * sizeof(Term *));
        cl->body_n = goals.n;
        for (int i = 0; i < goals.n; i++)
            cl->body[i] = goals.ptrs[i];
        free(goals.ptrs);
    }
    expect(P, TK_DOT, "expected '.' at end of clause");
    return cl;
}

/* ============ Unification & Engine ============ */

/* trail for variable bindings to allow backtracking */
typedef struct
{
    Term **v;
    int n, cap;
} Trail;
static Trail g_trail = {0};
static void trail_push(Term *var)
{
    if (g_trail.n >= g_trail.cap)
    {
        g_trail.cap = g_trail.cap ? g_trail.cap * 2 : 64;
        g_trail.v = (Term **)realloc(g_trail.v, (size_t)g_trail.cap * sizeof(Term *));
    }
    g_trail.v[g_trail.n++] = var;
}
static int trail_mark(void) { return g_trail.n; }
static void trail_unwind(int mark)
{
    while (g_trail.n > mark)
    {
        Term *v = g_trail.v[--g_trail.n];
        v->u.v.ref = NULL;
    }
}

static Term *deref(Term *t)
{
    while (t->k == TM_VAR && t->u.v.ref)
        t = t->u.v.ref;
    return t;
}

static int unify(Term *a, Term *b)
{
    a = deref(a);
    b = deref(b);
    if (a == b)
        return 1;
    if (a->k == TM_VAR)
    {
        a->u.v.ref = b;
        trail_push(a);
        return 1;
    }
    if (b->k == TM_VAR)
    {
        b->u.v.ref = a;
        trail_push(b);
        return 1;
    }
    if (a->k == TM_NUM && b->k == TM_NUM)
        return a->u.num == b->u.num;
    if (a->k == TM_STRUC && b->k == TM_STRUC)
    {
        if (strcmp(a->u.s.name, b->u.s.name) != 0)
            return 0;
        if (a->u.s.arity != b->u.s.arity)
            return 0;
        for (int i = 0; i < a->u.s.arity; i++)
        {
            if (!unify(a->u.s.args.a[i], b->u.s.args.a[i]))
                return 0;
        }
        return 1;
    }
    return 0;
}

/* deep copy a term, refreshing vars; map by pointer */
typedef struct
{
    const Term **from;
    Term **to;
    int n, cap;
} VMap;
static Term *copy_term(Term *t, VMap *M);

static Term *map_get(VMap *M, const Term *orig)
{
    for (int i = 0; i < M->n; i++)
        if (M->from[i] == orig)
            return M->to[i];
    if (M->n >= M->cap)
    {
        M->cap = M->cap ? M->cap * 2 : 16;
        M->from = (const Term **)realloc(M->from, (size_t)M->cap * sizeof(Term *));
        M->to = (Term **)realloc(M->to, (size_t)M->cap * sizeof(Term *));
    }
    Term *nv = mk_var(orig->k == TM_VAR && orig->u.v.name ? orig->u.v.name : "_");
    M->from[M->n] = orig;
    M->to[M->n] = nv;
    M->n++;
    return nv;
}

static Term *copy_term(Term *t, VMap *M)
{
    if (t->k == TM_VAR)
        return map_get(M, t);
    if (t->k == TM_NUM)
        return mk_num(t->u.num);
    /* struct */
    Term *c = mk_struct(t->u.s.name, t->u.s.arity);
    for (int i = 0; i < t->u.s.arity; i++)
        c->u.s.args.a[i] = copy_term(t->u.s.args.a[i], M);
    return c;
}

/* print a term (pretty lists) */
static void print_term(Term *t);

static void print_list(Term *t)
{
    /* assumes t is a proper list term ('.'/2 or '[]') */
    printf("[");
    int first = 1;
    while (1)
    {
        t = deref(t);
        if (t->k == TM_STRUC && t->u.s.arity == 0 && strcmp(t->u.s.name, "[]") == 0)
            break;
        if (t->k == TM_STRUC && strcmp(t->u.s.name, ".") == 0 && t->u.s.arity == 2)
        {
            if (!first)
                printf(", ");
            first = 0;
            print_term(t->u.s.args.a[0]);
            t = t->u.s.args.a[1];
        }
        else
        {
            /* improper tail */
            if (!first)
                printf(", ");
            printf("| ");
            print_term(t);
            break;
        }
    }
    printf("]");
}
static int is_list_like(Term *t)
{
    t = deref(t);
    if (t->k == TM_STRUC && t->u.s.arity == 0 && strcmp(t->u.s.name, "[]") == 0)
        return 1;
    if (t->k == TM_STRUC && strcmp(t->u.s.name, ".") == 0 && t->u.s.arity == 2)
        return 1;
    return 0;
}
static void print_term(Term *t)
{
    t = deref(t);
    if (t->k == TM_VAR)
    {
        if (t->u.v.name)
            printf("%s", t->u.v.name);
        else
            printf("_");
        return;
    }
    if (t->k == TM_NUM)
    {
        printf("%.15g", t->u.num);
        return;
    }
    /* struct/atom */
    if (is_list_like(t))
    {
        print_list(t);
        return;
    }
    printf("%s", t->u.s.name);
    if (t->u.s.arity > 0)
    {
        printf("(");
        for (int i = 0; i < t->u.s.arity; i++)
        {
            if (i)
                printf(", ");
            print_term(t->u.s.args.a[i]);
        }
        printf(")");
    }
}

/* Builtins */
static int is_atom(Term *t, const char *name, int arity)
{
    t = deref(t);
    return t->k == TM_STRUC && t->u.s.arity == arity && strcmp(t->u.s.name, name) == 0;
}

static int builtin_call(Term *goal)
{
    goal = deref(goal);
    if (is_atom(goal, "true", 0))
        return 1;
    if (is_atom(goal, "fail", 0))
        return 0;

    if (is_atom(goal, "nl", 0))
    {
        printf("\n");
        return 1;
    }

    if (is_atom(goal, "write", 1))
    {
        print_term(goal->u.s.args.a[0]);
        return 1;
    }

    if (is_atom(goal, "=", 2))
    {
        int m = trail_mark();
        if (unify(goal->u.s.args.a[0], goal->u.s.args.a[1]))
            return 1;
        trail_unwind(m);
        return 0;
    }

    if (is_atom(goal, "dif", 2))
    {
        int m = trail_mark();
        int ok = unify(goal->u.s.args.a[0], goal->u.s.args.a[1]);
        trail_unwind(m);
        return !ok;
    }

    return -1; /* not a builtin */
}

/* solver: depth-first search, enumerate all solutions */
static KB g_kb = {0};
static int g_solution_count = 0;

/* concatenate body with rest goals */
static Term **concat_goals(Term **body, int bn, Term **rest, int rn, int *outn)
{
    int n = bn + rn;
    Term **g = (Term **)xmalloc((size_t)n * sizeof(Term *));
    for (int i = 0; i < bn; i++)
        g[i] = body[i];
    for (int i = 0; i < rn; i++)
        g[bn + i] = rest[i];
    *outn = n;
    return g;
}

/* collect printable vars from original query goals */
typedef struct
{
    Term **v;
    int n, cap;
} VSet;
static void vset_add(VSet *S, Term *v)
{
    for (int i = 0; i < S->n; i++)
        if (S->v[i] == v)
            return;
    if (S->n >= S->cap)
    {
        S->cap = S->cap ? S->cap * 2 : 8;
        S->v = (Term **)realloc(S->v, (size_t)S->cap * sizeof(Term *));
    }
    S->v[S->n++] = v;
}
static void collect_vars(Term *t, VSet *S)
{
    t = deref(t);
    if (t->k == TM_VAR)
    {
        if (!t->u.v.anonymous)
            vset_add(S, t);
        return;
    }
    if (t->k == TM_STRUC)
    {
        for (int i = 0; i < t->u.s.arity; i++)
            collect_vars(t->u.s.args.a[i], S);
    }
}

static void print_solution(VSet *S)
{
    if (S->n == 0)
    {
        printf("true\n");
        return;
    }
    for (int i = 0; i < S->n; i++)
    {
        if (i)
            printf(", ");
        printf("%s = ", S->v[i]->u.v.name ? S->v[i]->u.v.name : "_");
        print_term(S->v[i]);
    }
    printf("\n");
}

/* recursive search */
static void solve(Term **goals, int gn, VSet *query_vars)
{
    if (gn == 0)
    {
        g_solution_count++;
        print_solution(query_vars);
        return; /* continue for more on backtracking */
    }
    Term *G = goals[0];

    /* check builtin first */
    int bi = builtin_call(G);
    if (bi == 1)
    {
        /* succeed: continue with rest */
        solve(goals + 1, gn - 1, query_vars);
        return;
    }
    else if (bi == 0)
    {
        /* builtin fail -> backtrack */
        return;
    }

    /* try user clauses */
    for (int i = 0; i < g_kb.n; i++)
    {
        Clause *cl = &g_kb.c[i];

        /* quick functor filter */
        Term *Gh = deref(G);
        Term *H = cl->head;
        if (Gh->k == TM_STRUC && H->k == TM_STRUC)
        {
            if (strcmp(Gh->u.s.name, H->u.s.name) != 0)
                continue;
            if (Gh->u.s.arity != H->u.s.arity)
                continue;
        }
        else if (Gh->k != H->k)
            continue;

        int mark = trail_mark();
        VMap M = {0};
        Term *Hcopy = copy_term(cl->head, &M);
        if (unify(Gh, Hcopy))
        {
            /* copy body with same var map; prepend to rest goals */
            int bn = cl->body_n;
            Term **B = NULL;
            if (bn > 0)
            {
                B = (Term **)xmalloc((size_t)bn * sizeof(Term *));
                for (int j = 0; j < bn; j++)
                    B[j] = copy_term(cl->body[j], &M);
            }
            int newn = 0;
            Term **NG = concat_goals(B, bn, goals + 1, gn - 1, &newn);
            solve(NG, newn, query_vars);
            free(B);
            free(NG);
        }
        trail_unwind(mark);
        free(M.from);
        free(M.to);
    }
}

/* ============ Driver ============ */

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
    const char *src = demo_program;
    char *heap = NULL;
    if (argc > 1)
    {
        heap = load_file(argv[1]);
        if (!heap)
        {
            fprintf(stderr, "Could not read '%s'\n", argv[1]);
            return 1;
        }
        src = heap;
    }

    Parser P;
    P.had_error = 0;
    P.L.src = src;
    P.L.len = strlen(src);
    P.L.pos = 0;
    P.L.line = 1;
    P.L.col = 1;
    lx_next(&P.L);

    /* parse loop */
    TermVec last_query = {0};
    while (P.L.cur.t != TK_EOF)
    {
        int is_q = 0;
        TermVec q_goals = {0};
        Clause *cl = parse_one(&P, &is_q, &q_goals);
        if (P.had_error)
        {
            fprintf(stderr, "Aborting due to parse errors.\n");
            free(heap);
            return 2;
        }
        if (is_q)
        {
            /* replace last query */
            free(last_query.ptrs);
            last_query = q_goals;
        }
        else
        {
            if (g_kb.n >= g_kb.cap)
            {
                g_kb.cap = g_kb.cap ? g_kb.cap * 2 : 16;
                g_kb.c = (Clause *)realloc(g_kb.c, (size_t)g_kb.cap * sizeof(Clause));
            }
            g_kb.c[g_kb.n++] = *cl;
            free(cl);
        }
    }

    if (last_query.n == 0)
    {
        /* if no query provided, use 'true.' */
        last_query.n = 1;
        last_query.ptrs = (Term **)xmalloc(sizeof(Term *));
        last_query.ptrs[0] = mk_atom("true");
    }

    /* collect query vars for printing */
    VSet qvars = {0};
    for (int i = 0; i < last_query.n; i++)
        collect_vars(last_query.ptrs[i], &qvars);

    g_solution_count = 0;
    solve(last_query.ptrs, last_query.n, &qvars);

    if (g_solution_count == 0)
        printf("false.\n");

    free(heap);
    free(g_trail.v);
    free(qvars.v);
    free(last_query.ptrs);
    free(g_kb.c); /* (not freeing deep terms for brevity) */
    return 0;
}
