/* rogue.c
 * A tiny Rogue-style, single-file C game for terminal (Linux/macOS/Windows).
 * - Random rooms + corridors
 * - FOV & map memory
 * - Monsters with simple chase AI
 * - Items: potions(!) heal, rations(%) feed
 * - Stairs(>) to descend; Amulet(*) on final floor
 *
 * Build:
 *   gcc -std=c11 -O2 -Wall -Wextra -o rogue rogue.c   (Linux/macOS)
 *   cl /std:c11 /O2 rogue.c                           (Windows)
 *
 * Controls:
 *   Arrows / WASD / HJKL (+ diagonals Y U B N), '>' to descend, 'Q' to quit
 *
 * Notes:
 * - Uses ANSI escapes; on modern Windows 10+ consoles this works. We enable it via
 *   SetConsoleMode if available; otherwise most newer terminals handle it fine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#endif

/* ---------- Config ---------- */
#define MAP_W 80
#define MAP_H 24 /* we'll use last line as status; visible area MAP_H-1 */
#define VIEW_R 8
#define MAX_ROOMS 32
#define MAX_MOBS 64
#define MAX_ITEMS 64
#define FLOORS 5

/* ---------- RNG helpers ---------- */
static int irand(int a, int b) { return a + rand() % (b - a + 1); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---------- Terminal control ---------- */
static void term_write(const char *s) { fputs(s, stdout); }
static void term_move(int y, int x) { printf("\x1b[%d;%dH", y, x); }
static void term_clear(void) { term_write("\x1b[2J\x1b[H"); }
static void term_hide_cursor(void) { term_write("\x1b[?25l"); }
static void term_show_cursor(void) { term_write("\x1b[?25h"); }

#if defined(_WIN32)
static DWORD g_origMode = 0;
static void enable_vt(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE)
        return;
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode))
    {
        g_origMode = mode;
        mode |= 0x0004; /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
        SetConsoleMode(hOut, mode);
    }
}
static void restore_vt(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && g_origMode)
        SetConsoleMode(hOut, g_origMode);
}
#else
static struct termios orig_term;
static void set_raw(void)
{
    tcgetattr(STDIN_FILENO, &orig_term);
    struct termios raw = orig_term;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VTIME] = 0;
    raw.c_cc[VMIN] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}
static void restore_term(void) { tcsetattr(STDIN_FILENO, TCSANOW, &orig_term); }
#endif

/* Unified getch (returns a single char/int key code) */
enum
{
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_Y,
    KEY_U,
    KEY_B,
    KEY_N,
    KEY_CHAR
}; /* KEY_CHAR implies see g_last_char */

static int g_last_char = 0;

static int read_key(void)
{
#if defined(_WIN32)
    int c = _getch();
    if (c == 0 || c == 224)
    {
        int c2 = _getch();
        switch (c2)
        {
        case 72:
            return KEY_UP;
        case 80:
            return KEY_DOWN;
        case 75:
            return KEY_LEFT;
        case 77:
            return KEY_RIGHT;
        default:
            return KEY_NONE;
        }
    }
    else
    {
        c = tolower(c);
        switch (c)
        {
        case 'w':
        case 'k':
            return KEY_UP;
        case 's':
        case 'j':
            return KEY_DOWN;
        case 'a':
        case 'h':
            return KEY_LEFT;
        case 'd':
        case 'l':
            return KEY_RIGHT;
        case 'y':
            return KEY_Y;
        case 'u':
            return KEY_U;
        case 'b':
            return KEY_B;
        case 'n':
            return KEY_N;
        default:
            g_last_char = c;
            return KEY_CHAR;
        }
    }
#else
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) <= 0)
        return KEY_NONE;
    if (c == 27)
    { /* ESC sequence */
        /* Try to parse CSI */
        struct timeval tv = {0, 20000}; /* 20ms */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0)
        {
            unsigned char b = 0;
            if (read(STDIN_FILENO, &b, 1) > 0 && b == '[')
            {
                unsigned char d = 0;
                if (read(STDIN_FILENO, &d, 1) > 0)
                {
                    switch (d)
                    {
                    case 'A':
                        return KEY_UP;
                    case 'B':
                        return KEY_DOWN;
                    case 'D':
                        return KEY_LEFT;
                    case 'C':
                        return KEY_RIGHT;
                    default:
                        return KEY_NONE;
                    }
                }
            }
        }
        return KEY_NONE;
    }
    else
    {
        c = tolower(c);
        switch (c)
        {
        case 'w':
        case 'k':
            return KEY_UP;
        case 's':
        case 'j':
            return KEY_DOWN;
        case 'a':
        case 'h':
            return KEY_LEFT;
        case 'd':
        case 'l':
            return KEY_RIGHT;
        case 'y':
            return KEY_Y;
        case 'u':
            return KEY_U;
        case 'b':
            return KEY_B;
        case 'n':
            return KEY_N;
        default:
            g_last_char = c;
            return KEY_CHAR;
        }
    }
