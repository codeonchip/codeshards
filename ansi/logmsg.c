/*
 * logmsg.c — minimal Interpeak/IPnet-style logging for Linux
 *
 * Features
 *  - Per-module levels: OFF,FATAL,ERROR,WARN,INFO,DEBUG,TRACE
 *  - Runtime control via a small mmap'd state file shared by processes
 *  - Thread-safe emission; timestamped single-line output
 *  - Sinks: stderr (default) or file; optional syslog when compiled with -DUSE_SYSLOG
 *  - Optional integration with sysvar (read initial levels) when linking with sysvar.c
 *
 * Build (CLI):
 *   gcc -O2 -Wall -pthread -DLOGMSG_MAIN logmsg.c -o logmsg
 *
 * Library use:
 *   gcc -O2 -Wall -pthread -c logmsg.c -o logmsg.o
 *   // in your app:
 *   LOGREG("iptcp", LOG_INFO);
 *   LOGMSG("iptcp", LOG_INFO, "hello %s", "world");
 *
 * Optional flags:
 *   -DUSE_SYSVAR  (reads keys: log.<module>.level)
 *   -DUSE_SYSLOG  (enables syslog sink; CLI: out syslog)
 */

#if 0

LOGREG("iptcp", LOG_INFO);
LOGMSG("iptcp", LOG_INFO, "hello %s", "world");

# state file: $LOGMSG_STATE or /tmp/logmsg.state
./logmsg list
./logmsg set iptcp TRACE
./logmsg off iptcp
./logmsg on iptcp
./logmsg out file /tmp/iptcp.log
./logmsg out stderr
./logmsg tail

#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#ifdef USE_SYSLOG
#include <syslog.h>
#endif

/* ===== Levels ===== */
typedef enum
{
    LOG_OFF = -1,
    LOG_FATAL = 0,
    LOG_ERROR = 1,
    LOG_WARN = 2,
    LOG_INFO = 3,
    LOG_DEBUG = 4,
    LOG_TRACE = 5
} log_level_t;
static const char *level_name[] = {"FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
static inline const char *level_to_str(log_level_t L) { return (L < 0) ? "OFF" : level_name[(unsigned)L]; }
static inline log_level_t str_to_level(const char *s)
{
    if (!s)
        return LOG_INFO;
    if (!strcasecmp(s, "OFF"))
        return LOG_OFF;
    if (!strcasecmp(s, "FATAL"))
        return LOG_FATAL;
    if (!strcasecmp(s, "ERROR"))
        return LOG_ERROR;
    if (!strcasecmp(s, "WARN") || !strcasecmp(s, "WARNING"))
        return LOG_WARN;
    if (!strcasecmp(s, "INFO"))
        return LOG_INFO;
    if (!strcasecmp(s, "DEBUG"))
        return LOG_DEBUG;
    if (!strcasecmp(s, "TRACE"))
        return LOG_TRACE;
    return LOG_INFO;
}

/* ===== Shared state (mmap) ===== */
#define MAX_MODULES 256
#define MODNAME_LEN 32

typedef struct
{
    char name[MODNAME_LEN];
    int32_t level;
} mod_slot_t;

typedef struct
{
    uint32_t magic;   /* 'LMSG' */
    uint32_t version; /* 1 */
    int32_t out_mode; /* 0=stderr, 1=file, 2=syslog */
    char out_path[256];
    mod_slot_t mod[MAX_MODULES];
} log_state_t;

static const uint32_t LOG_MAGIC = 0x4c4d5347; /* 'LMSG' */
static const uint32_t LOG_VERSION = 1;

static log_state_t *g_state;
static FILE *g_out_fp; /* when out_mode==file */
static pthread_mutex_t g_out_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *state_path(void)
{
    const char *p = getenv("LOGMSG_STATE");
    return p && *p ? p : "/tmp/logmsg.state";
}

static void state_init(void)
{
    if (g_state)
        return;
    const char *path = state_path();
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
    {
        perror("open state");
        exit(1);
    }
    size_t need = sizeof(log_state_t);
    if (ftruncate(fd, (off_t)need) != 0)
    {
        perror("ftruncate state");
        close(fd);
        exit(1);
    }
    void *mem = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        perror("mmap state");
        close(fd);
        exit(1);
    }
    close(fd);
    g_state = (log_state_t *)mem;
    if (g_state->magic != LOG_MAGIC || g_state->version != LOG_VERSION)
    {
        memset(g_state, 0, sizeof(*g_state));
        g_state->magic = LOG_MAGIC;
        g_state->version = LOG_VERSION;
        g_state->out_mode = 0; /* stderr */
        g_state->out_path[0] = '\0';
        for (int i = 0; i < MAX_MODULES; i++)
        {
            g_state->mod[i].name[0] = '\0';
            g_state->mod[i].level = LOG_INFO;
        }
    }
}

