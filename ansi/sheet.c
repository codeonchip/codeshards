/* sheet.c — terminal spreadsheet (C99)
 *
 * Features:
 *  - A1 references, 64 columns (A..BL), 100 rows.
 *  - Numbers + formulas (= …) with + - * / ^, parentheses.
 *  - Functions: SUM, AVG, MIN, MAX, COUNT over ranges/exprs.
 *  - Ranges: A1:B3, comma-separated arguments.
 *  - Recalc on-demand with cycle detection (#CYCLE), parse errors (#PARSE),
 *    invalid refs (#REF), divide-by-zero (#DIV0).
 *  - REPL commands: SET, SHOW, PRINT, CLEAR, SAVESS, LOADSS, SAVECSV, LOADCSV, HELP, QUIT.
 *
 * Build: gcc -std=c99 -O2 -Wall -Wextra sheet.c -o sheet -lm
 */

#if 0
ss> set A1 10
ss> set A2 20
ss> set B1 =A1*2
ss> set C1 =sum(A1:B1) + avg(A2:A2)
ss> print 5 5
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>

#define MAX_ROWS 100
#define MAX_COLS 64
#define COL_LABEL_MAX 3
#define LINE_MAXLEN 4096

/* ---------- utils ---------- */
static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i]))
        i++;
    if (i)
        memmove(s, s + i, n - i + 1);
}

/* ---------- cell / sheet ---------- */
typedef enum
{
    E_OK = 0,
    E_PARSE,
    E_REF,
    E_DIV0,
    E_CYCLE
} EvalErr;

typedef struct
{
    char *formula;  /* NULL if literal */
    double literal; /* if formula==NULL -> literal value */
    /* cache for this eval pass */
    int last_eval_id;
    double cached;
    EvalErr last_err;
    bool set; /* has any value/formula been set? */
} Cell;

typedef struct
{
    Cell cells[MAX_ROWS][MAX_COLS];
    int eval_pass_id;
} Sheet;

static void sheet_init(Sheet *sh)
{
    memset(sh, 0, sizeof *sh);
}

/* Convert column number to label (0->A, 25->Z, 26->AA, ...) */
static void col_to_label(int c, char out[COL_LABEL_MAX + 1])
{
    char buf[8];
    int i = 0;
    c += 1;
    while (c > 0 && i < 7)
    {
        int r = (c - 1) % 26;
        buf[i++] = 'A' + r;
        c = (c - 1) / 26;
    }
    // reverse
    for (int j = 0; j < i; j++)
        out[j] = buf[i - 1 - j];
    out[i] = 0;
}

/* Parse col label at s[pos], return col index and new pos; -1 if invalid */
static int parse_col(const char *s, int *pos)
{
    int i = *pos, val = 0, cnt = 0;
    while (isalpha((unsigned char)s[i]))
    {
        int ch = toupper((unsigned char)s[i]);
        if (ch < 'A' || ch > 'Z')
            break;
        val = val * 26 + (ch - 'A' + 1);
        i++;
        cnt++;
        if (cnt > COL_LABEL_MAX)
            break;
    }
    if (cnt == 0)
        return -1;
    val -= 1;
    if (val < 0 || val >= MAX_COLS)
        return -1;
    *pos = i;
    return val;
}

/* Parse number at s[pos] (strtod). */
static bool parse_number(const char *s, int *pos, double *out)
{
    char *end;
    double v = strtod(s + *pos, &end);
    if (end == s + *pos)
        return false;
    *out = v;
    *pos = (int)(end - s);
    return true;
}

/* Parse cell ref like A1 -> (r,c). Returns true on success */
static bool parse_cellref(const char *s, int *pos, int *prow, int *pcol)
{
    int i = *pos;
    int c = parse_col(s, &i);
    if (c < 0)
        return false;
    if (!isdigit((unsigned char)s[i]))
        return false;
    int row = 0, cnt = 0;
    while (isdigit((unsigned char)s[i]))
    {
        row = row * 10 + (s[i] - '0');
        i++;
        cnt++;
        if (cnt > 3)
            break;
    }
    if (row < 1 || row > MAX_ROWS)
        return false;
    *pos = i;
    *prow = row - 1;
    *pcol = c;
    return true;
}

