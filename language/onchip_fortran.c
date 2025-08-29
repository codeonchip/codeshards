#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ===================== Embedded demo program ===================== */
static const char *demo_program =
    "C Fortran-like demo\n"
    "INTEGER I, SUM\n"
    "SUM = 0\n"
    "DO I = 1, 10\n"
    "  SUM = SUM + I\n"
    "END DO\n"
    "PRINT *, \"sum(1..10)=\", SUM\n"
    "\n"
    "REAL X\n"
    "X = 1.0\n"
    "DO I = 1, 5, 1\n"
    "  X = X * 1.5\n"
    "END DO\n"
    "IF (SUM .EQ. 55 .AND. X .GT. 7.5) THEN\n"
    "  PRINT *, \"ok\", 111\n"
    "ELSE\n"
    "  PRINT *, \"ng\", 222\n"
    "END IF\n"
    "PRINT *, \"X=\", X\n"
    "END\n";

/* ===================== Utilities ===================== */
static char *strdup2(const char *s)
{
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (p)
  {
    memcpy(p, s, n);
  }
  return p;
}
static void upcase(char *s)
{
  for (; *s; ++s)
    *s = (char)toupper((unsigned char)*s);
}

/* ===================== Lexer ===================== */

typedef enum
{
  T_EOF = 0,
  T_EOL,
  T_IDENT,
  T_NUMBER,
  T_STRING,
  /* delimiters / ops */
  T_LPAREN,
  T_RPAREN,
  T_COMMA,
  T_COLON,
  T_EQ,
  T_PLUS,
  T_MINUS,
  T_STAR,
  T_SLASH,
  T_POW, /* ** */
  T_LT,
  T_LE,
  T_GT,
  T_GE,
  T_NE,
  T_EQEQ,
  /* keywords (case-insensitive) */
  K_INTEGER,
  K_REAL,
  K_PRINT,
  K_IF,
  K_THEN,
  K_ELSE,
  K_END,
  K_ENDIF,
  K_PROGRAM,
  K_DO,
  K_ENDDO,
  K_CONTINUE,
  /* dot-ops: .EQ. .NE. .LT. .LE. .GT. .GE. .AND. .OR. .NOT. */
  K_DEQ,
  K_DNE,
  K_DLT,
  K_DLE,
  K_DGT,
  K_DGE,
  K_DAND,
  K_DOR,
  K_DNOT
} Tok;

typedef struct
{
  Tok type;
  double num;
  char text[128]; /* IDENT or STRING (already unescaped) */
  int line, col;
} Token;

typedef struct
{
  const char *src;
  size_t pos, len;
  int line, col;
  Token cur;
} Lexer;

static void lx_init(Lexer *L, const char *s)
{
  L->src = s;
  L->pos = 0;
  L->len = strlen(s);
  L->line = 1;
  L->col = 1;
}

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

static void lx_skip_spaces(Lexer *L)
{
  for (;;)
  {
    int c = lx_peek(L);
    /* comment start? */
    if (c == '!')
    {
      while ((c = lx_peek(L)) && c != '\n')
        lx_get(L);
    }
    else if ((c == 'C' || c == 'c') && (L->col == 1))
    { /* whole-line comment */
      while ((c = lx_peek(L)) && c != '\n')
        lx_get(L);
    }
    else if (c == ' ' || c == '\t' || c == '\r')
    {
      lx_get(L);
    }
    else
      break;
  }
}

/* read string literal with " or ' */
static int lx_string(Lexer *L, Token *t)
{
  int q = lx_get(L); /* consume quote */
  int n = 0;
  while (lx_peek(L) && lx_peek(L) != q)
  {
    int c = lx_get(L);
    if (c == '\\')
    { /* simple escapes for convenience */
      int e = lx_peek(L);
      if (e == 'n')
      {
        lx_get(L);
        c = '\n';
      }
      else if (e == 't')
      {
        lx_get(L);
        c = '\t';
      }
    }
    if (n < (int)sizeof(t->text) - 1)
      t->text[n++] = (char)c;
  }
  if (lx_peek(L) == q)
    lx_get(L);
  t->text[n] = '\0';
  t->type = T_STRING;
  return 1;
}

