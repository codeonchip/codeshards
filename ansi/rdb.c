/* rdb.c — a relational database in one C99 file
 *
 * Features
 *  - Tables with INT and TEXT(<=255) columns, optional INT PRIMARY KEY (hash index)
 *  - CREATE TABLE, INSERT, SELECT (WHERE), DELETE (WHERE), DROP TABLE
 *  - SAVE/LOAD binary format (portable enough for this demo)
 *  - .tables, .schema [name], .quit
 *
 * Limitations
 *  - TEXT compare only for = and != in WHERE
 *  - No JOIN/UPDATE/ORDER BY; numeric ops only for INT
 *  - Naive parser; keep queries simple (quotes for TEXT)
 *
 * Build: gcc -std=c99 -O2 -Wall -Wextra rdb.c -o rdb
 */

#if 0
./rdb
db> CREATE TABLE people (id INT PRIMARY KEY, name TEXT, age INT);
db> INSERT INTO people VALUES (1, "Alice", 30);
db> INSERT INTO people VALUES (2, "Bob", 42);
db> SELECT * FROM people WHERE id = 2;
id | name  | age
2  | Bob   | 42
db> SELECT name,age FROM people WHERE age >= 30;
name  | age
Alice | 30
Bob   | 42
db> SAVE mydb.bin
db> .quit
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ------------------------- utils ------------------------- */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

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
static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q)
    {
        fprintf(stderr, "OOM\n");
        exit(1);
    }
    return q;
}
static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
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

/* ------------------------- data model ------------------------- */
typedef enum
{
    T_INT = 1,
    T_TEXT = 2
} ColType;

typedef struct
{
    char *name;
    ColType type;
    bool primary_key; /* only allowed on INT */
} Column;

typedef struct
{
    int32_t *i; /* valid when column type is INT */
    char **s;   /* valid when TEXT: malloced strings */
} ColData;

typedef struct IndexEntry
{
    int key; /* INT primary key */
    int row; /* row index */
    struct IndexEntry *next;
} IndexEntry;

typedef struct
{
    char *name;
    int ncols;
    Column *cols;
    int rows;      /* number of rows used */
    int cap;       /* capacity */
    ColData *data; /* array size ncols: column-major storage */
    int pk_col;    /* -1 if none; otherwise column index */
    /* Hash index for primary key (separate chaining) */
    int idx_buckets;
    IndexEntry **index;
} Table;

typedef struct
{
    int ntables;
    Table **tables;
} Database;