/* ---------- expression parser ---------- */
typedef struct
{
    const char *s;
    int pos;
    Sheet *sh;
    /* for cycle detection within one top-level evaluation */
    bool (*visiting)[MAX_COLS]; /* 2D array pointer [MAX_ROWS][MAX_COLS] */
} Parser;

/* Forward decls */
static EvalErr eval_cell(Sheet *sh, int r, int c, double *out, bool visiting[MAX_ROWS][MAX_COLS]);

static void skip_ws(Parser *p)
{
    while (isspace((unsigned char)p->s[p->pos]))
        p->pos++;
}

static bool match(Parser *p, char ch)
{
    skip_ws(p);
    if (p->s[p->pos] == ch)
    {
        p->pos++;
        return true;
    }
    return false;
}
static bool match_word(Parser *p, const char *kw)
{
    skip_ws(p);
    int n = (int)strlen(kw);
    if (strncasecmp(p->s + p->pos, kw, n) == 0)
    {
        if (isalpha((unsigned char)kw[n - 1]))
        {
            char nxt = p->s[p->pos + n];
            if (isalpha((unsigned char)nxt))
                return false; /* require token boundary */
        }
        p->pos += n;
        return true;
    }
    return false;
}

/* Forward: expression grammar */
static EvalErr parse_expr(Parser *p, double *out);

/* Evaluate a range (r1:c1 to r2:c2 inclusive) applying aggregator */
typedef enum
{
    F_SUM,
    F_AVG,
    F_MIN,
    F_MAX,
    F_COUNT
} FuncId;

static EvalErr eval_range_aggregate(Sheet *sh, int r1, int c1, int r2, int c2, FuncId f,
                                    double *acc, int *count, bool visiting[MAX_ROWS][MAX_COLS])
{
    if (r2 < r1)
    {
        int t = r1;
        r1 = r2;
        r2 = t;
    }
    if (c2 < c1)
    {
        int t = c1;
        c1 = c2;
        c2 = t;
    }
    for (int r = r1; r <= r2; r++)
    {
        for (int c = c1; c <= c2; c++)
        {
            double v = 0;
            EvalErr e = eval_cell(sh, r, c, &v, visiting);
            if (e != E_OK && e != E_REF)
            { /* ignore REF for COUNT */
                return e;
            }
            if (f == F_COUNT)
            {
                if (sh->cells[r][c].set)
                    (*count)++;
            }
            else
            {
                if (*count == 0)
                {
                    *acc = v;
                }
                else
                {
                    if (f == F_SUM)
                        *acc += v;
                    else if (f == F_MIN)
                        *acc = (v < *acc) ? v : *acc;
                    else if (f == F_MAX)
                        *acc = (v > *acc) ? v : *acc;
                }
                (*count)++;
            }
        }
    }
    return E_OK;
}

/* Try parse a range A1:B3. On success sets flag and returns E_OK (no evaluation).
 * If not a range, leaves parser pos unchanged and returns E_OK with range_flag=false. */
static EvalErr try_parse_range(Parser *p, int *r1, int *c1, int *r2, int *c2, bool *is_range)
{
    int save = p->pos;
    skip_ws(p);
    int rr1, cc1;
    if (!parse_cellref(p->s, &p->pos, &rr1, &cc1))
    {
        p->pos = save;
        *is_range = false;
        return E_OK;
    }
    skip_ws(p);
    if (p->s[p->pos] != ':')
    {
        p->pos = save;
        *is_range = false;
        return E_OK;
    }
    p->pos++; /* consume ':' */
    int rr2, cc2;
    if (!parse_cellref(p->s, &p->pos, &rr2, &cc2))
    {
        p->pos = save;
        *is_range = false;
        return E_OK;
    }
    *r1 = rr1;
    *c1 = cc1;
    *r2 = rr2;
    *c2 = cc2;
    *is_range = true;
    return E_OK;
}