static int is_ident_start(int c) { return isalpha(c) || c == '_'; }
static int is_ident_char(int c) { return isalnum(c) || c == '_'; }

static Tok kw_lookup(const char *u)
{
  /* expects uppercase */
  if (!strcmp(u, "INTEGER"))
    return K_INTEGER;
  if (!strcmp(u, "REAL"))
    return K_REAL;
  if (!strcmp(u, "PRINT"))
    return K_PRINT;
  if (!strcmp(u, "IF"))
    return K_IF;
  if (!strcmp(u, "THEN"))
    return K_THEN;
  if (!strcmp(u, "ELSE"))
    return K_ELSE;
  if (!strcmp(u, "END"))
    return K_END;
  if (!strcmp(u, "ENDIF"))
    return K_ENDIF;
  if (!strcmp(u, "PROGRAM"))
    return K_PROGRAM;
  if (!strcmp(u, "DO"))
    return K_DO;
  if (!strcmp(u, "ENDDO"))
    return K_ENDDO;
  if (!strcmp(u, "CONTINUE"))
    return K_CONTINUE;
  /* dot-ops */
  if (!strcmp(u, ".EQ."))
    return K_DEQ;
  if (!strcmp(u, ".NE."))
    return K_DNE;
  if (!strcmp(u, ".LT."))
    return K_DLT;
  if (!strcmp(u, ".LE."))
    return K_DLE;
  if (!strcmp(u, ".GT."))
    return K_DGT;
  if (!strcmp(u, ".GE."))
    return K_DGE;
  if (!strcmp(u, ".AND."))
    return K_DAND;
  if (!strcmp(u, ".OR."))
    return K_DOR;
  if (!strcmp(u, ".NOT."))
    return K_DNOT;
  return T_IDENT;
}

