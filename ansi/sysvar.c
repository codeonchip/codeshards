/*
 * sysvar.c — a "sysvar"-style runtime configuration store
 * for Linux, inspired by Interpeak IPnet sysvar.
 *
 * Features:
 *  - Hierarchical string keys like "net.ipv4.tcp_syn_retries" (dot-separated)
 *  - In‑memory hash map with pthread RW lock
 *  - Flags (read‑only) and overwrite control
 *  - Simple persistent store: tab‑separated file (key\tvalue\tflags) with
 *    C‑style escapes for tabs/newlines/backslashes
 *  - CLI: list/get/set/unset with optional -f <file>
 *
 * Build:  gcc -O2 -Wall -pthread sysvar.c -o sysvar
 * Usage:
 *   ./sysvar -f ~/.config/sysvar.db set key value
 *   ./sysvar get key
 *   ./sysvar list [prefix]
 *   ./sysvar unset key
 *
 * Default DB path order: $SYSVAR_DB, then $HOME/.config/sysvar.db
 */

#if 0
#default DB : $SYSVAR_DB or ~/.config / sysvar.db
./sysvar set -r net.ipv4.base_hop_limit 64     # create read-only
./sysvar get net.ipv4.base_hop_limit           # -> 64
./sysvar set -o net.ipv4.base_hop_limit 128    # overwrite (needs -o)
./sysvar list net.ipv4                         # list subtree
./sysvar unset some.temp.key                   # remove (fails if #ro)
#endif

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

/* ---------- Flags ---------- */
#define SV_FLAG_RDONLY (1u << 0)

/* ---------- Hash map ---------- */
#ifndef SV_BUCKETS
#define SV_BUCKETS 1024
#endif

typedef struct SVVar
{
    char *key;
    char *val;
    unsigned flags;
    struct SVVar *next;
} SVVar;

typedef struct
{
    SVVar *buckets[SV_BUCKETS];
    pthread_rwlock_t lock;
    char *db_path; /* nullable until set */
    size_t count;
} SVStore;

static SVStore g_store = {.buckets = {NULL}, .lock = PTHREAD_RWLOCK_INITIALIZER, .db_path = NULL, .count = 0};

/* ---------- Utilities ---------- */
static unsigned long djb2(const char *s)
{
    unsigned long h = 5381;
    unsigned char c;
    while ((c = (unsigned char)*s++))
        h = ((h << 5) + h) + c; /* h*33 + c */
    return h;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p)
    {
        perror("malloc");
        exit(1);
    }
    return p;
}

static char *xstrdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *d = xmalloc(n);
    memcpy(d, s, n);
    return d;
}

static char *sv_default_path(void)
{
    const char *env = getenv("SYSVAR_DB");
    if (env && *env)
        return xstrdup(env);
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "."; /* fallback */
    size_t need = strlen(home) + strlen("/.config/sysvar.db") + 1;
    char *buf = xmalloc(need);
    snprintf(buf, need, "%s/.config/sysvar.db", home);
    return buf;
}