/* Parse a function call: NAME '(' args ')' where args are expressions or ranges.
   Returns E_OK and writes result to *out. */
static EvalErr parse_function(Parser *p, const char *name, double *out)
{
    FuncId fid;
    if (strcasecmp(name, "SUM") == 0)
        fid = F_SUM;
    else if (strcasecmp(name, "AVG") == 0 || strcasecmp(name, "AVERAGE") == 0)
        fid = F_AVG;
    else if (strcasecmp(name, "MIN") == 0)
        fid = F_MIN;
    else if (strcasecmp(name, "MAX") == 0)
        fid = F_MAX;
    else if (strcasecmp(name, "COUNT") == 0)
        fid = F_COUNT;
    else
        return E_PARSE;

    if (!match(p, '('))
        return E_PARSE;

    double acc = 0.0;
    int count = 0;
    bool first = true;
    while (1)
    {
        skip_ws(p);
        if (match(p, ')'))
            break;

        /* Try range first */
        int r1, c1, r2, c2;
        bool isrange = false;
        EvalErr er = try_parse_range(p, &r1, &c1, &r2, &c2, &isrange);
        if (er != E_OK)
            return er;
        if (isrange)
        {
            er = eval_range_aggregate(p->sh, r1, c1, r2, c2, fid, &acc, &count, p->visiting);
            if (er != E_OK)
                return er;
        }
        else
        {
            /* General expression */
            double v = 0;
            er = parse_expr(p, &v);
            if (er != E_OK)
                return er;
            if (fid == F_COUNT)
            {
                count += 1;
            }
            else
            {
                if (first)
                    acc = v;
                else
                {
                    if (fid == F_SUM)
                        acc += v;
                    else if (fid == F_MIN)
                        acc = (v < acc) ? v : acc;
                    else if (fid == F_MAX)
                        acc = (v > acc) ? v : acc;
                }
                count += 1;
            }
        }

        skip_ws(p);
        if (match(p, ','))
        {
            first = false;
            continue;
        }
        if (match(p, ')'))
            break;
        /* Unexpected token */
        if (p->s[p->pos] == 0)
            return E_PARSE;
        return E_PARSE;
    }

    if (fid == F_AVG)
    {
        if (count == 0)
            return E_DIV0;
        *out = acc / (double)count;
    }
    else if (fid == F_COUNT)
    {
        *out = (double)count;
    }
    else
    {
        *out = acc;
    }
    return E_OK;
}

/* Primary := number | cell | function | '(' expr ')' | unary ('+'|'-') factor */
static EvalErr parse_factor(Parser *p, double *out)
{
    skip_ws(p);
    /* unary */
    if (match(p, '+'))
        return parse_factor(p, out);
    if (match(p, '-'))
    {
        EvalErr e = parse_factor(p, out);
        if (e != E_OK)
            return e;
        *out = -(*out);
        return E_OK;
    }

    /* number */
    {
        int sv = p->pos;
        double v = 0;
        if (parse_number(p->s, &p->pos, &v))
        {
            *out = v;
            return E_OK;
        }
        p->pos = sv;
    }

    /* cell reference */
    {
        int sv = p->pos;
        int r, c;
        if (parse_cellref(p->s, &p->pos, &r, &c))
        {
            EvalErr e = eval_cell(p->sh, r, c, out, p->visiting);
            return e;
        }
        p->pos = sv;
    }

    /* function NAME( ... ) */
    {
        int sv = p->pos;
        /* read identifier */
        char name[16];
        int ni = 0;
        while (isalpha((unsigned char)p->s[p->pos]) && ni < 15)
        {
            name[ni++] = toupper((unsigned char)p->s[p->pos]);
            p->pos++;
        }
        name[ni] = 0;
        if (ni > 0)
        {
            skip_ws(p);
            if (p->s[p->pos] == '(')
            {
                double v = 0;
                EvalErr e = parse_function(p, name, &v);
                if (e != E_OK)
                    return e;
                *out = v;
                return E_OK;
            }
            else
            {
                p->pos = sv; /* not a function after all */
            }
        }
        else
        {
            p->pos = sv;
        }
    }

    /* (expr) */
    if (match(p, '('))
    {
        EvalErr e = parse_expr(p, out);
        if (e != E_OK)
            return e;
        if (!match(p, ')'))
            return E_PARSE;
        return E_OK;
    }

    return E_PARSE;
}