#endif
}

/* ---------- Map & Entities ---------- */
typedef enum
{
    T_WALL,
    T_FLOOR,
    T_STAIR,
    T_AMULET
} TileType;

typedef struct
{
    TileType t;
    bool seen; /* ever seen (memory) */
    bool vis;  /* currently visible */
} Tile;

static Tile mapc[MAP_H - 1][MAP_W]; /* last line reserved for status */

typedef struct
{
    int x, y, w, h;
} Rect;

typedef struct
{
    int x, y;
    int hp, hpmax;
    int hunger; /* 0..1000+ */
    bool have_amulet;
    int floor;
} Player;

typedef enum
{
    MOB_GOBLIN = 1,
    MOB_ORC = 2
} MobType;

typedef struct
{
    bool alive;
    MobType type;
    int x, y;
    int hp, hpmax;
    char ch;
    const char *name;
} Mob;

typedef enum
{
    IT_POTION = 1,
    IT_RATION = 2
} ItemType;

typedef struct
{
    bool alive;
    ItemType type;
    int x, y;
    char ch;
    const char *name;
} Item;

static Player pl;
static Mob mobs[MAX_MOBS];
static Item items[MAX_ITEMS];
static int stairs_x = 0, stairs_y = 0;

/* ---------- Utils ---------- */
static bool in_bounds(int x, int y)
{
    return x >= 0 && x < MAP_W && y >= 0 && y < (MAP_H - 1);
}
static bool is_blocking(int x, int y)
{
    if (!in_bounds(x, y))
        return true;
    TileType t = mapc[y][x].t;
    return (t == T_WALL);
}
static bool is_opaque(int x, int y)
{
    if (!in_bounds(x, y))
        return true;
    return mapc[y][x].t == T_WALL;
}
static void place_player_in_rect(Rect r)
{
    pl.x = r.x + r.w / 2;
    pl.y = r.y + r.h / 2;
}

/* ---------- Rooms & dungeon gen ---------- */
static Rect rooms[MAX_ROOMS];
static int nrooms = 0;

static bool rect_overlap(Rect a, Rect b)
{
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}
static void carve_rect(Rect r)
{
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            if (in_bounds(x, y))
                mapc[y][x].t = T_FLOOR;
}
static void carve_h_tunnel(int x1, int x2, int y)
{
    if (x2 < x1)
    {
        int t = x1;
        x1 = x2;
        x2 = t;
    }
    for (int x = x1; x <= x2; ++x)
        if (in_bounds(x, y))
            mapc[y][x].t = T_FLOOR;
}
static void carve_v_tunnel(int y1, int y2, int x)
{
    if (y2 < y1)
    {
        int t = y1;
        y1 = y2;
        y2 = t;
    }
    for (int y = y1; y <= y2; ++y)
        if (in_bounds(x, y))
            mapc[y][x].t = T_FLOOR;
}