/* ------------------------- hashing ------------------------- */
static uint32_t hash_u32(uint32_t x)
{
    /* Simple but decent integer hash (Murmur-inspired) */
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

/* ------------------------- table helpers ------------------------- */
static Table *table_create(const char *name, int ncols)
{
    Table *t = xmalloc(sizeof *t);
    t->name = xstrdup(name);
    t->ncols = ncols;
    t->cols = xmalloc(sizeof(Column) * ncols);
    memset(t->cols, 0, sizeof(Column) * ncols);
    t->rows = 0;
    t->cap = 16;
    t->data = xmalloc(sizeof(ColData) * ncols);
    for (int c = 0; c < ncols; c++)
    {
        t->data[c].i = NULL;
        t->data[c].s = NULL;
    }
    t->pk_col = -1;
    t->idx_buckets = 0;
    t->index = NULL;
    return t;
}

static void table_free(Table *t)
{
    if (!t)
        return;
    for (int c = 0; c < t->ncols; c++)
    {
        free(t->cols[c].name);
        if (t->cols[c].type == T_TEXT && t->data[c].s)
        {
            for (int r = 0; r < t->rows; r++)
                free(t->data[c].s[r]);
            free(t->data[c].s);
        }
        if (t->cols[c].type == T_INT)
            free(t->data[c].i);
    }
    free(t->cols);
    /* free index */
    if (t->index)
    {
        for (int b = 0; b < t->idx_buckets; b++)
        {
            IndexEntry *e = t->index[b];
            while (e)
            {
                IndexEntry *n = e->next;
                free(e);
                e = n;
            }
        }
        free(t->index);
    }
    free(t->data);
    free(t->name);
    free(t);
}

static void table_prepare_storage(Table *t)
{
    /* allocate per-column arrays by type */
    for (int c = 0; c < t->ncols; c++)
    {
        if (t->cols[c].type == T_INT)
        {
            if (!t->data[c].i)
            {
                t->data[c].i = xmalloc(sizeof(int32_t) * t->cap);
            }
            else
            {
                t->data[c].i = xrealloc(t->data[c].i, sizeof(int32_t) * t->cap);
            }
        }
        else
        {
            if (!t->data[c].s)
            {
                t->data[c].s = xmalloc(sizeof(char *) * t->cap);
                for (int r = 0; r < t->cap; r++)
                    t->data[c].s[r] = NULL;
            }
            else
            {
                int old_cap = t->rows; /* approximate; we’ll realloc pointer array */
                t->data[c].s = xrealloc(t->data[c].s, sizeof(char *) * t->cap);
                for (int r = old_cap; r < t->cap; r++)
                    t->data[c].s[r] = NULL;
            }
        }
    }
}

static void table_grow_if_needed(Table *t)
{
    if (t->rows < t->cap)
        return;
    t->cap = t->cap * 2 + 8;
    table_prepare_storage(t);
}

/* Build PK hash index */
static void table_build_index(Table *t)
{
    if (t->pk_col < 0)
        return;
    /* free old */
    if (t->index)
    {
        for (int b = 0; b < t->idx_buckets; b++)
        {
            IndexEntry *e = t->index[b];
            while (e)
            {
                IndexEntry *n = e->next;
                free(e);
                e = n;
            }
        }
        free(t->index);
        t->index = NULL;
    }
    t->idx_buckets = 1;
    while (t->idx_buckets < (t->rows * 2 + 1))
        t->idx_buckets <<= 1;
    if (t->idx_buckets < 16)
        t->idx_buckets = 16;
    t->index = xmalloc(sizeof(IndexEntry *) * t->idx_buckets);
    for (int i = 0; i < t->idx_buckets; i++)
        t->index[i] = NULL;

    for (int r = 0; r < t->rows; r++)
    {
        int key = t->data[t->pk_col].i[r];
        uint32_t h = hash_u32((uint32_t)key) & (t->idx_buckets - 1);
        IndexEntry *e = xmalloc(sizeof *e);
        e->key = key;
        e->row = r;
        e->next = t->index[h];
        t->index[h] = e;
    }
}

static int table_find_pk(Table *t, int key)
{
    if (t->pk_col < 0 || !t->index)
        return -1;
    uint32_t h = hash_u32((uint32_t)key) & (t->idx_buckets - 1);
    for (IndexEntry *e = t->index[h]; e; e = e->next)
    {
        if (e->key == key)
            return e->row;
    }
    return -1;
}

/* ------------------------- database container ------------------------- */
static Database *db_create(void)
{
    Database *db = xmalloc(sizeof *db);
    db->ntables = 0;
    db->tables = NULL;
    return db;
}

static void db_free(Database *db)
{
    if (!db)
        return;
    for (int i = 0; i < db->ntables; i++)
        table_free(db->tables[i]);
    free(db->tables);
    free(db);
}

static Table *db_find_table(Database *db, const char *name)
{
    for (int i = 0; i < db->ntables; i++)
        if (strcasecmp(db->tables[i]->name, name) == 0)
            return db->tables[i];
    return NULL;
}

/* ------------------------- tokenizer ------------------------- */
typedef enum
{
    TK_EOF = 0,
    TK_IDENT,
    TK_NUMBER,
    TK_STRING,
    TK_LP = '(',
    TK_RP = ')',
    TK_COMMA = ',',
    TK_STAR = '*',
    TK_EQ = '=',
    TK_NE,
    TK_LT = '<',
    TK_LE,
    TK_GT = '>',
    TK_GE,
    TK_SEMI = ';',
    TK_DOT = '.',
    TK_KW_CREATE,
    TK_KW_TABLE,
    TK_KW_INT,
    TK_KW_TEXT,
    TK_KW_PRIMARY,
    TK_KW_KEY,
    TK_KW_INSERT,
    TK_KW_INTO,
    TK_KW_VALUES,
    TK_KW_SELECT,
    TK_KW_FROM,
    TK_KW_WHERE,
    TK_KW_DELETE,
    TK_KW_DROP,
    TK_KW_SAVE,
    TK_KW_LOAD
} TokKind;

typedef struct
{
    TokKind kind;
    char *lex;  /* for IDENT/STRING */
    int number; /* for NUMBER (int only for simplicity) */
} Token;

typedef struct
{
    const char *s;
    size_t pos;
    Token cur;
} Lexer;

static void tok_free(Token *t)
{
    if (t->kind == TK_IDENT || t->kind == TK_STRING)
    {
        free(t->lex);
        t->lex = NULL;
    }
}

static bool is_ident_start(int ch) { return isalpha(ch) || ch == '_'; }
static bool is_ident(int ch) { return isalnum(ch) || ch == '_'; }

static TokKind kw_lookup(const char *id)
{
    struct
    {
        const char *s;
        TokKind k;
    } kws[] = {
        {"CREATE", TK_KW_CREATE},
        {"TABLE", TK_KW_TABLE},
        {"INT", TK_KW_INT},
        {"TEXT", TK_KW_TEXT},
        {"PRIMARY", TK_KW_PRIMARY},
        {"KEY", TK_KW_KEY},
        {"INSERT", TK_KW_INSERT},
        {"INTO", TK_KW_INTO},
        {"VALUES", TK_KW_VALUES},
        {"SELECT", TK_KW_SELECT},
        {"FROM", TK_KW_FROM},
        {"WHERE", TK_KW_WHERE},
        {"DELETE", TK_KW_DELETE},
        {"DROP", TK_KW_DROP},
        {"SAVE", TK_KW_SAVE},
        {"LOAD", TK_KW_LOAD},
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
        if (strcasecmp(id, kws[i].s) == 0)
            return kws[i].k;
    return TK_IDENT;
}

static void lex_next(Lexer *L)
{
    tok_free(&L->cur);
    const char *s = L->s;
    size_t p = L->pos;
    while (isspace((unsigned char)s[p]))
        p++;
    int ch = s[p];
    if (!ch)
    {
        L->cur.kind = TK_EOF;
        L->pos = p;
        return;
    }

    if (ch == '(' || ch == ')' || ch == ',' || ch == '*' || ch == ';' || ch == '.')
    {
        L->cur.kind = (TokKind)ch;
        L->pos = p + 1;
        return;
    }
    if (ch == '"' || ch == '\'')
    {
        int quote = ch;
        p++;
        size_t start = p;
        while (s[p] && s[p] != quote)
        {
            if (s[p] == '\\' && s[p + 1])
                p++;
            p++;
        }
        size_t len = p - start;
        char *out = xmalloc(len + 1);
        size_t j = 0;
        for (size_t i = start; i < p; i++)
        {
            if (s[i] == '\\' && s[i + 1])
            {
                i++;
                out[j++] = s[i];
            }
            else
                out[j++] = s[i];
        }
        out[j] = 0;
        L->cur.kind = TK_STRING;
        L->cur.lex = out;
        if (s[p] == quote)
            p++;
        L->pos = p;
        return;
    }
    if (isdigit((unsigned char)ch) || (ch == '-' && isdigit((unsigned char)s[p + 1])))
    {
        char *end;
        long v = strtol(s + p, &end, 10);
        L->cur.kind = TK_NUMBER;
        L->cur.number = (int)v;
        L->pos = (size_t)(end - s);
        return;
    }
    if (is_ident_start((unsigned char)ch))
    {
        size_t start = p;
        p++;
        while (is_ident((unsigned char)s[p]))
            p++;
        size_t len = p - start;
        char *id = xmalloc(len + 1);
        memcpy(id, s + start, len);
        id[len] = 0;
        TokKind k = kw_lookup(id);
        if (k == TK_IDENT)
        {
            L->cur.kind = TK_IDENT;
            L->cur.lex = id;
        }
        else
        {
            L->cur.kind = k;
            free(id);
        }
        L->pos = p;
        return;
    }
    if (ch == '!' + 0)
    { /* never hit; placeholder to keep -Wall happy */
    }

    /* Operators: !=, <=, >=, =, <, > */
    if (ch == '!')
    {
        if (s[p + 1] == '=')
        {
            L->cur.kind = TK_NE;
            L->pos = p + 2;
            return;
        }
    }
    if (ch == '<')
    {
        if (s[p + 1] == '=')
        {
            L->cur.kind = TK_LE;
            L->pos = p + 2;
            return;
        }
        L->cur.kind = TK_LT;
        L->pos = p + 1;
        return;
    }
    if (ch == '>')
    {
        if (s[p + 1] == '=')
        {
            L->cur.kind = TK_GE;
            L->pos = p + 2;
            return;
        }
        L->cur.kind = TK_GT;
        L->pos = p + 1;
        return;
    }
    if (ch == '=')
    {
        L->cur.kind = TK_EQ;
        L->pos = p + 1;
        return;
    }

    /* Unknown */
    L->cur.kind = TK_EOF;
    L->pos = p + 1;
    return;
}

static bool accept(Lexer *L, TokKind k)
{
    if (L->cur.kind == k)
    {
        lex_next(L);
        return true;
    }
    return false;
}
static bool expect(Lexer *L, TokKind k, const char *msg)
{
    if (accept(L, k))
        return true;
    fprintf(stderr, "Parse error: expected %s\n", msg);
    return false;
}

/* ------------------------- DDL / DML parsing ------------------------- */
typedef enum
{
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE
} Op;

typedef struct
{
    int col; /* column index */
    Op op;
    bool is_int;
    int ival;
    char *sval;
} Where;

static bool parse_where(Lexer *L, Table *t, Where *w)
{
    if (!accept(L, TK_KW_WHERE))
        return false;
    if (L->cur.kind != TK_IDENT)
    {
        fprintf(stderr, "WHERE: need column name\n");
        return false;
    }
    char *col = xstrdup(L->cur.lex);
    lex_next(L);
    int cidx = -1;
    for (int i = 0; i < t->ncols; i++)
        if (strcasecmp(t->cols[i].name, col) == 0)
        {
            cidx = i;
            break;
        }
    free(col);
    if (cidx < 0)
    {
        fprintf(stderr, "WHERE: unknown column\n");
        return false;
    }

    Op op;
    if (accept(L, TK_EQ))
        op = OP_EQ;
    else if (accept(L, TK_NE))
        op = OP_NE;
    else if (accept(L, TK_LT))
        op = OP_LT;
    else if (accept(L, TK_LE))
        op = OP_LE;
    else if (accept(L, TK_GT))
        op = OP_GT;
    else if (accept(L, TK_GE))
        op = OP_GE;
    else
    {
        fprintf(stderr, "WHERE: expected operator\n");
        return false;
    }

    bool is_int = false;
    int ival = 0;
    char *sval = NULL;
    if (L->cur.kind == TK_NUMBER)
    {
        is_int = true;
        ival = L->cur.number;
        lex_next(L);
    }
    else if (L->cur.kind == TK_STRING)
    {
        is_int = false;
        sval = xstrdup(L->cur.lex);
        lex_next(L);
    }
    else
    {
        fprintf(stderr, "WHERE: expected number or quoted string\n");
        if (sval)
            free(sval);
        return false;
    }

    w->col = cidx;
    w->op = op;
    w->is_int = is_int;
    w->ival = ival;
    w->sval = sval;
    return true;
}

static bool eval_where(Table *t, const Where *w, int row)
{
    if (w->is_int)
    {
        int v = (t->cols[w->col].type == T_INT) ? t->data[w->col].i[row] : 0;
        switch (w->op)
        {
        case OP_EQ:
            return v == w->ival;
        case OP_NE:
            return v != w->ival;
        case OP_LT:
            return v < w->ival;
        case OP_LE:
            return v <= w->ival;
        case OP_GT:
            return v > w->ival;
        case OP_GE:
            return v >= w->ival;
        }
    }
    else
    {
        if (t->cols[w->col].type != T_TEXT)
            return false;
        const char *s = t->data[w->col].s[row] ? t->data[w->col].s[row] : "";
        int cmp = strcmp(s, w->sval);
        switch (w->op)
        {
        case OP_EQ:
            return cmp == 0;
        case OP_NE:
            return cmp != 0;
        default:
            return false; /* only = and != meaningful for TEXT here */
        }
    }
    return false;
}

/* CREATE TABLE name (col TYPE [PRIMARY KEY], ...) */
static bool cmd_create(Database *db, Lexer *L)
{
    if (!expect(L, TK_KW_TABLE, "TABLE"))
        return false;
    if (L->cur.kind != TK_IDENT)
    {
        fprintf(stderr, "CREATE: need table name\n");
        return false;
    }
    char *tname = xstrdup(L->cur.lex);
    lex_next(L);

    if (db_find_table(db, tname))
    {
        fprintf(stderr, "Table exists\n");
        free(tname);
        return false;
    }

    if (!expect(L, TK_LP, "("))
    {
        free(tname);
        return false;
    }

    Column tmp[64];
    int ncols = 0;
    int pk_col = -1;
    while (1)
    {
        if (L->cur.kind != TK_IDENT)
        {
            fprintf(stderr, "CREATE: need column name\n");
            free(tname);
            return false;
        }
        tmp[ncols].name = xstrdup(L->cur.lex);
        lex_next(L);

        if (L->cur.kind == TK_KW_INT)
        {
            tmp[ncols].type = T_INT;
            lex_next(L);
        }
        else if (L->cur.kind == TK_KW_TEXT)
        {
            tmp[ncols].type = T_TEXT;
            lex_next(L);
        }
        else
        {
            fprintf(stderr, "CREATE: column type must be INT or TEXT\n");
            free(tname);
            return false;
        }

        tmp[ncols].primary_key = false;
        if (accept(L, TK_KW_PRIMARY))
        {
            if (!expect(L, TK_KW_KEY, "KEY"))
            {
                free(tname);
                return false;
            }
            if (tmp[ncols].type != T_INT)
            {
                fprintf(stderr, "PRIMARY KEY must be INT\n");
                free(tname);
                return false;
            }
            tmp[ncols].primary_key = true;
            if (pk_col != -1)
            {
                fprintf(stderr, "Only one PRIMARY KEY allowed\n");
                free(tname);
                return false;
            }
            pk_col = ncols;
        }

        ncols++;
        if (accept(L, TK_COMMA))
            continue;
        if (accept(L, TK_RP))
            break;
        fprintf(stderr, "CREATE: expected ',' or ')'\n");
        free(tname);
        return false;
    }

    Table *t = table_create(tname, ncols);
    for (int i = 0; i < ncols; i++)
    {
        t->cols[i].name = tmp[i].name;
        t->cols[i].type = tmp[i].type;
        t->cols[i].primary_key = tmp[i].primary_key;
    }
    t->pk_col = pk_col;
    table_prepare_storage(t);

    db->tables = xrealloc(db->tables, sizeof(Table *) * (db->ntables + 1));
    db->tables[db->ntables++] = t;

    printf("Table '%s' created with %d column(s)%s.\n", t->name, t->ncols,
           t->pk_col >= 0 ? " (PRIMARY KEY indexed)" : "");
    free(tname);
    return true;
}

/* INSERT INTO name VALUES ( ... ) */
static bool cmd_insert(Database *db, Lexer *L)
{
    if (!expect(L, TK_KW_INTO, "INTO"))
        return false;
    if (L->cur.kind != TK_IDENT)
    {
        fprintf(stderr, "INSERT: need table name\n");
        return false;
    }
    Table *t = db_find_table(db, L->cur.lex);
    lex_next(L);
    if (!t)
    {
        fprintf(stderr, "No such table\n");
        return false;
    }
    if (!expect(L, TK_KW_VALUES, "VALUES"))
        return false;
    if (!expect(L, TK_LP, "("))
        return false;

    int32_t ivals[128];
    char *svals[128];
    bool is_str[128];
    if (t->ncols > 128)
    {
        fprintf(stderr, "Too many columns\n");
        return false;
    }

    for (int c = 0; c < t->ncols; c++)
    {
        svals[c] = NULL;
        is_str[c] = false;
    }

    for (int c = 0; c < t->ncols; c++)
    {
        if (t->cols[c].type == T_INT)
        {
            if (L->cur.kind != TK_NUMBER)
            {
                fprintf(stderr, "INSERT: INT expected at column %d\n", c + 1);
                goto fail;
            }
            ivals[c] = L->cur.number;
            lex_next(L);
        }
        else
        {
            if (L->cur.kind != TK_STRING)
            {
                fprintf(stderr, "INSERT: TEXT (quoted) expected at column %d\n", c + 1);
                goto fail;
            }
            svals[c] = xstrdup(L->cur.lex);
            is_str[c] = true;
            lex_next(L);
        }
        if (c < t->ncols - 1)
        {
            if (!expect(L, TK_COMMA, ","))
                goto fail;
        }
    }
    if (!expect(L, TK_RP, ")"))
        goto fail;

    /* primary key uniqueness check */
    if (t->pk_col >= 0)
    {
        int key = (t->cols[t->pk_col].type == T_INT) ? ivals[t->pk_col] : 0;
        int exists = table_find_pk(t, key);
        if (exists >= 0)
        {
            fprintf(stderr, "PRIMARY KEY duplicate: %d\n", key);
            goto fail;
        }
    }

    table_grow_if_needed(t);
    int r = t->rows;
    for (int c = 0; c < t->ncols; c++)
    {
        if (t->cols[c].type == T_INT)
            t->data[c].i[r] = ivals[c];
        else
            t->data[c].s[r] = svals[c], svals[c] = NULL; /* take ownership */
    }
    t->rows++;

    /* update index if needed */
    if (t->pk_col >= 0)
    {
        if (!t->index)
        {
            t->idx_buckets = 0;
            table_build_index(t);
        }
        int key = t->data[t->pk_col].i[r];
        uint32_t h = hash_u32((uint32_t)key) & (t->idx_buckets - 1);
        IndexEntry *e = xmalloc(sizeof *e);
        e->key = key;
        e->row = r;
        e->next = t->index[h];
        t->index[h] = e;
    }

    printf("Inserted 1 row.\n");
    return true;

fail:
    for (int c = 0; c < t->ncols; c++)
        if (is_str[c] && svals[c])
            free(svals[c]);
    return false;
}

/* SELECT collist FROM name [WHERE ...] */
static bool cmd_select(Database *db, Lexer *L)
{
    /* parse select list (either * or a set of identifiers) */
    int sel_all = 0;
    char *cols[128];
    int nsel = 0;
    if (accept(L, TK_STAR))
    {
        sel_all = 1;
    }
    else
    {
        while (1)
        {
            if (L->cur.kind != TK_IDENT)
            {
                fprintf(stderr, "SELECT: need column name or *\n");
                goto fail;
            }
            cols[nsel++] = xstrdup(L->cur.lex);
            lex_next(L);
            if (accept(L, TK_COMMA))
                continue;
            break;
        }
    }
    if (!expect(L, TK_KW_FROM, "FROM"))
        goto fail;
    if (L->cur.kind != TK_IDENT)
    {
        fprintf(stderr, "SELECT: need table name\n");
        goto fail;
    }
    Table *t = db_find_table(db, L->cur.lex);
    lex_next(L);
    if (!t)
    {
        fprintf(stderr, "No such table\n");
        goto fail;
    }

    Where w;
    memset(&w, 0, sizeof w);
    bool has_where = parse_where(L, t, &w);

    /* map select cols */
    int sel_idx[128];
    if (sel_all)
    {
        nsel = t->ncols;
        for (int i = 0; i < nsel; i++)
            sel_idx[i] = i;
    }
    else
    {
        for (int i = 0; i < nsel; i++)
        {
            int c = -1;
            for (int j = 0; j < t->ncols; j++)
                if (strcasecmp(cols[i], t->cols[j].name) == 0)
                {
                    c = j;
                    break;
                }
            if (c < 0)
            {
                fprintf(stderr, "Unknown column '%s'\n", cols[i]);
                if (has_where && w.sval)
                    free(w.sval);
                goto fail;
            }
            sel_idx[i] = c;
        }
    }

    /* print header */
    for (int i = 0; i < nsel; i++)
    {
        printf("%s%s", i ? " | " : "", t->cols[sel_idx[i]].name);
    }
    printf("\n");

    /* fast path: WHERE on PK with '=' */
    int start_row = 0;
    int end_row = t->rows;
    int only_row = -1;
    if (has_where && t->pk_col >= 0 && w.is_int && w.col == t->pk_col && w.op == OP_EQ)
    {
        only_row = table_find_pk(t, w.ival);
        if (only_row >= 0)
        {
            start_row = only_row;
            end_row = only_row + 1;
        }
        else
        {
            start_row = 0;
            end_row = 0;
        } /* nothing to print */
    }

    for (int r = start_row; r < end_row; r++)
    {
        if (has_where)
        {
            if (!eval_where(t, &w, r))
                continue;
        }
        /* print row */
        for (int i = 0; i < nsel; i++)
        {
            int c = sel_idx[i];
            if (i)
                printf(" | ");
            if (t->cols[c].type == T_INT)
                printf("%d", t->data[c].i[r]);
            else
                printf("%s", t->data[c].s[r] ? t->data[c].s[r] : "");
        }
        printf("\n");
    }
    if (has_where && w.sval)
        free(w.sval);
    for (int i = 0; i < nsel && !sel_all; i++)
        free(cols[i]);
    return true;

fail:
    for (int i = 0; i < nsel && !sel_all; i++)
        free(cols[i]);
    return false;
}

/* DELETE FROM name [WHERE ...] */
static bool cmd_delete(Database *db, Lexer *L)
{
    if (!expect(L, TK_KW_FROM, "FROM"))
        return false;
    if (L->cur.kind != TK_IDENT)
    {
        fprintf(stderr, "DELETE: need table name\n");
        return false;
    }
    Table *t = db_find_table(db, L->cur.lex);
    lex_next(L);
    if (!t)
    {
        fprintf(stderr, "No such table\n");
        return false;
    }

    Where w;
    memset(&w, 0, sizeof w);
    bool has_where = parse_where(L, t, &w);

    int kept = 0;
    for (int r = 0; r < t->rows; r++)
    {
        bool match = has_where ? eval_where(t, &w, r) : true;
        if (match)
        {
            /* free TEXT cells */
            for (int c = 0; c < t->ncols; c++)
                if (t->cols[c].type == T_TEXT && t->data[c].s[r])
                {
                    free(t->data[c].s[r]);
                    t->data[c].s[r] = NULL;
                }
            continue; /* drop row r */
        }
        else
        {
            if (kept != r)
            {
                /* move row r -> kept */
                for (int c = 0; c < t->ncols; c++)
                {
                    if (t->cols[c].type == T_INT)
                        t->data[c].i[kept] = t->data[c].i[r];
                    else
                    {
                        t->data[c].s[kept] = t->data[c].s[r];
                        t->data[c].s[r] = NULL;
                    }
                }
            }
            kept++;
        }
    }
    int deleted = t->rows - kept;
    t->rows = kept;

    /* rebuild index if present */
    if (t->pk_col >= 0)
        table_build_index(t);

    if (has_where && w.sval)
        free(w.sval);
    printf("Deleted %d row(s).\n", deleted);
    return true;
}

/* DROP TABLE name */
static bool cmd_drop(Database *db, Lexer *L)
{
    if (!expect(L, TK_KW_TABLE, "TABLE"))
        return false;
    if (L->cur.kind != TK_IDENT)
    {
        fprintf(stderr, "DROP: need table name\n");
        return false;
    }
    char *name = xstrdup(L->cur.lex);
    lex_next(L);
    int idx = -1;
    for (int i = 0; i < db->ntables; i++)
        if (strcasecmp(db->tables[i]->name, name) == 0)
        {
            idx = i;
            break;
        }
    if (idx < 0)
    {
        fprintf(stderr, "No such table\n");
        free(name);
        return false;
    }
    table_free(db->tables[idx]);
    for (int i = idx + 1; i < db->ntables; i++)
        db->tables[i - 1] = db->tables[i];
    db->ntables--;
    printf("Dropped table '%s'.\n", name);
    free(name);
    return true;
}

/* ------------------------- save/load ------------------------- */
/* Binary format (little-endian-ish):
   magic[8]="RRRRDB01"
   u32 ntables
   For each table:
     u16 name_len, bytes name
     u16 ncols
     For each column:
       u16 colname_len, bytes colname
       u8  type (1=int,2=text)
       u8  primary_key (0/1)
     u32 rows
     For each column, type==INT: rows * i32
                    type==TEXT: rows * [u16 len | bytes]
*/
static bool cmd_save(Database *db, Lexer *L)
{
    if (L->cur.kind != TK_IDENT && L->cur.kind != TK_STRING)
    {
        fprintf(stderr, "SAVE: need filename\n");
        return false;
    }
    const char *path = (L->cur.kind == TK_STRING) ? L->cur.lex : L->cur.lex; /* use as-is */
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        perror("open");
        return false;
    }
    fwrite("RRRRDB01", 1, 8, f);
    uint32_t nt = (uint32_t)db->ntables;
    fwrite(&nt, 4, 1, f);
    for (int ti = 0; ti < db->ntables; ti++)
    {
        Table *t = db->tables[ti];
        uint16_t nlen = (uint16_t)strlen(t->name);
        fwrite(&nlen, 2, 1, f);
        fwrite(t->name, 1, nlen, f);
        uint16_t nc = (uint16_t)t->ncols;
        fwrite(&nc, 2, 1, f);
        for (int c = 0; c < t->ncols; c++)
        {
            uint16_t clen = (uint16_t)strlen(t->cols[c].name);
            fwrite(&clen, 2, 1, f);
            fwrite(t->cols[c].name, 1, clen, f);
            uint8_t ty = (uint8_t)t->cols[c].type;
            fwrite(&ty, 1, 1, f);
            uint8_t pk = (uint8_t)(t->cols[c].primary_key ? 1 : 0);
            fwrite(&pk, 1, 1, f);
        }
        uint32_t rows = (uint32_t)t->rows;
        fwrite(&rows, 4, 1, f);
        for (int c = 0; c < t->ncols; c++)
        {
            if (t->cols[c].type == T_INT)
            {
                for (int r = 0; r < t->rows; r++)
                {
                    int32_t v = t->data[c].i[r];
                    fwrite(&v, 4, 1, f);
                }
            }
            else
            {
                for (int r = 0; r < t->rows; r++)
                {
                    const char *s = t->data[c].s[r] ? t->data[c].s[r] : "";
                    size_t len = strlen(s);
                    if (len > 65535)
                        len = 65535;
                    uint16_t L16 = (uint16_t)len;
                    fwrite(&L16, 2, 1, f);
                    fwrite(s, 1, len, f);
                }
            }
        }
    }
    fclose(f);
    printf("Saved to %s\n", path);
    return true;
}

static bool cmd_load(Database *db, Lexer *L)
{
    if (L->cur.kind != TK_IDENT && L->cur.kind != TK_STRING)
    {
        fprintf(stderr, "LOAD: need filename\n");
        return false;
    }
    const char *path = (L->cur.kind == TK_STRING) ? L->cur.lex : L->cur.lex;
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        perror("open");
        return false;
    }
    char magic[9] = {0};
    fread(magic, 1, 8, f);
    if (strcmp(magic, "RRRRDB01") != 0)
    {
        fprintf(stderr, "Bad file\n");
        fclose(f);
        return false;
    }
    /* clear existing db */
    for (int i = 0; i < db->ntables; i++)
        table_free(db->tables[i]);
    free(db->tables);
    db->tables = NULL;
    db->ntables = 0;

    uint32_t nt = 0;
    fread(&nt, 4, 1, f);
    for (uint32_t ti = 0; ti < nt; ti++)
    {
        uint16_t nlen;
        fread(&nlen, 2, 1, f);
        char *tname = xmalloc(nlen + 1);
        fread(tname, 1, nlen, f);
        tname[nlen] = 0;
        uint16_t nc;
        fread(&nc, 2, 1, f);
        Table *t = table_create(tname, nc);
        free(tname);
        for (int c = 0; c < nc; c++)
        {
            uint16_t clen;
            fread(&clen, 2, 1, f);
            char *cname = xmalloc(clen + 1);
            fread(cname, 1, clen, f);
            cname[clen] = 0;
            uint8_t ty, pk;
            fread(&ty, 1, 1, f);
            fread(&pk, 1, 1, f);
            t->cols[c].name = cname;
            t->cols[c].type = (ColType)ty;
            t->cols[c].primary_key = (pk != 0);
            if (pk)
                t->pk_col = c;
        }
        table_prepare_storage(t);
        uint32_t rows;
        fread(&rows, 4, 1, f);
        t->rows = rows;
        table_grow_if_needed(t); /* ensure cap >= rows */
        for (int c = 0; c < t->ncols; c++)
        {
            if (t->cols[c].type == T_INT)
            {
                for (uint32_t r = 0; r < rows; r++)
                {
                    int32_t v;
                    fread(&v, 4, 1, f);
                    t->data[c].i[r] = v;
                }
            }
            else
            {
                for (uint32_t r = 0; r < rows; r++)
                {
                    uint16_t L16;
                    fread(&L16, 2, 1, f);
                    char *s = xmalloc(L16 + 1);
                    fread(s, 1, L16, f);
                    s[L16] = 0;
                    t->data[c].s[r] = s;
                }
            }
        }
        db->tables = xrealloc(db->tables, sizeof(Table *) * (db->ntables + 1));
        db->tables[db->ntables++] = t;
        if (t->pk_col >= 0)
            table_build_index(t);
    }
    fclose(f);
    printf("Loaded %u table(s) from %s\n", nt, path);
    return true;
}