/* exponentiation (right-assoc) */
static EvalErr parse_power(Parser *p, double *out)
{
    EvalErr e = parse_factor(p, out);
    if (e != E_OK)
        return e;
    skip_ws(p);
    if (match(p, '^'))
    {
        double rhs = 0;
        e = parse_power(p, &rhs);
        if (e != E_OK)
            return e;
        *out = pow(*out, rhs);
    }
    return E_OK;
}

/* term: factor ((* or /) factor)* */
static EvalErr parse_term(Parser *p, double *out)
{
    EvalErr e = parse_power(p, out);
    if (e != E_OK)
        return e;
    while (1)
    {
        skip_ws(p);
        if (match(p, '*'))
        {
            double rhs = 0;
            e = parse_power(p, &rhs);
            if (e != E_OK)
                return e;
            *out *= rhs;
        }
        else if (match(p, '/'))
        {
            double rhs = 0;
            e = parse_power(p, &rhs);
            if (e != E_OK)
                return e;
            if (rhs == 0)
                return E_DIV0;
            *out /= rhs;
        }
        else
            break;
    }
    return E_OK;
}

/* expr: term ((+|-) term)* */
static EvalErr parse_expr(Parser *p, double *out)
{
    EvalErr e = parse_term(p, out);
    if (e != E_OK)
        return e;
    while (1)
    {
        skip_ws(p);
        if (match(p, '+'))
        {
            double rhs = 0;
            e = parse_term(p, &rhs);
            if (e != E_OK)
                return e;
            *out += rhs;
        }
        else if (match(p, '-'))
        {
            double rhs = 0;
            e = parse_term(p, &rhs);
            if (e != E_OK)
                return e;
            *out -= rhs;
        }
        else
            break;
    }
    return E_OK;
}

/* ---------- evaluation ---------- */
static EvalErr eval_formula(Sheet *sh, int r, int c, const char *fml,
                            double *out, bool visiting[MAX_ROWS][MAX_COLS])
{
    Parser p = {.s = fml, .pos = 0, .sh = sh, .visiting = visiting};
    EvalErr e = parse_expr(&p, out);
    if (e != E_OK)
        return e;
    /* trailing junk? */
    skip_ws(&p);
    if (p.s[p.pos])
        return E_PARSE;
    return E_OK;
}

static EvalErr eval_cell(Sheet *sh, int r, int c, double *out, bool visiting[MAX_ROWS][MAX_COLS])
{
    if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS)
        return E_REF;

    Cell *cell = &sh->cells[r][c];

    if (cell->last_eval_id == sh->eval_pass_id)
    {
        *out = cell->cached;
        return cell->last_err;
    }

    if (!cell->set)
    {
        cell->last_eval_id = sh->eval_pass_id;
        cell->cached = 0.0;
        cell->last_err = E_OK;
        *out = 0.0;
        return E_OK;
    }

    if (visiting[r][c])
    {
        cell->last_eval_id = sh->eval_pass_id;
        cell->last_err = E_CYCLE;
        cell->cached = NAN;
        *out = NAN;
        return E_CYCLE;
    }
    visiting[r][c] = true;

    EvalErr err = E_OK;
    double val = 0.0;
    if (cell->formula == NULL)
    {
        val = cell->literal;
    }
    else
    {
        err = eval_formula(sh, r, c, cell->formula, &val, visiting);
    }

    visiting[r][c] = false;
    cell->last_eval_id = sh->eval_pass_id;
    cell->cached = val;
    cell->last_err = err;
    *out = val;
    return err;
}