static void clear_mobs_items(void)
{
    for (int i = 0; i < MAX_MOBS; i++)
        mobs[i].alive = false;
    for (int i = 0; i < MAX_ITEMS; i++)
        items[i].alive = false;
}
static bool mob_at(int x, int y, int *idx)
{
    for (int i = 0; i < MAX_MOBS; i++)
        if (mobs[i].alive && mobs[i].x == x && mobs[i].y == y)
        {
            if (idx)
                *idx = i;
            return true;
        }
    return false;
}
static bool item_at(int x, int y, int *idx)
{
    for (int i = 0; i < MAX_ITEMS; i++)
        if (items[i].alive && items[i].x == x && items[i].y == y)
        {
            if (idx)
                *idx = i;
            return true;
        }
    return false;
}
static bool is_walkable_free(int x, int y)
{
    if (is_blocking(x, y))
        return false;
    if (mob_at(x, y, NULL))
        return false;
    return true;
}

static void add_mob(MobType t, int x, int y)
{
    for (int i = 0; i < MAX_MOBS; i++)
        if (!mobs[i].alive)
        {
            mobs[i].alive = true;
            mobs[i].type = t;
            mobs[i].x = x;
            mobs[i].y = y;
            if (t == MOB_GOBLIN)
            {
                mobs[i].hpmax = 6;
                mobs[i].hp = 6;
                mobs[i].ch = 'g';
                mobs[i].name = "goblin";
            }
            else
            {
                mobs[i].hpmax = 10;
                mobs[i].hp = 10;
                mobs[i].ch = 'o';
                mobs[i].name = "orc";
            }
            return;
        }
}
static void add_item(ItemType t, int x, int y)
{
    for (int i = 0; i < MAX_ITEMS; i++)
        if (!items[i].alive)
        {
            items[i].alive = true;
            items[i].x = x;
            items[i].y = y;
            if (t == IT_POTION)
            {
                items[i].type = t;
                items[i].ch = '!';
                items[i].name = "potion";
            }
            else
            {
                items[i].type = t;
                items[i].ch = '%';
                items[i].name = "ration";
            }
            return;
        }
}

