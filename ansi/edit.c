/* edit.c — terminal text editor (C99, POSIX)
 * Features:
 *  - Open/save UTF-8 text (no special shaping), insert/delete, newlines
 *  - Cursor movement, scrolling, Home/End, PageUp/Down
 *  - Status & message bars, dirty flag with “press Ctrl-Q again” guard
 *  - Simple forward search (Ctrl-F), repeat with Enter, cancel with ESC
 *
 * Works on Linux/macOS/BSD terminals (and on Windows via WSL/MSYS2).
 *
 * Build: gcc -std=c99 -Wall -Wextra -O2 -pedantic edit.c -o edit
 */

/*
  Keys
    Ctrl-S = save
    Ctrl-Q = quit (press twice if there are unsaved changes)
    Ctrl-F = search (ESC to cancel; Enter finds next)
    Backspace / Delete / Enter work as expected
    Arrows, PageUp/Down, Home/End to move
*/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---- Config ---- */
#define TAB_STOP 4
#define MINI_VERSION "0.1"

/* ---- Key codes ---- */
#define CTRL_KEY(k) ((k) & 0x1f)
enum
{
    KEY_ARROW_LEFT = 1000,
    KEY_ARROW_RIGHT,
    KEY_ARROW_UP,
    KEY_ARROW_DOWN,
    KEY_DEL,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN
};

/* ---- Row model ---- */
typedef struct
{
    int size;     /* chars length */
    int rsize;    /* render length (tabs expanded) */
    char *chars;  /* raw bytes */
    char *render; /* for drawing */
} erow;

/* ---- Editor state ---- */
struct editorConfig
{
    int cx, cy; /* cursor x in chars, y in rows */
    int rx;     /* render x (tabs expanded) */
    int rowoff; /* row scroll offset */
    int coloff; /* column scroll offset */
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
} E;

/* ---- Terminal raw mode ---- */
static void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7); /* clear screen */
    perror(s);
    exit(1);
}

static void disableRawMode(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

static void enableRawMode(void)
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; /* 100ms */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

/* ---- low-level I/O ---- */
static int editorReadKey(void)
{
    char c;
    while (1)
    {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 1)
            break;
        if (n == -1 && errno != EAGAIN)
            die("read");
    }

    if (c == '\x1b')
    { /* escape sequence */
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                char seq2;
                if (read(STDIN_FILENO, &seq2, 1) != 1)
                    return '\x1b';
                if (seq2 == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return KEY_HOME;
                    case '3':
                        return KEY_DEL;
                    case '4':
                        return KEY_END;
                    case '5':
                        return KEY_PAGE_UP;
                    case '6':
                        return KEY_PAGE_DOWN;
                    case '7':
                        return KEY_HOME;
                    case '8':
                        return KEY_END;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return KEY_ARROW_UP;
                case 'B':
                    return KEY_ARROW_DOWN;
                case 'C':
                    return KEY_ARROW_RIGHT;
                case 'D':
                    return KEY_ARROW_LEFT;
                case 'H':
                    return KEY_HOME;
                case 'F':
                    return KEY_END;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return KEY_HOME;
            case 'F':
                return KEY_END;
            }
        }
        return '\x1b';
    }

    return (unsigned char)c;
}

static int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;
    return 0;
}

static int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/* ---- Row ops ---- */
static int editorRowCxToRx(erow *row, int cx)
{
    int rx = 0;
    for (int j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
            rx += (TAB_STOP - 1) - (rx % TAB_STOP) + 1;
        else
            rx++;
    }
    return rx;
}

static int editorRowRxToCx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
            cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP) + 1;
        else
            cur_rx++;
        if (cur_rx > rx)
            return cx;
    }
    return cx;
}

static void editorUpdateRow(erow *row)
{
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t')
            tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);
    if (!row->render)
        die("malloc");

    int idx = 0;
    for (int j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0)
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

static void editorInsertRow(int at, const char *s, int len)
{
    if (at < 0 || at > E.numrows)
        return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    if (!E.row)
        die("realloc");
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    if (!E.row[at].chars)
        die("malloc");
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].render = NULL;
    E.row[at].rsize = 0;
    editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

static void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
}

static void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows)
        return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

static void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    if (!row->chars)
        die("realloc");
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

static void editorRowAppendString(erow *row, const char *s, int len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    if (!row->chars)
        die("realloc");
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

static void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at >= row->size)
        return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* ---- Editor ops ---- */