static void lx_next(Lexer *L)
{
  lx_skip_spaces(L);
  int line = L->line, col = L->col;
  Token t;
  t.line = line;
  t.col = col;
  t.text[0] = '\0';
  t.num = 0;
  t.type = T_EOF;

  int c = lx_peek(L);
  if (!c)
  {
    L->cur = t;
    return;
  }

  if (c == '\n')
  {
    lx_get(L);
    t.type = T_EOL;
    L->cur = t;
    return;
  }

  if (c == '\'' || c == '\"')
  {
    lx_string(L, &t);
    L->cur = t;
    return;
  }

  if (isdigit(c) || ((c == '.') && isdigit(lx_peek2(L))))
  {
    /* number: int or float; allow leading '.' */
    char buf[128];
    int n = 0;
    int saw_dot = 0;
    while (isdigit(lx_peek(L)) || lx_peek(L) == '.')
    {
      int d = lx_get(L);
      if (d == '.')
      {
        if (saw_dot)
          break;
        saw_dot = 1;
      }
      if (n < 127)
        buf[n++] = (char)d;
    }
    buf[n] = '\0';
    t.type = T_NUMBER;
    t.num = strtod(buf, NULL);
    L->cur = t;
    return;
  }

  if (is_ident_start(c) || c == '.')
  {
    /* read word or .WORD. */
    char buf[128];
    int n = 0;
    if (c == '.')
    { /* dot-word */
      while (lx_peek(L) && n < 127)
      {
        int d = lx_get(L);
        buf[n++] = (char)d;
        if (d == '.' && n > 1)
          break;
      }
    }
    else
    {
      while (is_ident_char(lx_peek(L)) && n < 127)
      {
        buf[n++] = (char)toupper(lx_get(L));
      }
      buf[n] = '\0';
      Tok maybe = kw_lookup(buf);
      if (maybe != T_IDENT)
      {
        t.type = maybe;
        strcpy(t.text, buf);
        L->cur = t;
        return;
      }
      /* normal identifier */
      strcpy(t.text, buf);
      t.type = T_IDENT;
      L->cur = t;
      return;
    }
    buf[n] = '\0';
    upcase(buf);
    Tok tk = kw_lookup(buf);
    if (tk != T_IDENT)
    {
      t.type = tk;
      L->cur = t;
      return;
    }
    /* fallthrough: treat as ident-like */
    strncpy(t.text, buf, sizeof(t.text) - 1);
    t.text[sizeof(t.text) - 1] = '\0';
    t.type = T_IDENT;
    L->cur = t;
    return;
  }

  /* multi-char ops */
  if (c == '*' && lx_peek2(L) == '*')
  {
    lx_get(L);
    lx_get(L);
    t.type = T_POW;
    L->cur = t;
    return;
  }
  if (c == '<' && lx_peek2(L) == '=')
  {
    lx_get(L);
    lx_get(L);
    t.type = T_LE;
    L->cur = t;
    return;
  }
  if (c == '>' && lx_peek2(L) == '=')
  {
    lx_get(L);
    lx_get(L);
    t.type = T_GE;
    L->cur = t;
    return;
  }
  if (c == '=' && lx_peek2(L) == '=')
  {
    lx_get(L);
    lx_get(L);
    t.type = T_EQEQ;
    L->cur = t;
    return;
  }
  if (c == '/' && lx_peek2(L) == '=')
  {
    lx_get(L);
    lx_get(L);
    t.type = T_NE;
    L->cur = t;
    return;
  }

  /* single-char */
  lx_get(L);
  switch (c)
  {
  case '(':
    t.type = T_LPAREN;
    break;
  case ')':
    t.type = T_RPAREN;
    break;
  case ',':
    t.type = T_COMMA;
    break;
  case ':':
    t.type = T_COLON;
    break;    
  case '=':
    t.type = T_EQ;
    break;
  case '+':
    t.type = T_PLUS;
    break;
  case '-':
    t.type = T_MINUS;
    break;
  case '*':
    t.type = T_STAR;
    break;
  case '/':
    t.type = T_SLASH;
    break;
  case '<':
    t.type = T_LT;
    break;
  case '>':
    t.type = T_GT;
    break;
  default:
    fprintf(stderr, "Lex error %d:%d: unexpected '%c'\n", line, col, c);
    t.type = T_EOF;
    break;
  }
  L->cur = t;
}

/* ===================== AST ===================== */

typedef enum
{
  EX_NUM,
  EX_VAR,
  EX_STR,
  EX_UN,
  EX_BIN
} ExprKind;

typedef struct Expr
{
  ExprKind kind;
  int op; /* for UN/BIN; custom codes for relations/logical */
  double num;
  char *s;
  char *var;
  struct Expr *a, *b;
} Expr;

typedef enum
{
  ST_EMPTY,
  ST_BLOCK,
  ST_DECL,
  ST_ASSIGN,
  ST_PRINT,
  ST_IF,
  ST_DO,
  ST_END
} StmtKind;

typedef struct Stmt Stmt;
struct Stmt
{
  StmtKind kind;
  union
  {
    struct
    {
      char **names;
      int count; /* ignore type */
    } decl;
    struct
    {
      char *name;
      Expr *value;
    } assign;
    struct
    {
      Expr **items;
      int count;
    } print;
    struct
    {
      Expr *cond;
      Stmt *thenb;
      Stmt *elseb;
    } ifs;
    struct
    {
      char *ivar;
      Expr *start;
      Expr *end;
      Expr *step;
      Stmt *body;
    } doloop;
    struct
    {
      Stmt **items;
      int count, cap;
    } block;
  } u;
};

/* ===================== Parser ===================== */

typedef struct
{
  Lexer L;
  int had_error;
} Parser;