static void gen_floor(int floor)
{
    /* walls everywhere */
    for (int y = 0; y < MAP_H - 1; y++)
    {
        for (int x = 0; x < MAP_W; x++)
        {
            mapc[y][x].t = T_WALL;
            mapc[y][x].seen = mapc[y][x].vis = false;
        }
    }
    nrooms = 0;
    int attempts = 0;
    while (nrooms < MAX_ROOMS && attempts < 2000)
    {
        attempts++;
        Rect r;
        r.w = irand(4, 12);
        r.h = irand(3, 7);
        r.x = irand(1, MAP_W - r.w - 2);
        r.y = irand(1, (MAP_H - 2) - r.h - 1);
        bool ok = true;
        for (int i = 0; i < nrooms; i++)
        {
            if (rect_overlap(r, rooms[i]))
            {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;
        carve_rect(r);
        if (nrooms > 0)
        {
            /* connect to previous */
            Rect p = rooms[nrooms - 1];
            int x1 = r.x + r.w / 2;
            int y1 = r.y + r.h / 2;
            int x2 = p.x + p.w / 2;
            int y2 = p.y + p.h / 2;
            if (rand() % 2)
            {
                carve_h_tunnel(x1, x2, y1);
                carve_v_tunnel(y1, y2, x2);
            }
            else
            {
                carve_v_tunnel(y1, y2, x1);
                carve_h_tunnel(x1, x2, y2);
            }
        }
        rooms[nrooms++] = r;
    }
    /* place stairs */
    Rect last = rooms[nrooms - 1];
    stairs_x = last.x + last.w / 2;
    stairs_y = last.y + last.h / 2;
    mapc[stairs_y][stairs_x].t = (floor < FLOORS ? T_STAIR : T_FLOOR);

    /* place amulet on final floor */
    if (floor == FLOORS)
    {
        /* choose another room */
        Rect r = rooms[irand(0, nrooms - 1)];
        int ax = r.x + irand(1, r.w - 2);
        int ay = r.y + irand(1, r.h - 2);
        mapc[ay][ax].t = T_AMULET;
    }

    /* place player in first room for new floor */
    place_player_in_rect(rooms[0]);

    /* monsters & items */
    clear_mobs_items();
    int nm = irand(6, 12);
    for (int i = 0; i < nm; i++)
    {
        Rect r = rooms[irand(0, nrooms - 1)];
        int x = r.x + irand(1, r.w - 2);
        int y = r.y + irand(1, r.h - 2);
        if ((x == pl.x && y == pl.y) || (x == stairs_x && y == stairs_y))
        {
            i--;
            continue;
        }
        if (mob_at(x, y, NULL))
        {
            i--;
            continue;
        }
        add_mob((rand() % 3) ? MOB_GOBLIN : MOB_ORC, x, y);
    }
    int ni = irand(4, 8);
    for (int i = 0; i < ni; i++)
    {
        Rect r = rooms[irand(0, nrooms - 1)];
        int x = r.x + irand(1, r.w - 2);
        int y = r.y + irand(1, r.h - 2);
        if ((x == pl.x && y == pl.y) || mob_at(x, y, NULL))
        {
            i--;
            continue;
        }
        add_item((rand() % 2) ? IT_POTION : IT_RATION, x, y);
    }
}

/* ---------- FOV (simple ray casting) ---------- */
static void set_visibility(bool v)
{
    for (int y = 0; y < MAP_H - 1; y++)
        for (int x = 0; x < MAP_W; x++)
            mapc[y][x].vis = v;
}

/* Bresenham line; returns whether fully visible to end */
static bool los(int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    int x = x0, y = y0;
    while (1)
    {
        if (x == x1 && y == y1)
            return true;
        if (!(x == x0 && y == y0) && is_opaque(x, y))
            return false;
        e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y += sy;
        }
    }
}

static void compute_fov(void)
{
    set_visibility(false);
    for (int y = pl.y - VIEW_R; y <= pl.y + VIEW_R; ++y)
    {
        for (int x = pl.x - VIEW_R; x <= pl.x + VIEW_R; ++x)
        {
            if (!in_bounds(x, y))
                continue;
            int dx = x - pl.x, dy = y - pl.y;
            if (dx * dx + dy * dy <= VIEW_R * VIEW_R)
            {
                if (los(pl.x, pl.y, x, y))
                {
                    mapc[y][x].vis = true;
                    mapc[y][x].seen = true;
                }
            }
        }
    }
}

/* ---------- Rendering ---------- */
static void draw_map(void)
{
    for (int y = 0; y < MAP_H - 1; y++)
    {
        term_move(y + 1, 1);
        for (int x = 0; x < MAP_W; x++)
        {
            char ch = ' ';
            Tile *t = &mapc[y][x];
            if (!t->seen)
            {
                ch = ' ';
            }
            else
            {
                if (t->vis)
                {
                    switch (t->t)
                    {
                    case T_WALL:
                        ch = '#';
                        break;
                    case T_FLOOR:
                        ch = '.';
                        break;
                    case T_STAIR:
                        ch = '>';
                        break;
                    case T_AMULET:
                        ch = '*';
                        break;
                    }
                }
                else
                {
                    /* memory (dim) */
                    switch (t->t)
                    {
                    case T_WALL:
                        ch = '#';
                        break;
                    case T_FLOOR:
                        ch = '.';
                        break;
                    case T_STAIR:
                        ch = '>';
                        break;
                    case T_AMULET:
                        ch = '.';
                        break; /* hidden if not in view */
                    }
                }
            }
            putchar(ch);
        }
    }
    /* Items */
    for (int i = 0; i < MAX_ITEMS; i++)
        if (items[i].alive)
        {
            if (mapc[items[i].y][items[i].x].vis)
            {
                term_move(items[i].y + 1, items[i].x + 1);
                putchar(items[i].ch);
            }
        }
    /* Mobs */
    for (int i = 0; i < MAX_MOBS; i++)
        if (mobs[i].alive)
        {
            if (mapc[mobs[i].y][mobs[i].x].vis)
            {
                term_move(mobs[i].y + 1, mobs[i].x + 1);
                putchar(mobs[i].ch);
            }
        }
    /* Player */
    term_move(pl.y + 1, pl.x + 1);
    putchar('@');
}

/* ---------- Status line ---------- */
static char status_msg[MAP_W + 1] = "";
static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_msg, MAP_W, fmt, ap);
    va_end(ap);
}
static void draw_status(void)
{
    term_move(MAP_H, 1);
    for (int i = 0; i < MAP_W; i++)
        putchar(' ');
    term_move(MAP_H, 1);
    printf("HP %d/%d  Food %d  Floor %d/%d  %s%s",
           pl.hp, pl.hpmax, pl.hunger, pl.floor, FLOORS,
           pl.have_amulet ? "Amulet:Yes " : "",
           status_msg);
}