/* Register or lookup a module slot */
static mod_slot_t *mod_get(const char *name, bool create)
{
    state_init();
    for (int i = 0; i < MAX_MODULES; i++)
    {
        if (g_state->mod[i].name[0] && strncmp(g_state->mod[i].name, name, MODNAME_LEN) == 0)
            return &g_state->mod[i];
    }
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_MODULES; i++)
    {
        if (!g_state->mod[i].name[0])
        {
            snprintf(g_state->mod[i].name, MODNAME_LEN, "%s", name);
            g_state->mod[i].level = LOG_INFO;
            return &g_state->mod[i];
        }
    }
    return NULL; /* no space */
}

/* ===== Output sink ===== */
static void ensure_output_open(void)
{
    state_init();
    pthread_mutex_lock(&g_out_mu);
    if (g_state->out_mode == 1)
    { /* file */
        if (!g_out_fp)
        {
            g_out_fp = fopen(g_state->out_path[0] ? g_state->out_path : "/tmp/logmsg.log", "a");
            if (!g_out_fp)
            {
                perror("fopen log");
                g_out_fp = stderr;
            }
        }
    }
    else
    {
        if (g_out_fp && g_out_fp != stderr)
        {
            fclose(g_out_fp);
            g_out_fp = NULL;
        }
        g_out_fp = stderr;
    }
    pthread_mutex_unlock(&g_out_mu);
}

static void emit_line(const char *module, log_level_t lvl, const char *msg)
{
    ensure_output_open();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
    pthread_mutex_lock(&g_out_mu);
#ifdef USE_SYSLOG
    if (g_state->out_mode == 2)
    {
        int pri = LOG_INFO;
        if (lvl <= LOG_FATAL)
            pri = LOG_CRIT;
        else if (lvl == LOG_ERROR)
            pri = LOG_ERR;
        else if (lvl == LOG_WARN)
            pri = LOG_WARNING;
        else if (lvl == LOG_DEBUG || lvl == LOG_TRACE)
            pri = LOG_DEBUG;
        openlog("logmsg", LOG_PID | LOG_CONS, LOG_USER);
        syslog(pri, "%s: %s", module, msg);
        closelog();
    }
    else
#endif
    {
        FILE *fp = g_out_fp ? g_out_fp : stderr;
        fprintf(fp, "%s.%03ld %-5s %-12s | %s\n", tbuf, ts.tv_nsec / 1000000L, level_to_str(lvl), module, msg);
        fflush(fp);
    }
    pthread_mutex_unlock(&g_out_mu);
}

/* ===== Public API ===== */
#define LOGREG(module, default_level)            \
    do                                           \
    {                                            \
        log_register((module), (default_level)); \
    } while (0)