static void perr(Parser *P, const char *msg)
{
  fprintf(stderr, "Parse error %d:%d: %s\n", P->L.cur.line, P->L.cur.col, msg);
  P->had_error = 1;
}
static int accept(Parser *P, Tok t)
{
  if (P->L.cur.type == t)
  {
    lx_next(&P->L);
    return 1;
  }
  return 0;
}
static void expect(Parser *P, Tok t, const char *msg)
{
  if (!accept(P, t))
    perr(P, msg);
}

static Expr *mk_num(double v)
{
  Expr *e = (Expr *)calloc(1, sizeof(*e));
  e->kind = EX_NUM;
  e->num = v;
  return e;
}
static Expr *mk_var(const char *n)
{
  Expr *e = (Expr *)calloc(1, sizeof(*e));
  e->kind = EX_VAR;
  e->var = strdup2(n);
  return e;
}
static Expr *mk_str(const char *s)
{
  Expr *e = (Expr *)calloc(1, sizeof(*e));
  e->kind = EX_STR;
  e->s = strdup2(s);
  return e;
}
static Expr *mk_un(int op, Expr *a)
{
  Expr *e = (Expr *)calloc(1, sizeof(*e));
  e->kind = EX_UN;
  e->op = op;
  e->a = a;
  return e;
}
static Expr *mk_bin(int op, Expr *a, Expr *b)
{
  Expr *e = (Expr *)calloc(1, sizeof(*e));
  e->kind = EX_BIN;
  e->op = op;
  e->a = a;
  e->b = b;
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
  s->u.block.items = (Stmt **)calloc(8, sizeof(Stmt *));
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

/* Forward decls */
static Stmt *parse_stmt(Parser *P);
static Expr *parse_expr(Parser *P);

/* expression precedence (highest first):
   7: ** (right-assoc)
   6: unary + - .NOT.
   5: * /
   4: + -
   3: relations: < <= > >= == /= .EQ. .NE. .LT. .LE. .GT. .GE.
   2: .AND.
   1: .OR.
*/

static int tok_prec(Tok t)
{
  switch (t)
  {
  case T_POW:
    return 7;
  case T_STAR:
  case T_SLASH:
    return 5;
  case T_PLUS:
  case T_MINUS:
    return 4;
  case T_LT:
  case T_LE:
  case T_GT:
  case T_GE:
  case T_NE:
  case T_EQEQ:
  case K_DEQ:
  case K_DNE:
  case K_DLT:
  case K_DLE:
  case K_DGT:
  case K_DGE:
    return 3;
  case K_DAND:
    return 2;
  case K_DOR:
    return 1;
  default:
    return -1;
  }
}
static int tok_to_op(Tok t)
{
  switch (t)
  {
  case T_PLUS:
    return '+';
  case T_MINUS:
    return '-';
  case T_STAR:
    return '*';
  case T_SLASH:
    return '/';
  case T_POW:
    return '^';
  case T_LT:
    return '<';
  case T_LE:
    return 256 + 'l';
  case T_GT:
    return '>';
  case T_GE:
    return 256 + 'g';
  case T_EQEQ:
  case K_DEQ:
    return 256 + 'e';
  case T_NE:
  case K_DNE:
    return 256 + 'n';
  case K_DLT:
    return '<';
  case K_DLE:
    return 256 + 'l';
  case K_DGT:
    return '>';
  case K_DGE:
    return 256 + 'g';
  case K_DAND:
    return 256 + '&';
  case K_DOR:
    return 256 + '|';
  default:
    return 0;
  }
}

static void eat_eols(Parser *P)
{
  while (accept(P, T_EOL))
  {
  }
}

static Expr *parse_primary(Parser *P)
{
  Token t = P->L.cur;
  if (accept(P, T_NUMBER))
    return mk_num(t.num);
  if (accept(P, T_STRING))
    return mk_str(t.text);
  if (accept(P, T_IDENT))
    return mk_var(t.text);
  if (accept(P, T_LPAREN))
  {
    Expr *e = parse_expr(P);
    expect(P, T_RPAREN, "expected ')'");
    return e;
  }
  perr(P, "expected primary");
  return mk_num(0);
}

static Expr *parse_unary(Parser *P)
{
  if (accept(P, T_PLUS))
    return mk_un('+', parse_unary(P));
  if (accept(P, T_MINUS))
    return mk_un('-', parse_unary(P));
  if (accept(P, K_DNOT))
    return mk_un('!', parse_unary(P));
  return parse_primary(P);
}

/* precedence-climbing; right-assoc for ** */
static Expr *parse_bin_rhs(Parser *P, Expr *lhs, int min_prec)
{
  for (;;)
  {
    Tok t = P->L.cur.type;
    int prec = tok_prec(t);
    if (prec < min_prec)
      return lhs;

    /* handle right-assoc for ** */
    int assoc_right = (t == T_POW);
    int op = tok_to_op(t);
    lx_next(&P->L);
    Expr *rhs = parse_unary(P);
    int next_prec = tok_prec(P->L.cur.type);
    if (next_prec > prec || (assoc_right && next_prec == prec))
    {
      rhs = parse_bin_rhs(P, rhs, prec + (assoc_right ? 0 : 1));
    }
    lhs = mk_bin(op, lhs, rhs);
  }
}
static Expr *parse_expr(Parser *P)
{
  return parse_bin_rhs(P, parse_unary(P), 0);
}

/* Parse identifier list (optionally with '::' present) */
static Stmt *parse_decl(Parser *P)
{
  /* INTEGER [::] a, b, c ...  OR  REAL [::] ... */
  if (accept(P, T_EQ))
    perr(P, "unexpected '=' after type");
  if (accept(P, T_COMMA))
    perr(P, "unexpected ',' after type");
  if (accept(P, T_COLON))
    perr(P, "unexpected ':' after type"); /* not used */

  /* optional '::' */
  if (accept(P, T_COLON))
  {
    if (!accept(P, T_COLON))
      perr(P, "single ':' not allowed");
  }

  Stmt *s = mk_stmt(ST_DECL);
  s->u.decl.count = 0;
  s->u.decl.names = NULL;

  int done = 0;
  while (!done)
  {
    if (P->L.cur.type != T_IDENT)
    {
      perr(P, "expected identifier in declaration");
      break;
    }
    char *nm = strdup2(P->L.cur.text);
    lx_next(&P->L);
    s->u.decl.names = (char **)realloc(s->u.decl.names, (size_t)(s->u.decl.count + 1) * sizeof(char *));
    s->u.decl.names[s->u.decl.count++] = nm;
    if (accept(P, T_COMMA))
      continue;
    done = 1;
  }
  return s;
}

static Stmt *parse_block(Parser *P)
{
  Stmt *blk = mk_block();
  for (;;)
  {
    eat_eols(P);
    if (P->L.cur.type == K_END || P->L.cur.type == K_ENDIF || P->L.cur.type == K_ENDDO || P->L.cur.type == T_EOF)
      break;
    if (P->L.cur.type == K_ELSE)
      break;
    Stmt *st = parse_stmt(P);
    block_add(blk, st);
  }
  return blk;
}

static Stmt *parse_print(Parser *P)
{
  /* PRINT *, expr[, expr ...] */
  Stmt *s = mk_stmt(ST_PRINT);
  s->u.print.items = NULL;
  s->u.print.count = 0;

  /* optional '*' , */
  if (accept(P, T_STAR))
  { /* PRINT *  */
    expect(P, T_COMMA, "expected ',' after PRINT *");
  }
  else if (accept(P, T_COMMA))
  {
    /* allow PRINT , expr (non-standard, but convenient) */
  }
  /* read list */
  int first = 1;
  while (first || accept(P, T_COMMA))
  {
    first = 0;
    /* stop at EOL/END/ELSE etc */
    if (P->L.cur.type == T_EOL || P->L.cur.type == K_END || P->L.cur.type == K_ELSE)
      break;
    Expr *e = parse_expr(P);
    s->u.print.items = (Expr **)realloc(s->u.print.items, (size_t)(s->u.print.count + 1) * sizeof(Expr *));
    s->u.print.items[s->u.print.count++] = e;
    if (P->L.cur.type != T_COMMA)
      break;
  }
  return s;
}

static Stmt *parse_if(Parser *P)
{
  /* IF (expr) THEN  <block> [ELSE <block>] END IF  (or ENDIF) */
  Stmt *s = mk_stmt(ST_IF);
  expect(P, T_LPAREN, "expected '(' after IF");
  s->u.ifs.cond = parse_expr(P);
  expect(P, T_RPAREN, "expected ')'");
  if (!accept(P, K_THEN))
    perr(P, "expected THEN");
  eat_eols(P);
  s->u.ifs.thenb = parse_block(P);
  if (accept(P, K_ELSE))
  {
    eat_eols(P);
    s->u.ifs.elseb = parse_block(P);
  }
  else
  {
    s->u.ifs.elseb = mk_stmt(ST_EMPTY);
  }
  if (accept(P, K_ENDIF))
  { /* ok */
  }
  else
  {
    expect(P, K_END, "expected END IF / ENDIF");
    if (!accept(P, K_IF))
      perr(P, "expected END IF");
  }
  return s;
}

static Stmt *parse_do(Parser *P)
{
  /* DO i = start, end [, step]  <block>  END DO (or ENDDO) */
  Stmt *s = mk_stmt(ST_DO);
  if (P->L.cur.type != T_IDENT)
  {
    perr(P, "expected loop variable");
    s->u.doloop.ivar = strdup2("I");
  }
  else
  {
    s->u.doloop.ivar = strdup2(P->L.cur.text);
    lx_next(&P->L);
  }
  expect(P, T_EQ, "expected '=' in DO");
  s->u.doloop.start = parse_expr(P);
  expect(P, T_COMMA, "expected ',' after start");
  s->u.doloop.end = parse_expr(P);
  if (accept(P, T_COMMA))
    s->u.doloop.step = parse_expr(P);
  else
    s->u.doloop.step = mk_num(1.0);
  eat_eols(P);
  s->u.doloop.body = parse_block(P);
  if (accept(P, K_ENDDO))
  { /* ok */
  }
  else
  {
    expect(P, K_END, "expected END DO/ENDDO");
    if (!accept(P, K_DO))
      perr(P, "expected END DO");
  }
  return s;
}

static Stmt *parse_stmt(Parser *P)
{
  eat_eols(P);
  if (accept(P, T_EOL))
    return mk_stmt(ST_EMPTY);
  if (P->L.cur.type == K_INTEGER || P->L.cur.type == K_REAL)
  {
    lx_next(&P->L);
    Stmt *s = parse_decl(P);
    eat_eols(P);
    return s;
  }
  if (accept(P, K_PRINT))
  {
    Stmt *s = parse_print(P);
    eat_eols(P);
    return s;
  }
  if (accept(P, K_IF))
  {
    Stmt *s = parse_if(P);
    eat_eols(P);
    return s;
  }
  if (accept(P, K_DO))
  {
    Stmt *s = parse_do(P);
    eat_eols(P);
    return s;
  }
  if (accept(P, K_CONTINUE))
  {
    eat_eols(P);
    return mk_stmt(ST_EMPTY);
  }
  if (accept(P, K_END))
  { /* optional PROGRAM or IF/DO handled by callers */
    return mk_stmt(ST_END);
  }

  /* assignment: IDENT '=' expr [EOL] */
  if (P->L.cur.type == T_IDENT)
  {
    Token id = P->L.cur;
    lx_next(&P->L);
    expect(P, T_EQ, "expected '=' in assignment");
    Stmt *s = mk_stmt(ST_ASSIGN);
    s->u.assign.name = strdup2(id.text);
    s->u.assign.value = parse_expr(P);
    eat_eols(P);
    return s;
  }

  perr(P, "unknown statement");
  eat_eols(P);
  return mk_stmt(ST_EMPTY);
}

static Stmt *parse_program(Parser *P)
{
  Stmt *top = mk_block();
  /* optional PROGRAM name */
  if (accept(P, K_PROGRAM))
  {
    if (P->L.cur.type == T_IDENT)
      lx_next(&P->L);
    eat_eols(P);
  }
  for (;;)
  {
    eat_eols(P);
    if (P->L.cur.type == T_EOF)
      break;
    if (P->L.cur.type == K_END)
    {
      lx_next(&P->L);
      break;
    }
    Stmt *s = parse_stmt(P);
    if (s->kind == ST_END)
      break;
    block_add(top, s);
  }
  return top;
}

/* ===================== Runtime ===================== */

typedef struct
{
  char *name;
  double val;
  int inited;
} Var;
typedef struct
{
  Var *v;
  int n, cap;
} Env;

static void env_init(Env *E)
{
  E->n = 0;
  E->cap = 16;
  E->v = (Var *)calloc(16, sizeof(Var));
}
static void env_free(Env *E)
{
  for (int i = 0; i < E->n; i++)
    free(E->v[i].name);
  free(E->v);
}
static int env_find(Env *E, const char *name)
{
  for (int i = 0; i < E->n; i++)
    if (!strcmp(E->v[i].name, name))
      return i;
  return -1;
}
static int env_add(Env *E, const char *name)
{
  if (E->n >= E->cap)
  {
    E->cap *= 2;
    E->v = (Var *)realloc(E->v, (size_t)E->cap * sizeof(Var));
  }
  int i = E->n++;
  E->v[i].name = strdup2(name);
  E->v[i].val = 0.0;
  E->v[i].inited = 0;
  return i;
}
static int ensure_var(Env *E, const char *name)
{
  int i = env_find(E, name);
  if (i < 0)
    i = env_add(E, name);
  return i;
}

static int rt_ok = 1;

static double eval_expr(Env *E, Expr *e);

static double eval_bin(int op, double a, double b)
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
      fprintf(stderr, "Runtime: division by zero\n");
      rt_ok = 0;
      return 0;
    }
    return a / b;
  case '^':
    return pow(a, b);
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
    return (a != 0.0 && b != 0.0);
  case 256 + '|':
    return (a != 0.0 || b != 0.0);
  default:
    fprintf(stderr, "Runtime: unknown binop %d\n", op);
    rt_ok = 0;
    return 0;
  }
}
static double eval_expr(Env *E, Expr *e)
{
  if (!rt_ok)
    return 0;
  switch (e->kind)
  {
  case EX_NUM:
    return e->num;
  case EX_VAR:
  {
    int i = env_find(E, e->var);
    if (i < 0)
    {
      fprintf(stderr, "Runtime: undefined var %s\n", e->var);
      rt_ok = 0;
      return 0;
    }
    if (!E->v[i].inited)
    {
      fprintf(stderr, "Runtime: uninitialized var %s\n", e->var);
      rt_ok = 0;
      return 0;
    }
    return E->v[i].val;
  }
  case EX_STR:
    fprintf(stderr, "Runtime: string used in numeric context\n");
    rt_ok = 0;
    return 0;
  case EX_UN:
  {
    double v = eval_expr(E, e->a);
    if (!rt_ok)
      return 0;
    if (e->op == '+')
      return v;
    if (e->op == '-')
      return -v;
    if (e->op == '!')
      return (v == 0.0);
    fprintf(stderr, "Runtime: unknown unary %d\n", e->op);
    rt_ok = 0;
    return 0;
  }
  case EX_BIN:
  {
    if (e->op == 256 + '&')
    {
      double a = eval_expr(E, e->a);
      if (!a)
        return 0;
      double b = eval_expr(E, e->b);
      return (a != 0.0 && b != 0.0);
    }
    if (e->op == 256 + '|')
    {
      double a = eval_expr(E, e->a);
      if (a)
        return 1;
      double b = eval_expr(E, e->b);
      return (a != 0.0 || b != 0.0);
    }
    double a = eval_expr(E, e->a), b = eval_expr(E, e->b);
    return eval_bin(e->op, a, b);
  }
  }
  return 0;
}