/* ---------- Combat & Items ---------- */
static void attack_player(int i)
{
    int dmg = irand(1, 3) + (mobs[i].type == MOB_ORC ? 1 : 0);
    pl.hp -= dmg;
    set_msg("The %s hits you for %d!", mobs[i].name, dmg);
}
static void attack_mob(int i)
{
    int dmg = irand(1, 4);
    mobs[i].hp -= dmg;
    if (mobs[i].hp <= 0)
    {
        mobs[i].alive = false;
        set_msg("You slay the %s.", mobs[i].name);
    }
    else
        set_msg("You hit the %s for %d.", mobs[i].name, dmg);
}
static void pickup_if_any(void)
{
    int idx;
    if (item_at(pl.x, pl.y, &idx))
    {
        if (items[idx].type == IT_POTION)
        {
            int heal = irand(4, 8);
            pl.hp = clampi(pl.hp + heal, 0, pl.hpmax);
            set_msg("You quaff a potion and heal %d.", heal);
        }
        else
        {
            int feed = irand(200, 350);
            pl.hunger += feed;
            if (pl.hunger > 1200)
                pl.hunger = 1200;
            set_msg("You eat a ration (+%d food).", feed);
        }
        items[idx].alive = false;
    }
    /* Amulet */
    if (mapc[pl.y][pl.x].t == T_AMULET)
    {
        pl.have_amulet = true;
        mapc[pl.y][pl.x].t = T_FLOOR;
        set_msg("You pick up the Amulet of Yendor!");
    }
}

/* ---------- Monster AI ---------- */
static int signi(int v) { return (v > 0) - (v < 0); }

static void mobs_turn(void)
{
    for (int i = 0; i < MAX_MOBS; i++)
        if (mobs[i].alive)
        {
            /* If adjacent to player, attack */
            int dx = pl.x - mobs[i].x;
            int dy = pl.y - mobs[i].y;
            if (abs(dx) <= 1 && abs(dy) <= 1)
            {
                attack_player(i);
                continue;
            }
            /* Move toward player if in FOV & somewhat close, else wander */
            int step_x = 0, step_y = 0;
            if (mapc[mobs[i].y][mobs[i].x].vis && (dx * dx + dy * dy) <= (VIEW_R * VIEW_R))
            {
                step_x = signi(dx);
                step_y = signi(dy);
                /* try axis-aligned if blocked */
                if (!is_walkable_free(mobs[i].x + step_x, mobs[i].y + step_y))
                {
                    if (abs(dx) > abs(dy))
                    {
                        if (is_walkable_free(mobs[i].x + step_x, mobs[i].y))
                        {
                            step_y = 0;
                        }
                        else if (is_walkable_free(mobs[i].x, mobs[i].y + step_y))
                        {
                            step_x = 0;
                        }
                        else
                        {
                            step_x = (rand() % 3) - 1;
                            step_y = (rand() % 3) - 1;
                        }
                    }
                    else
                    {
                        if (is_walkable_free(mobs[i].x, mobs[i].y + step_y))
                        {
                            step_x = 0;
                        }
                        else if (is_walkable_free(mobs[i].x + step_x, mobs[i].y))
                        {
                            step_y = 0;
                        }
                        else
                        {
                            step_x = (rand() % 3) - 1;
                            step_y = (rand() % 3) - 1;
                        }
                    }
                }
            }
            else
            {
                step_x = (rand() % 3) - 1;
                step_y = (rand() % 3) - 1;
            }
            int nx = mobs[i].x + clampi(step_x, -1, 1);
            int ny = mobs[i].y + clampi(step_y, -1, 1);
            if (in_bounds(nx, ny) && is_walkable_free(nx, ny))
            {
                mobs[i].x = nx;
                mobs[i].y = ny;
            }
        }
}