static void editorInsertChar(int c)
{
    if (E.cy == E.numrows)
    {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

static void editorInsertNewline(void)
{
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    }
    else
    {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

static void editorDelChar(void)
{
    if (E.cy == E.numrows)
        return;
    if (E.cx == 0 && E.cy == 0)
        return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else
    {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/* ---- File I/O ---- */
struct abuf
{
    char *b;
    int len;
};
#define ABUF_INIT {NULL, 0}
static void abAppend(struct abuf *ab, const char *s, int len)
{
    char *newb = realloc(ab->b, ab->len + len);
    if (!newb)
        return;
    memcpy(&newb[ab->len], s, len);
    ab->b = newb;
    ab->len += len;
}
static void abFree(struct abuf *ab) { free(ab->b); }

static char *editorRowsToString(int *buflen)
{
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    if (!buf)
        die("malloc");
    int p = 0;
    for (int j = 0; j < E.numrows; j++)
    {
        memcpy(&buf[p], E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        buf[p++] = '\n';
    }
    return buf;
}

static void editorOpen(const char *filename)
{
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "rb");
    if (!fp)
    { /* new file */
        E.dirty = 0;
        return;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, fp)) != -1)
    {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            len--;
        editorInsertRow(E.numrows, line, (int)len);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

static void editorSave(void)
{
    if (E.filename == NULL)
        return;

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1)
    {
        if (ftruncate(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0;
                snprintf(E.statusmsg, sizeof(E.statusmsg), "\"%s\" (%d bytes) written", E.filename, len);
                E.statusmsg_time = time(NULL);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Can't save! I/O error: %s", strerror(errno));
    E.statusmsg_time = time(NULL);
}

/* ---- Find ---- */
static void editorFindCallback(const char *query, int key, int *last_match_row, int *last_match_off)
{
    static int direction = 1; /* 1 forward, -1 backward */

    if (key == KEY_ARROW_RIGHT || key == KEY_ARROW_DOWN)
        direction = 1;
    else if (key == KEY_ARROW_LEFT || key == KEY_ARROW_UP)
        direction = -1;
    else if (key == '\r')
        direction = 1;
    else
        direction = 1;

    if (*last_match_row == -1)
    {
        E.cy = 0;
        E.cx = 0;
    }

    int current = *last_match_row;
    int start = current == -1 ? 0 : current;
    for (int i = 0; i < E.numrows; i++)
    {
        current += direction;
        if (current == -1)
            current = E.numrows - 1;
        if (current == E.numrows)
            current = 0;

        erow *row = &E.row[current];
        char *match = query && *query ? strstr(row->render, query) : NULL;
        if (match)
        {
            *last_match_row = current;
            *last_match_off = (int)(match - row->render);
            E.cy = current;
            E.cx = editorRowRxToCx(row, *last_match_off);
            E.rowoff = E.numrows; /* force scroll to center on refresh */
            break;
        }
    }
}

static char *editorPrompt(const char *prompt, void (*callback)(const char *, int, int *, int *))
{
    size_t bufcap = 128, buflen = 0;
    char *buf = malloc(bufcap);
    if (!buf)
        die("malloc");
    buf[0] = '\0';

    int last_match_row = -1, last_match_off = 0;

    while (1)
    {
        snprintf(E.statusmsg, sizeof(E.statusmsg), prompt, buf);
        E.statusmsg_time = time(NULL);
        /* draw to show prompt */
        extern void editorRefreshScreen(void);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == '\x1b')
        { /* ESC cancel */
            snprintf(E.statusmsg, sizeof(E.statusmsg), "");
            if (callback)
                callback(NULL, c, &last_match_row, &last_match_off);
            free(buf);
            return NULL;
        }
        else if (c == '\r')
        {
            if (callback)
                callback(buf, c, &last_match_row, &last_match_off);
            if (buflen != 0)
            {
                snprintf(E.statusmsg, sizeof(E.statusmsg), "");
                return buf;
            }
        }
        else if (c == KEY_DEL || c == CTRL_KEY('h') || c == 127)
        {
            if (buflen)
                buf[--buflen] = '\0';
        }
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen + 1 >= bufcap)
            {
                bufcap *= 2;
                buf = realloc(buf, bufcap);
                if (!buf)
                    die("realloc");
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback)
            callback(buf, c, &last_match_row, &last_match_off);
    }
}

static void editorFind(void)
{
    int saved_cx = E.cx, saved_cy = E.cy;
    int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (ESC to cancel, Enter for next)", editorFindCallback);
    if (query)
        free(query);
    else
    { /* restore cursor/scroll if cancelled */
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/* ---- Output ---- */
static void editorScroll(void)
{
    E.rx = 0;
    if (E.cy < E.numrows)
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows - 2)
        E.rowoff = E.cy - E.screenrows + 2;

    if (E.rx < E.coloff)
        E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols)
        E.coloff = E.rx - E.screencols + 1;
}

static void editorDrawRows(struct abuf *ab)
{
    for (int y = 0; y < E.screenrows - 2; y++)
    {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows)
        {
            if (E.numrows == 0 && y == (E.screenrows - 2) / 3)
            {
                char welcome[80];
                int wel_len = snprintf(welcome, sizeof(welcome),
                                       "mini_edit v%s — Ctrl-S:Save  Ctrl-Q:Quit  Ctrl-F:Find", MINI_VERSION);
                if (wel_len > E.screencols)
                    wel_len = E.screencols;
                int padding = (E.screencols - wel_len) / 2;
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    abAppend(ab, " ", 1);
                abAppend(ab, welcome, wel_len);
            }
            else
            {
                abAppend(ab, "~", 1);
            }
        }
        else
        {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0)
                len = 0;
            if (len > E.screencols)
                len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3); /* clear line right */
        abAppend(ab, "\r\n", 2);
    }
}

static void editorDrawStatusBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[7m", 4); /* invert */
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s%s - %d lines %s",
                       E.filename ? E.filename : "[No Name]",
                       E.dirty ? " *" : "", E.numrows,
                       "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d,%d",
                        E.cy + 1, E.cx + 1);
    if (len > E.screencols)
        len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols - rlen)
    {
        abAppend(ab, " ", 1);
        len++;
    }
    abAppend(ab, rstatus, rlen);
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

static void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);
    int msglen = (int)strlen(E.statusmsg);
    if (msglen > E.screencols)
        msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(void)
{
    editorScroll();

    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6); /* hide cursor */
    abAppend(&ab, "\x1b[H", 3);    /* cursor to 1;1 */

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    int cy = (E.cy - E.rowoff) + 1;
    int cx = (E.rx - E.coloff) + 1;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cy, cx);
    abAppend(&ab, buf, n);

    abAppend(&ab, "\x1b[?25h", 6); /* show cursor */
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/* ---- Status msg ---- */
static void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* ---- Input ---- */
static void editorMoveCursor(int key)
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key)
    {
    case KEY_ARROW_LEFT:
        if (E.cx != 0)
            E.cx--;
        else if (E.cy > 0)
        {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case KEY_ARROW_RIGHT:
        if (row && E.cx < row->size)
            E.cx++;
        else if (row && E.cx == row->size && E.cy + 1 < E.numrows)
        {
            E.cy++;
            E.cx = 0;
        }
        break;
    case KEY_ARROW_UP:
        if (E.cy != 0)
            E.cy--;
        break;
    case KEY_ARROW_DOWN:
        if (E.cy < E.numrows)
            E.cy++;
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen)
        E.cx = rowlen;
}