static int ensure_parent_dir(const char *path)
{
    char tmp[PATH_MAX];
    size_t n = strnlen(path, sizeof(tmp));
    if (n >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, n + 1);
    char *slash = strrchr(tmp, '/');
    if (!slash)
        return 0; /* current dir */
    *slash = '\0';
    struct stat st;
    if (stat(tmp, &st) == 0)
    {
        if (S_ISDIR(st.st_mode))
            return 0;
        errno = ENOTDIR;
        return -1;
    }
    /* recursively create */
    if (ensure_parent_dir(tmp) != 0)
        return -1;
    if (mkdir(tmp, 0700) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

/* Escape value for file: \t, \n, \r, \\ */
static char *escape_value(const char *v)
{
    size_t extra = 0;
    for (const unsigned char *p = (const unsigned char *)v; *p; ++p)
    {
        if (*p == '\t' || *p == '\n' || *p == '\r' || *p == '\\')
            extra++;
    }
    size_t n = strlen(v);
    char *out = xmalloc(n + extra + 1);
    char *q = out;
    for (const unsigned char *p = (const unsigned char *)v; *p; ++p)
    {
        switch (*p)
        {
        case '\t':
            *q++ = '\\';
            *q++ = 't';
            break;
        case '\n':
            *q++ = '\\';
            *q++ = 'n';
            break;
        case '\r':
            *q++ = '\\';
            *q++ = 'r';
            break;
        case '\\':
            *q++ = '\\';
            *q++ = '\\';
            break;
        default:
            *q++ = (char)*p;
            break;
        }
    }
    *q = '\0';
    return out;
}

static char *unescape_value(const char *v)
{
    size_t n = strlen(v);
    char *out = xmalloc(n + 1);
    char *q = out;
    const char *p = v;
    while (*p)
    {
        if (*p == '\\' && p[1])
        {
            p++;
            switch (*p)
            {
            case 't':
                *q++ = '\t';
                break;
            case 'n':
                *q++ = '\n';
                break;
            case 'r':
                *q++ = '\r';
                break;
            case '\\':
                *q++ = '\\';
                break;
            default:
                *q++ = *p;
                break; /* unknown escape: pass through */
            }
            p++;
        }
        else
        {
            *q++ = *p++;
        }
    }
    *q = '\0';
    return out;
}

/* ---------- Core store ops (internal; lock must be held) ---------- */
static SVVar *sv_find_locked(const char *key, unsigned long *out_idx)
{
    unsigned long idx = djb2(key) % SV_BUCKETS;
    if (out_idx)
        *out_idx = idx;
    SVVar *v = g_store.buckets[idx];
    while (v)
    {
        if (strcmp(v->key, key) == 0)
            return v;
        v = v->next;
    }
    return NULL;
}

static int sv_set_locked(const char *key, const char *val, unsigned flags, bool overwrite)
{
    unsigned long idx;
    SVVar *v = sv_find_locked(key, &idx);
    if (v)
    {
        if ((v->flags & SV_FLAG_RDONLY) && !overwrite)
            return -2; /* read-only */
        if (!overwrite)
            return -1; /* exists and overwrite not allowed */
        /* Overwrite allowed */
        free(v->val);
        v->val = xstrdup(val);
        v->flags = (v->flags & SV_FLAG_RDONLY) | (flags & SV_FLAG_RDONLY); /* preserve RO if already RO unless explicitly set again */
        return 0;
    }
    v = xmalloc(sizeof(*v));
    v->key = xstrdup(key);
    v->val = xstrdup(val);
    v->flags = flags;
    v->next = g_store.buckets[idx];
    g_store.buckets[idx] = v;
    g_store.count++;
    return 0;
}

static int sv_unset_locked(const char *key)
{
    unsigned long idx = djb2(key) % SV_BUCKETS;
    SVVar *prev = NULL, *cur = g_store.buckets[idx];
    while (cur)
    {
        if (strcmp(cur->key, key) == 0)
        {
            if (cur->flags & SV_FLAG_RDONLY)
                return -2;
            if (prev)
                prev->next = cur->next;
            else
                g_store.buckets[idx] = cur->next;
            free(cur->key);
            free(cur->val);
            free(cur);
            g_store.count--;
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }
    return -1; /* not found */
}

/* ---------- Public API ---------- */
int sv_init(const char *db_path);
int sv_load(const char *db_path);
int sv_save(const char *db_path);
int sv_set(const char *key, const char *val, unsigned flags, bool overwrite);
int sv_get(const char *key, char **out_val, unsigned *out_flags);
int sv_unset(const char *key);
size_t sv_list(const char *prefix, SVVar ***out_vec);

int sv_init(const char *db_path)
{
    pthread_rwlock_wrlock(&g_store.lock);
    if (g_store.db_path)
    {
        free(g_store.db_path);
        g_store.db_path = NULL;
    }
    g_store.db_path = xstrdup(db_path ? db_path : sv_default_path());
    pthread_rwlock_unlock(&g_store.lock);
    return sv_load(NULL); /* load from g_store.db_path */
}

int sv_load(const char *db_path)
{
    int rc = 0;
    pthread_rwlock_wrlock(&g_store.lock);
    const char *path = db_path ? db_path : g_store.db_path;
    if (!path)
    {
        pthread_rwlock_unlock(&g_store.lock);
        return 0;
    }
    FILE *fp = fopen(path, "r");
    if (!fp)
    { /* not fatal if missing */
        rc = (errno == ENOENT) ? 0 : -1;
        pthread_rwlock_unlock(&g_store.lock);
        return rc;
    }

    /* Clear existing */
    for (size_t i = 0; i < SV_BUCKETS; i++)
    {
        SVVar *v = g_store.buckets[i];
        while (v)
        {
            SVVar *n = v->next;
            free(v->key);
            free(v->val);
            free(v);
            v = n;
        }
        g_store.buckets[i] = NULL;
    }
    g_store.count = 0;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, fp)) != -1)
    {
        if (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#')
            continue;
        /* Expect: key\tvalue\tflags */
        char *k = line;
        char *tab1 = strchr(k, '\t');
        if (!tab1)
            continue;
        *tab1 = '\0';
        char *v = tab1 + 1;
        char *tab2 = strchr(v, '\t');
        char *flags_str = NULL;
        if (tab2)
        {
            *tab2 = '\0';
            flags_str = tab2 + 1;
        }
        char *val_unesc = unescape_value(v);
        unsigned flags = 0;
        if (flags_str && strstr(flags_str, "ro"))
            flags |= SV_FLAG_RDONLY;
        sv_set_locked(k, val_unesc, flags, true);
        free(val_unesc);
    }
    free(line);
    fclose(fp);
    pthread_rwlock_unlock(&g_store.lock);
    return rc;
}

int sv_save(const char *db_path)
{
    int rc = 0;
    pthread_rwlock_rdlock(&g_store.lock);
    const char *path = db_path ? db_path : g_store.db_path;
    if (!path)
    {
        pthread_rwlock_unlock(&g_store.lock);
        errno = EINVAL;
        return -1;
    }
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    pthread_rwlock_unlock(&g_store.lock);

    if (ensure_parent_dir(path) != 0)
        return -1;

    FILE *fp = fopen(tmp_path, "w");
    if (!fp)
        return -1;

    pthread_rwlock_rdlock(&g_store.lock);
    for (size_t i = 0; i < SV_BUCKETS; i++)
    {
        for (SVVar *v = g_store.buckets[i]; v; v = v->next)
        {
            char *esc = escape_value(v->val);
            fprintf(fp, "%s\t%s\t%s\n", v->key, esc, (v->flags & SV_FLAG_RDONLY) ? "ro" : "-");
            free(esc);
        }
    }
    pthread_rwlock_unlock(&g_store.lock);

    if (fclose(fp) != 0)
    {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, path) != 0)
    {
        unlink(tmp_path);
        return -1;
    }
    return rc;
}