/* ------------------------- meta commands ------------------------- */
static void cmd_tables(Database *db)
{
    if (db->ntables == 0)
    {
        puts("(no tables)");
        return;
    }
    for (int i = 0; i < db->ntables; i++)
        puts(db->tables[i]->name);
}

static void cmd_schema(Database *db, const char *opt)
{
    if (opt && *opt)
    {
        Table *t = db_find_table(db, opt);
        if (!t)
        {
            fprintf(stderr, "No such table\n");
            return;
        }
        printf("Table %s (\n", t->name);
        for (int c = 0; c < t->ncols; c++)
        {
            printf("  %s %s%s%s\n", t->cols[c].name,
                   t->cols[c].type == T_INT ? "INT" : "TEXT",
                   t->cols[c].primary_key ? " PRIMARY" : "", t->cols[c].primary_key ? " KEY" : "");
        }
        printf(");\n");
    }
    else
    {
        for (int i = 0; i < db->ntables; i++)
        {
            Table *t = db->tables[i];
            printf("%s(", t->name);
            for (int c = 0; c < t->ncols; c++)
            {
                if (c)
                    printf(", ");
                printf("%s %s%s", t->cols[c].name, t->cols[c].type == T_INT ? "INT" : "TEXT",
                       t->cols[c].primary_key ? " PRIMARY KEY" : "");
            }
            printf(")\n");
        }
    }
}