static void editorProcessKeypress(void)
{
    static int quit_times = 1;

    int c = editorReadKey();

    switch (c)
    {
    case '\r': /* Enter */
        editorInsertNewline();
        break;

    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0)
        {
            editorSetStatusMessage("Unsaved changes. Press Ctrl-Q again to quit.");
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
        exit(0);
        break;

    case CTRL_KEY('s'):
        if (!E.filename)
        {
            char *name = editorPrompt("Save as: %s (ESC to cancel)", NULL);
            if (name)
            {
                free(E.filename);
                E.filename = name;
                editorSave();
            }
            else
            {
                editorSetStatusMessage("Save aborted");
            }
        }
        else
            editorSave();
        break;

    case CTRL_KEY('f'):
        editorFind();
        break;

    case KEY_HOME:
        E.cx = 0;
        break;

    case KEY_END:
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        /* ignore */
        break;

    case KEY_PAGE_UP:
    case KEY_PAGE_DOWN:
    {
        if (c == KEY_PAGE_UP)
            E.cy = E.rowoff;
        else if (c == KEY_PAGE_DOWN)
            E.cy = E.rowoff + E.screenrows - 2;
        if (E.cy > E.numrows)
            E.cy = E.numrows;
        int times = E.screenrows - 2;
        while (times--)
            editorMoveCursor(c == KEY_PAGE_UP ? KEY_ARROW_UP : KEY_ARROW_DOWN);
    }
    break;

    case KEY_ARROW_UP:
    case KEY_ARROW_DOWN:
    case KEY_ARROW_LEFT:
    case KEY_ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case KEY_DEL:
    case CTRL_KEY('h'):
    case 127:
        editorDelChar();
        break;

    case CTRL_KEY('a'):
        E.cx = 0;
        break;

    case CTRL_KEY('e'):
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
        break;

    default:
        if (!iscntrl(c))
            editorInsertChar(c);
        break;
    }
    quit_times = 1;
}

/* ---- Init ---- */
static void initEditor(void)
{
    E.cx = E.cy = E.rx = 0;
    E.rowoff = E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    /* two lines for status+message */
    E.screenrows -= 2;
    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
}

/* ---- Main ---- */
int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
        editorOpen(argv[1]);

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