int sv_set(const char *key, const char *val, unsigned flags, bool overwrite)
{
    int rc;
    pthread_rwlock_wrlock(&g_store.lock);
    rc = sv_set_locked(key, val, flags, overwrite);
    pthread_rwlock_unlock(&g_store.lock);
    return rc;
}

int sv_get(const char *key, char **out_val, unsigned *out_flags)
{
    int rc = 0;
    pthread_rwlock_rdlock(&g_store.lock);
    SVVar *v = sv_find_locked(key, NULL);
    if (!v)
    {
        rc = -1;
    }
    else
    {
        if (out_val)
            *out_val = xstrdup(v->val);
        if (out_flags)
            *out_flags = v->flags;
    }
    pthread_rwlock_unlock(&g_store.lock);
    return rc;
}

int sv_unset(const char *key)
{
    int rc;
    pthread_rwlock_wrlock(&g_store.lock);
    rc = sv_unset_locked(key);
    pthread_rwlock_unlock(&g_store.lock);
    return rc;
}

/* Collect variables matching prefix (or all if prefix==NULL). Caller must free vec and entries. */
size_t sv_list(const char *prefix, SVVar ***out_vec)
{
    size_t cap = 64, n = 0;
    SVVar **vec = xmalloc(cap * sizeof(*vec));
    pthread_rwlock_rdlock(&g_store.lock);
    for (size_t i = 0; i < SV_BUCKETS; i++)
    {
        for (SVVar *v = g_store.buckets[i]; v; v = v->next)
        {
            if (prefix && strncmp(v->key, prefix, strlen(prefix)) != 0)
                continue;
            if (n == cap)
            {
                cap *= 2;
                vec = realloc(vec, cap * sizeof(*vec));
                if (!vec)
                {
                    perror("realloc");
                    exit(1);
                }
            }
            vec[n++] = v; /* note: pointing into store; do not free entries */
        }
    }
    pthread_rwlock_unlock(&g_store.lock);
    /* sort by key for stable output */
    int cmp(const void *a, const void *b)
    {
        const SVVar *va = *(const SVVar *const *)a;
        const SVVar *vb = *(const SVVar *const *)b;
        return strcmp(va->key, vb->key);
    }
    qsort(vec, n, sizeof(*vec), cmp);
    *out_vec = vec;
    return n;
}