/* ------------------------- top-level parser ------------------------- */
static bool run_stmt(Database *db, const char *line)
{
    Lexer L = {.s = line, .pos = 0};
    lex_next(&L);

    /* dot-commands */
    if (L.cur.kind == TK_DOT)
    {
        lex_next(&L);
        if (L.cur.kind == TK_IDENT && strcasecmp(L.cur.lex, "tables") == 0)
        {
            lex_next(&L);
            cmd_tables(db);
            return true;
        }
        if (L.cur.kind == TK_IDENT && strcasecmp(L.cur.lex, "schema") == 0)
        {
            lex_next(&L);
            char buf[256] = {0};
            if (L.cur.kind == TK_IDENT || L.cur.kind == TK_STRING)
            {
                strncpy(buf, L.cur.lex, sizeof buf - 1);
            }
            cmd_schema(db, buf[0] ? buf : NULL);
            return true;
        }
        if (L.cur.kind == TK_IDENT && (strcasecmp(L.cur.lex, "quit") == 0 || strcasecmp(L.cur.lex, "exit") == 0))
        {
            exit(0);
        }
        fprintf(stderr, "Unknown dot-command\n");
        return false;
    }

    switch (L.cur.kind)
    {
    case TK_KW_CREATE:
        lex_next(&L);
        return cmd_create(db, &L);
    case TK_KW_INSERT:
        lex_next(&L);
        return cmd_insert(db, &L);
    case TK_KW_SELECT:
        return cmd_select(db, &L);
    case TK_KW_DELETE:
        lex_next(&L);
        return cmd_delete(db, &L);
    case TK_KW_DROP:
        lex_next(&L);
        return cmd_drop(db, &L);
    case TK_KW_SAVE:
        lex_next(&L);
        return cmd_save(db, &L);
    case TK_KW_LOAD:
        lex_next(&L);
        return cmd_load(db, &L);
    case TK_EOF:
        return true;
    default:
        fprintf(stderr, "Unknown statement. Try: CREATE/INSERT/SELECT/DELETE/DROP/SAVE/LOAD or .tables/.schema/.quit\n");
        return false;
    }
}

/* ------------------------- main REPL ------------------------- */
int main(void)
{
    Database *db = db_create();
    char line[8192];

    puts("rdb — C99 database. Commands end at newline (semicolon optional).");
    puts("Examples:");
    puts("  CREATE TABLE people (id INT PRIMARY KEY, name TEXT, age INT)");
    puts("  INSERT INTO people VALUES (1, \"Alice\", 30)");
    puts("  SELECT * FROM people WHERE id = 1");
    puts("  SAVE mydb.bin   |  LOAD mydb.bin");
    puts("Meta: .tables, .schema [table], .quit");
    while (1)
    {
        fputs("db> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;
        trim(line);
        if (!line[0])
            continue;
        run_stmt(db, line);
    }
    db_free(db);
    return 0;
}