/* ---------- commands ---------- */
static void print_cell_value(Sheet *sh, int r, int c, char *buf, size_t bufsz)
{
    double v = 0;
    bool visiting[MAX_ROWS][MAX_COLS] = {0};
    sh->eval_pass_id++;
    EvalErr e = eval_cell(sh, r, c, &v, visiting);
    if (e == E_OK)
    {
        if (isnan(v) || isinf(v))
            snprintf(buf, bufsz, "#NUM");
        else
            snprintf(buf, bufsz, "%.8g", v);
    }
    else if (e == E_PARSE)
        snprintf(buf, bufsz, "#PARSE");
    else if (e == E_REF)
        snprintf(buf, bufsz, "#REF");
    else if (e == E_DIV0)
        snprintf(buf, bufsz, "#DIV0");
    else if (e == E_CYCLE)
        snprintf(buf, bufsz, "#CYCLE");
    else
        snprintf(buf, bufsz, "#ERR");
}

static void cmd_print(Sheet *sh, int rows, int cols)
{
    if (rows < 1)
        rows = 10;
    if (cols < 1)
        cols = 10;
    if (rows > MAX_ROWS)
        rows = MAX_ROWS;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    const int W = 12;

    /* header */
    printf("%*s", W, "");
    for (int c = 0; c < cols; c++)
    {
        char lbl[COL_LABEL_MAX + 1];
        col_to_label(c, lbl);
        printf("%*s", W, lbl);
    }
    printf("\n");

    for (int r = 0; r < rows; r++)
    {
        printf("%*d", W, r + 1);
        for (int c = 0; c < cols; c++)
        {
            char out[64];
            print_cell_value(sh, r, c, out, sizeof out);
            printf("%*s", W, out);
        }
        printf("\n");
    }
}

/* Parse literal vs formula and set cell */
static void set_cell(Sheet *sh, int r, int c, const char *rhs)
{
    Cell *cell = &sh->cells[r][c];
    /* free existing */
    if (cell->formula)
    {
        free(cell->formula);
        cell->formula = NULL;
    }
    cell->literal = 0;
    cell->set = true;

    /* formula if starts with '=' (allow spaces before) */
    while (isspace((unsigned char)*rhs))
        rhs++;
    if (*rhs == '=')
    {
        rhs++;
        while (isspace((unsigned char)*rhs))
            rhs++;
        cell->formula = xstrdup(rhs);
    }
    else
    {
        char *end;
        double v = strtod(rhs, &end);
        while (isspace((unsigned char)*end))
            end++;
        if (*end != 0)
        {
            /* treat as formula anyway to allow things like "A1+2" without '=' */
            cell->formula = xstrdup(rhs);
        }
        else
        {
            cell->formula = NULL;
            cell->literal = v;
        }
    }
}

/* SHOW A1: print formula and value */
static void cmd_show(Sheet *sh, int r, int c)
{
    Cell *cell = &sh->cells[r][c];
    double v = 0;
    bool visiting[MAX_ROWS][MAX_COLS] = {0};
    sh->eval_pass_id++;
    EvalErr e = eval_cell(sh, r, c, &v, visiting);

    char lbl[COL_LABEL_MAX + 1];
    col_to_label(c, lbl);
    printf("[%s%d] ", lbl, r + 1);
    if (!cell->set)
    {
        printf("(empty)\n");
        return;
    }
    if (cell->formula)
        printf("= %s\n", cell->formula);
    else
        printf("%g\n", cell->literal);

    if (e == E_OK)
        printf(" value: %.12g\n", v);
    else if (e == E_PARSE)
        printf(" value: #PARSE\n");
    else if (e == E_REF)
        printf(" value: #REF\n");
    else if (e == E_DIV0)
        printf(" value: #DIV0\n");
    else if (e == E_CYCLE)
        printf(" value: #CYCLE\n");
}