/* ---------- CLI ---------- */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-f <dbfile>] <cmd> [args]\n"
            "Commands:\n"
            "  list [prefix]\n"
            "  get <key>\n"
            "  set [-o] [-r] <key> <value>\n"
            "     -o  overwrite existing\n"
            "     -r  mark as read-only\n"
            "  unset <key>\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *dbfile = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "f:h")) != -1)
    {
        switch (opt)
        {
        case 'f':
            dbfile = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h' ? 0 : 1);
        }
    }
    if (!dbfile)
        dbfile = sv_default_path();
    if (sv_init(dbfile) != 0)
    {
        fprintf(stderr, "warning: could not load '%s': %s\n", dbfile, strerror(errno));
    }

    if (optind >= argc)
    {
        usage(argv[0]);
        return 1;
    }
    const char *cmd = argv[optind++];

    if (strcmp(cmd, "list") == 0)
    {
        const char *prefix = (optind < argc) ? argv[optind++] : NULL;
        SVVar **vec = NULL;
        size_t n = sv_list(prefix, &vec);
        for (size_t i = 0; i < n; i++)
        {
            const SVVar *v = vec[i];
            printf("%s = %s%s\n", v->key, v->val, (v->flags & SV_FLAG_RDONLY) ? "\t#ro" : "");
        }
        free(vec);
        return 0;
    }
    else if (strcmp(cmd, "get") == 0)
    {
        if (optind >= argc)
        {
            usage(argv[0]);
            return 1;
        }
        const char *key = argv[optind++];
        char *val = NULL;
        unsigned fl = 0;
        if (sv_get(key, &val, &fl) != 0)
        {
            fprintf(stderr, "not found: %s\n", key);
            return 2;
        }
        printf("%s\n", val);
        free(val);
        return 0;
    }
    else if (strcmp(cmd, "set") == 0)
    {
        bool overwrite = false;
        bool readonly = false;
        /* parse optional flags after 'set' */
        while (optind < argc && argv[optind][0] == '-' && argv[optind][1] && strcmp(argv[optind], "--") != 0)
        {
            if (strcmp(argv[optind], "-o") == 0)
                overwrite = true;
            else if (strcmp(argv[optind], "-r") == 0)
                readonly = true;
            else
                break;
            optind++;
        }
        if (optind + 2 > argc)
        {
            usage(argv[0]);
            return 1;
        }
        const char *key = argv[optind++];
        const char *val = argv[optind++];
        unsigned flags = readonly ? SV_FLAG_RDONLY : 0;
        int rc = sv_set(key, val, flags, overwrite);
        if (rc == -2)
        {
            fprintf(stderr, "refused: '%s' is read-only\n", key);
            return 3;
        }
        if (rc == -1)
        {
            fprintf(stderr, "exists, use -o to overwrite: %s\n", key);
            return 4;
        }
        if (sv_save(NULL) != 0)
        {
            fprintf(stderr, "warning: save failed: %s\n", strerror(errno));
        }
        return 0;
    }
    else if (strcmp(cmd, "unset") == 0)
    {
        if (optind >= argc)
        {
            usage(argv[0]);
            return 1;
        }
        const char *key = argv[optind++];
        int rc = sv_unset(key);
        if (rc == -2)
        {
            fprintf(stderr, "refused: '%s' is read-only\n", key);
            return 3;
        }
        if (rc != 0)
        {
            fprintf(stderr, "not found: %s\n", key);
            return 2;
        }
        if (sv_save(NULL) != 0)
        {
            fprintf(stderr, "warning: save failed: %s\n", strerror(errno));
        }
        return 0;
    }
    else
    {
        usage(argv[0]);
        return 1;
    }
}