#define LOGMSG(module, level, fmt, ...)                        \
    do                                                         \
    {                                                          \
        if (log_would_log((module), (level)))                  \
            log_logf((module), (level), (fmt), ##__VA_ARGS__); \
    } while (0)

static void log_register(const char *module, log_level_t deflevel)
{
    mod_slot_t *m = mod_get(module, true);
    if (!m)
        return;
    if (m->level == LOG_INFO)
        m->level = deflevel; /* first registrant sets default */
#ifdef USE_SYSVAR
    extern int sv_get(const char *, char **, unsigned *); /* from sysvar.c */
    char key[128];
    snprintf(key, sizeof(key), "log.%s.level", module);
    char *v = NULL;
    if (sv_get(key, &v, NULL) == 0 && v)
    {
        m->level = str_to_level(v);
        free(v);
    }
#endif
}

static bool log_would_log(const char *module, log_level_t lvl)
{
    mod_slot_t *m = mod_get(module, false);
    if (!m)
        return (lvl <= LOG_INFO); /* before registration, be conservative */
    return (m->level != LOG_OFF && lvl <= m->level);
}

static void log_logf(const char *module, log_level_t lvl, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit_line(module, lvl, buf);
}

/* ===== CLI helpers ===== */
static int cmd_list(void)
{
    state_init();
    for (int i = 0; i < MAX_MODULES; i++)
    {
        if (!g_state->mod[i].name[0])
            continue;
        int32_t L = g_state->mod[i].level;
        const char *s = (L == LOG_OFF) ? "OFF" : level_to_str((log_level_t)L);
        printf("%-16s %s\n", g_state->mod[i].name, s);
    }
    return 0;
}

static int cmd_set(const char *mod, const char *lvl)
{
    mod_slot_t *m = mod_get(mod, true);
    if (!m)
    {
        fprintf(stderr, "no free module slots\n");
        return 1;
    }
    m->level = str_to_level(lvl);
    return 0;
}

static int cmd_out(int argc, char **argv)
{
    if (argc < 1)
    {
        fprintf(stderr, "out <stderr|file <path>|syslog>\n");
        return 1;
    }
    if (!strcmp(argv[0], "stderr"))
        g_state->out_mode = 0;
    else if (!strcmp(argv[0], "file"))
    {
        if (argc < 2)
        {
            fprintf(stderr, "out file <path>\n");
            return 1;
        }
        g_state->out_mode = 1;
        snprintf(g_state->out_path, sizeof(g_state->out_path), "%s", argv[1]);
    }
#ifdef USE_SYSLOG
    else if (!strcmp(argv[0], "syslog"))
        g_state->out_mode = 2;
#endif
    else
    {
        fprintf(stderr, "unknown sink: %s\n", argv[0]);
        return 1;
    }
    pthread_mutex_lock(&g_out_mu);
    if (g_out_fp && g_out_fp != stderr)
    {
        fclose(g_out_fp);
        g_out_fp = NULL;
    }
    pthread_mutex_unlock(&g_out_mu);
    return 0;
}

static int cmd_tail(void)
{
    if (g_state->out_mode != 1)
    {
        fprintf(stderr, "not in file mode. use: out file <path>\n");
        return 1;
    }
    const char *p = g_state->out_path[0] ? g_state->out_path : "/tmp/logmsg.log";
    FILE *fp = fopen(p, "r");
    if (!fp)
    {
        perror("fopen");
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    char line[2048];
    for (;;)
    {
        if (fgets(line, sizeof(line), fp))
        {
            fputs(line, stdout);
            fflush(stdout);
        }
        else
        {
            clearerr(fp);
            usleep(100 * 1000);
        }
    }
}

/* ===== CLI entry (optional) ===== */
#ifdef LOGMSG_MAIN
static void usage(const char *p)
{
    fprintf(stderr,
            "Usage: %s <cmd> [args]\n"
            "  list\n"
            "  set <module> <level>\n"
            "  on <module>    (alias: set <module> DEBUG)\n"
            "  off <module>   (alias: set <module> OFF)\n"
            "  out <stderr | file <path> | syslog>\n"
            "  tail\n",
            p);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage(argv[0]);
        return 1;
    }
    state_init();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "list"))
        return cmd_list();
    if (!strcmp(cmd, "set"))
    {
        if (argc < 4)
        {
            usage(argv[0]);
            return 1;
        }
        return cmd_set(argv[2], argv[3]);
    }
    if (!strcmp(cmd, "on"))
    {
        if (argc < 3)
        {
            usage(argv[0]);
            return 1;
        }
        return cmd_set(argv[2], "DEBUG");
    }
    if (!strcmp(cmd, "off"))
    {
        if (argc < 3)
        {
            usage(argv[0]);
            return 1;
        }
        return cmd_set(argv[2], "OFF");
    }
    if (!strcmp(cmd, "out"))
    {
        if (argc < 3)
        {
            usage(argv[0]);
            return 1;
        }
        return cmd_out(argc - 2, argv + 2);
    }
    if (!strcmp(cmd, "tail"))
        return cmd_tail();
    usage(argv[0]);
    return 1;
}
#endif