/* CLEAR A1 or CLEAR ALL */
static void cmd_clear(Sheet *sh, const char *arg)
{
    if (strcasecmp(arg, "ALL") == 0)
    {
        for (int r = 0; r < MAX_ROWS; r++)
            for (int c = 0; c < MAX_COLS; c++)
            {
                Cell *cell = &sh->cells[r][c];
                if (cell->formula)
                    free(cell->formula);
            }
        sheet_init(sh);
        printf("Cleared all.\n");
        return;
    }
    int pos = 0, r, c;
    if (!parse_cellref(arg, &pos, &r, &c) || arg[pos] != 0)
    {
        printf("Bad cell ref.\n");
        return;
    }
    Cell *cell = &sh->cells[r][c];
    if (cell->formula)
    {
        free(cell->formula);
        cell->formula = NULL;
    }
    cell->literal = 0;
    cell->set = false;
    cell->last_eval_id = 0;
    printf("Cleared.\n");
}

/* Save with formulas (script of SET commands) */
static void cmd_savess(Sheet *sh, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        perror("open");
        return;
    }
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++)
        {
            Cell *cell = &sh->cells[r][c];
            if (!cell->set)
                continue;
            char lbl[COL_LABEL_MAX + 1];
            col_to_label(c, lbl);
            if (cell->formula)
                fprintf(f, "SET %s%d =%s\n", lbl, r + 1, cell->formula);
            else
                fprintf(f, "SET %s%d %.*g\n", lbl, r + 1, 15, cell->literal);
        }
    fclose(f);
    printf("Saved to %s\n", path);
}

/* Load script with SET lines (and other commands) */
static void process_line(Sheet *sh, char *line); /* fwd */
static void cmd_loadss(Sheet *sh, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        perror("open");
        return;
    }
    char buf[LINE_MAXLEN];
    int ln = 0;
    while (fgets(buf, sizeof buf, f))
    {
        ln++;
        trim(buf);
        if (buf[0] == 0 || buf[0] == '#')
            continue;
        process_line(sh, buf);
    }
    fclose(f);
    printf("Loaded %s\n", path);
}

/* CSV save/load (values only) — simple, no quoting */
static void cmd_savecsv(Sheet *sh, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        perror("open");
        return;
    }
    bool visiting[MAX_ROWS][MAX_COLS] = {0};
    sh->eval_pass_id++;

    for (int r = 0; r < MAX_ROWS; r++)
    {
        bool any = false;
        for (int c = 0; c < MAX_COLS; c++)
        {
            double v = 0;
            EvalErr e = eval_cell(sh, r, c, &v, visiting);
            if (any)
                fprintf(f, ",");
            if (e == E_OK)
                fprintf(f, "%.15g", v);
            else
                fprintf(f, "");
            any = true;
        }
        fprintf(f, "\n");
    }
    fclose(f);
    printf("Saved CSV to %s\n", path);
}

static void cmd_loadcsv(Sheet *sh, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        perror("open");
        return;
    }
    char line[LINE_MAXLEN];
    int r = 0;
    while (r < MAX_ROWS && fgets(line, sizeof line, f))
    {
        int c = 0;
        char *p = line;
        while (c < MAX_COLS)
        {
            char *q = strchr(p, ',');
            if (q)
                *q = 0;
            trim(p);
            if (*p)
            {
                set_cell(sh, r, c, p);
            }
            else
            {
                Cell *cell = &sh->cells[r][c];
                if (cell->formula)
                {
                    free(cell->formula);
                    cell->formula = NULL;
                }
                cell->literal = 0;
                cell->set = false;
            }
            c++;
            if (!q)
                break;
            p = q + 1;
        }
        r++;
    }
    fclose(f);
    printf("Loaded CSV from %s\n", path);
}