static void exec_stmt(Env *E, Stmt *s);

static void exec_block(Env *E, Stmt *blk)
{
  for (int i = 0; i < blk->u.block.count && rt_ok; i++)
    exec_stmt(E, blk->u.block.items[i]);
}

static void exec_stmt(Env *E, Stmt *s)
{
  if (!rt_ok)
    return;
  switch (s->kind)
  {
  case ST_EMPTY:
    return;
  case ST_BLOCK:
    exec_block(E, s);
    return;
  case ST_DECL:
  {
    for (int i = 0; i < s->u.decl.count; i++)
    {
      char *nm = s->u.decl.names[i];
      ensure_var(E, nm);
    }
    return;
  }
  case ST_ASSIGN:
  {
    int i = ensure_var(E, s->u.assign.name);
    double v = eval_expr(E, s->u.assign.value);
    if (!rt_ok)
      return;
    E->v[i].val = v;
    E->v[i].inited = 1;
    return;
  }
  case ST_PRINT:
  {
    for (int i = 0; i < s->u.print.count; i++)
    {
      Expr *it = s->u.print.items[i];
      if (i)
        printf(" ");
      if (it->kind == EX_STR)
      {
        printf("%s", it->s ? it->s : "");
      }
      else
      {
        double v = eval_expr(E, it);
        if (!rt_ok)
          return;
        /* match Fortran-ish default: compact numeric */
        printf("%g", v);
      }
    }
    printf("\n");
    return;
  }
  case ST_IF:
  {
    double c = eval_expr(E, s->u.ifs.cond);
    if (!rt_ok)
      return;
    if (c != 0.0)
      exec_stmt(E, s->u.ifs.thenb);
    else
      exec_stmt(E, s->u.ifs.elseb);
    return;
  }
  case ST_DO:
  {
    int idx = ensure_var(E, s->u.doloop.ivar);
    double start = eval_expr(E, s->u.doloop.start);
    double end = eval_expr(E, s->u.doloop.end);
    double step = eval_expr(E, s->u.doloop.step);
    if (!rt_ok)
      return;
    if (step == 0)
    {
      fprintf(stderr, "Runtime: DO step cannot be 0\n");
      rt_ok = 0;
      return;
    }
    /* Fortran semantics: inclusive end; direction by step */
    E->v[idx].val = start;
    E->v[idx].inited = 1;
    if (step > 0)
    {
      for (; rt_ok && E->v[idx].val <= end; E->v[idx].val += step)
        exec_stmt(E, s->u.doloop.body);
    }
    else
    {
      for (; rt_ok && E->v[idx].val >= end; E->v[idx].val += step)
        exec_stmt(E, s->u.doloop.body);
    }
    return;
  }
  case ST_END:
    return;
  }
}

/* ===================== Driver ===================== */

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
  lx_init(&P.L, src);
  lx_next(&P.L);
  P.had_error = 0;
  Stmt *prog = parse_program(&P);
  if (P.had_error)
  {
    fprintf(stderr, "Aborting due to parse errors.\n");
    free(heap);
    return 2;
  }

  Env env;
  env_init(&env);
  rt_ok = 1;
  exec_stmt(&env, prog);
  env_free(&env);
  free(heap);
  return rt_ok ? 0 : 3;
}