/* ---------- Player movement ---------- */
static void try_move_player(int dx, int dy)
{
    int nx = clampi(pl.x + dx, 0, MAP_W - 1);
    int ny = clampi(pl.y + dy, 0, MAP_H - 2);
    if (nx == pl.x && ny == pl.y)
        return;
    if (is_blocking(nx, ny))
    {
        set_msg("You bump into a wall.");
        return;
    }
    int mi;
    if (mob_at(nx, ny, &mi))
    {
        attack_mob(mi);
        return;
    }
    pl.x = nx;
    pl.y = ny;
    pickup_if_any();
}

/* ---------- Floor transitions ---------- */
static bool descend_if_possible(void)
{
    if (mapc[pl.y][pl.x].t == T_STAIR)
    {
        pl.floor++;
        set_msg("You descend to floor %d.", pl.floor);
        gen_floor(pl.floor);
        compute_fov();
        return true;
    }
    else
    {
        set_msg("No stairs here.");
        return false;
    }
}

/* ---------- Main loop ---------- */
static void game_over_screen(const char *msg)
{
    term_move(MAP_H / 2, (MAP_W - (int)strlen(msg)) / 2);
    printf("%s", msg);
    term_move(MAP_H, 1);
    term_show_cursor();
    fflush(stdout);
#if defined(_WIN32)
    restore_vt();
#else
    restore_term();
#endif
}

int main(void)
{
    srand((unsigned)time(NULL));

#if defined(_WIN32)
    enable_vt();
#else
    set_raw();
#endif
    atexit(term_show_cursor);
#if defined(_WIN32)
    atexit(restore_vt);
#else
    atexit(restore_term);
#endif

    term_hide_cursor();
    term_clear();

    /* init player */
    pl.hpmax = 20;
    pl.hp = 20;
    pl.hunger = 800;
    pl.floor = 1;
    pl.have_amulet = false;

    gen_floor(pl.floor);
    compute_fov();
    pickup_if_any();
    set_msg("Find the stairs (>). Floor %d/%d", pl.floor, FLOORS);

    bool running = true;
    while (running)
    {
        compute_fov();
        term_clear();
        draw_map();
        draw_status();
        fflush(stdout);

        /* starvation / regen */
        pl.hunger -= 1;
        if (pl.hunger <= 0)
        {
            pl.hunger = 0;
            if (rand() % 3 == 0)
            {
                pl.hp -= 1;
                set_msg("You are starving!");
            }
        }
        else if (pl.hunger > 600 && rand() % 9 == 0)
        {
            pl.hp = clampi(pl.hp + 1, 0, pl.hpmax);
        }

        if (pl.hp <= 0)
        {
            game_over_screen("You died... Game Over.");
            return 0;
        }
        if (pl.have_amulet)
        {
            game_over_screen("You escape with the Amulet! You win!");
            return 0;
        }

        int key = read_key();

        switch (key)
        {
        case KEY_UP:
            try_move_player(0, -1);
            break;
        case KEY_DOWN:
            try_move_player(0, 1);
            break;
        case KEY_LEFT:
            try_move_player(-1, 0);
            break;
        case KEY_RIGHT:
            try_move_player(1, 0);
            break;
        case KEY_Y:
            try_move_player(-1, -1);
            break;
        case KEY_U:
            try_move_player(1, -1);
            break;
        case KEY_B:
            try_move_player(-1, 1);
            break;
        case KEY_N:
            try_move_player(1, 1);
            break;
        case KEY_CHAR:
            if (g_last_char == '>')
            {
                if (pl.floor == FLOORS)
                {
                    set_msg("No deeper stairs... find the Amulet (*)!");
                }
                else
                    descend_if_possible();
            }
            else if (g_last_char == 'q')
            {
                running = false;
            }
            break;
        default:
            break;
        }

        /* monster turn */
        mobs_turn();
    }

    game_over_screen("You quit. Bye!");
    return 0;
}