/* ---------- REPL ---------- */
static void help()
{
    puts("Commands:");
    puts("  SET A1 42                 set literal");
    puts("  SET B2 =A1*2+SUM(A1:A3)   set formula (equals sign optional)");
    puts("  SHOW A1                   show formula & value");
    puts("  PRINT [rows] [cols]       render grid (default 10x10)");
    puts("  CLEAR A1 | CLEAR ALL      clear cell or sheet");
    puts("  SAVESS file.ss / LOADSS file.ss   save/load with formulas");
    puts("  SAVECSV file.csv / LOADCSV file.csv   values only");
    puts("  HELP");
    puts("  QUIT / EXIT");
}

static void process_line(Sheet *sh, char *line)
{
    trim(line);
    if (!line[0])
        return;
    char cmd[32];
    int n = 0;
    int i = 0;
    while (line[i] && !isspace((unsigned char)line[i]) && n < 31)
        cmd[n++] = toupper((unsigned char)line[i++]);
    cmd[n] = 0;
    while (isspace((unsigned char)line[i]))
        i++;

    if (strcmp(cmd, "HELP") == 0)
    {
        help();
        return;
    }
    if (strcmp(cmd, "QUIT") == 0 || strcmp(cmd, "EXIT") == 0)
    {
        exit(0);
    }
    if (strcmp(cmd, "PRINT") == 0)
    {
        int rows = 10, cols = 10;
        if (line[i])
        {
            rows = atoi(line + i);
            while (line[i] && !isspace((unsigned char)line[i]))
                i++;
            while (isspace((unsigned char)line[i]))
                i++;
        }
        if (line[i])
        {
            cols = atoi(line + i);
        }
        cmd_print(sh, rows, cols);
        return;
    }
    if (strcmp(cmd, "SHOW") == 0)
    {
        int r, c, pos = i;
        if (!parse_cellref(line, &pos, &r, &c))
        {
            puts("SHOW: need cell (e.g., SHOW A1)");
            return;
        }
        cmd_show(sh, r, c);
        return;
    }
    if (strcmp(cmd, "SET") == 0)
    {
        int r, c;
        int pos = i;
        if (!parse_cellref(line, &pos, &r, &c))
        {
            puts("SET: need cell (e.g., SET A1 123 or SET B2 =A1+2)");
            return;
        }
        while (isspace((unsigned char)line[pos]))
            pos++;
        if (!line[pos])
        {
            puts("SET: missing value/formula");
            return;
        }
        set_cell(sh, r, c, line + pos);
        printf("OK\n");
        return;
    }
    if (strcmp(cmd, "CLEAR") == 0)
    {
        if (!line[i])
        {
            puts("CLEAR A1 | CLEAR ALL");
            return;
        }
        cmd_clear(sh, line + i);
        return;
    }
    if (strcmp(cmd, "SAVESS") == 0)
    {
        if (!line[i])
        {
            puts("SAVESS path");
            return;
        }
        cmd_savess(sh, line + i);
        return;
    }
    if (strcmp(cmd, "LOADSS") == 0)
    {
        if (!line[i])
        {
            puts("LOADSS path");
            return;
        }
        cmd_loadss(sh, line + i);
        return;
    }
    if (strcmp(cmd, "SAVECSV") == 0)
    {
        if (!line[i])
        {
            puts("SAVECSV path");
            return;
        }
        cmd_savecsv(sh, line + i);
        return;
    }
    if (strcmp(cmd, "LOADCSV") == 0)
    {
        if (!line[i])
        {
            puts("LOADCSV path");
            return;
        }
        cmd_loadcsv(sh, line + i);
        return;
    }

    puts("Unknown command. Type HELP.");
}

int main(void)
{
    Sheet sh;
    sheet_init(&sh);
    printf("MiniSheet — C99 tiny spreadsheet. Type HELP.\n");
    char line[LINE_MAXLEN];
    while (1)
    {
        printf("ss> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;
        process_line(&sh, line);
    }
    return 0;
}
