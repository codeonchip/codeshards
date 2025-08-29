/* gostop.c - Text-based Go-Stop (Hwatu) - simplified 1vCPU
 *
 * Build:  cc -O2 -std=c99 -Wall -Wextra -o gostop gostop.c
 *
 * House rules / simplifications:
 * - 2 players (You vs CPU). 10 cards each, 8 on table. Play ends when hands empty.
 * - On a match by month, you capture ALL table cards of that month (no choose-one).
 * - Scoring at end only:
 *      Pi:   max(0, total_pi - 9)   (double-Pi counts as 2)
 *      Tti:  max(0, total_tti - 4)
 *      Yeol: max(0, total_yeol - 4)
 *      Gwang: 3->3 pts, 4->4 pts, 5->15 pts
 * - No mid-round "Go/Stop", no shakes/bombs, no ribbon-color sets.
 *
 * Card kinds:
 *   KWANG, YEOL (animal), TTI (ribbon), PI (junk)
 *
 * Deck mapping approximates common hwatu distribution for playability.
 * This is NOT tournament-accurate; it's a learning/console-friendly version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define DECK_SIZE 48
#define MAX_SET 48

typedef enum
{
    KWANG = 0,
    YEOL = 1,
    TTI = 2,
    PI = 3
} Kind;

typedef struct
{
    int month;     /* 1..12 */
    Kind kind;     /* KWANG, YEOL, TTI, PI */
    int double_pi; /* 1 if counts as two Pi, else 0 */
} Card;

/* ---------- Utility containers as simple arrays ---------- */
typedef struct
{
    int ids[MAX_SET];
    int n;
} Pile;

static Card DECK[DECK_SIZE];

/* ---------- Forward declarations ---------- */
static void build_deck(Card deck[DECK_SIZE]);
static void shuffle_ids(int ids[], int n);
static const char *kind_short(Kind k);
static void print_card(int cid);
static void print_pile_inline(const char *label, const Pile *p, int show_index);
static void print_table_grouped(const Pile *table);
static void add_card(Pile *p, int cid);
static int remove_at(Pile *p, int pos);
static int find_and_remove_month(Pile *table, int month, int removed_ids[], int max_removed);
static int any_match_month(const Pile *table, int month);
static int read_choice(int lo, int hi);
static void deal(const int order[], Pile *phand, Pile *aihand, Pile *table, int *draw_top);
static void resolve_play_and_draw(const char *who, int hand_index, Pile *hand, Pile *table, Pile *captured, const int order[], int *draw_top, int verbose);
static int ai_choose_card(const Pile *hand, const Pile *table);
static void score_captured(const Pile *captured, int *pi_pts, int *tti_pts, int *yeol_pts, int *gwang_pts, int *total_pi, int *total_tti, int *total_yeol, int *total_gwang);

/* ---------- Deck mapping ----------
 * Month layout (simplified, common-ish):
 * 1:  KWANG, TTI,  PI,  PI
 * 2:  YEOL,  TTI,  PI,  PI
 * 3:  KWANG, TTI,  PI,  PI
 * 4:  YEOL,  TTI,  PI,  PI
 * 5:  YEOL,  TTI,  PI,  PI
 * 6:  YEOL,  TTI,  PI,  PI
 * 7:  YEOL,  PI,   PI,  PI
 * 8:  KWANG, YEOL, PI,  PI
 * 9:  YEOL,  TTI,  PI,  PI
 * 10: YEOL,  TTI,  PI,  PI
 * 11: KWANG, YEOL, TTI, PI (rain Pi is double)
 * 12: KWANG, PI,   PI,  PI (one PI is double)
 */
static void build_deck(Card deck[DECK_SIZE])
{
    int idx = 0;
    for (int m = 1; m <= 12; ++m)
    {
        for (int k = 0; k < 4; ++k)
        {
            deck[idx].month = m;
            deck[idx].kind = PI;
            deck[idx].double_pi = 0;
            idx++;
        }
    }
    /* overwrite with intended kinds */
    int i = 0;

    /* Helper to set a month’s four kinds quickly */
    auto void set_month(int month, Kind a, Kind b, Kind c, Kind d)
    {
        int base = (month - 1) * 4;
        deck[base + 0].kind = a;
        deck[base + 1].kind = b;
        deck[base + 2].kind = c;
        deck[base + 3].kind = d;
    };

    set_month(1, KWANG, TTI, PI, PI);
    set_month(2, YEOL, TTI, PI, PI);
    set_month(3, KWANG, TTI, PI, PI);
    set_month(4, YEOL, TTI, PI, PI);
    set_month(5, YEOL, TTI, PI, PI);
    set_month(6, YEOL, TTI, PI, PI);
    set_month(7, YEOL, PI, PI, PI);
    set_month(8, KWANG, YEOL, PI, PI);
    set_month(9, YEOL, TTI, PI, PI);
    set_month(10, YEOL, TTI, PI, PI);
    set_month(11, KWANG, YEOL, TTI, PI);
    set_month(12, KWANG, PI, PI, PI);

    /* Mark double-Pi: 11's Pi, and one of 12's Pi */
    deck[(11 - 1) * 4 + 3].double_pi = 1; /* month 11, slot 4 */
    deck[(12 - 1) * 4 + 2].double_pi = 1; /* month 12, slot 3 */
}

static void shuffle_ids(int ids[], int n)
{
    for (int i = 0; i < n; ++i)
        ids[i] = i;
    for (int i = n - 1; i > 0; --i)
    {
        int j = rand() % (i + 1);
        int t = ids[i];
        ids[i] = ids[j];
        ids[j] = t;
    }
}

static const char *kind_short(Kind k)
{
    switch (k)
    {
    case KWANG:
        return "K";
    case YEOL:
        return "A"; /* Animal */
    case TTI:
        return "R"; /* Ribbon */
    default:
        return "P"; /* Pi */
    }
}

static void print_card(int cid)
{
    Card c = DECK[cid];
    const char *ks = kind_short(c.kind);
    if (c.kind == PI && c.double_pi)
    {
        printf("%02d-P2", c.month);
    }
    else
    {
        printf("%02d-%s ", c.month, ks);
    }
}

static void print_pile_inline(const char *label, const Pile *p, int show_index)
{
    printf("%s", label);
    if (p->n == 0)
    {
        printf("(none)\n");
        return;
    }
    for (int i = 0; i < p->n; ++i)
    {
        if (show_index)
            printf("[%d] ", i + 1);
        print_card(p->ids[i]);
        if (i + 1 < p->n)
            printf("  ");
    }
    printf("\n");
}

static void print_table_grouped(const Pile *table)
{
    /* Group by month for readability */
    int counts[13] = {0};
    for (int i = 0; i < table->n; ++i)
        counts[DECK[table->ids[i]].month]++;
    printf("Table (%d):\n", table->n);
    for (int m = 1; m <= 12; ++m)
    {
        if (!counts[m])
            continue;
        printf("  M%02d: ", m);
        for (int i = 0; i < table->n; ++i)
        {
            int cid = table->ids[i];
            if (DECK[cid].month == m)
            {
                print_card(cid);
                printf("  ");
            }
        }
        printf("\n");
    }
}

static void add_card(Pile *p, int cid)
{
    if (p->n < MAX_SET)
    {
        p->ids[p->n++] = cid;
    }
}

static int remove_at(Pile *p, int pos)
{
    int out = p->ids[pos];
    for (int i = pos; i < p->n - 1; ++i)
        p->ids[i] = p->ids[i + 1];
    p->n--;
    return out;
}

static int find_and_remove_month(Pile *table, int month, int removed_ids[], int max_removed)
{
    int removed = 0;
    for (int i = 0; i < table->n;)
    {
        if (DECK[table->ids[i]].month == month)
        {
            if (removed < max_removed)
                removed_ids[removed++] = table->ids[i];
            remove_at(table, i);
            continue;
        }
        i++;
    }
    return removed;
}

static int any_match_month(const Pile *table, int month)
{
    for (int i = 0; i < table->n; ++i)
        if (DECK[table->ids[i]].month == month)
            return 1;
    return 0;
}

static int read_choice(int lo, int hi)
{
    char buf[64];
    for (;;)
    {
        printf("Choose a card [%d-%d]: ", lo, hi);
        if (!fgets(buf, sizeof(buf), stdin))
            continue;
        /* Trim spaces */
        int allspace = 1;
        for (char *p = buf; *p; ++p)
            if (!isspace((unsigned char)*p))
            {
                allspace = 0;
                break;
            }
        if (allspace)
            continue;
        int v = atoi(buf);
        if (v >= lo && v <= hi)
            return v;
        printf("Invalid input.\n");
    }
}

static void deal(const int order[], Pile *phand, Pile *aihand, Pile *table, int *draw_top)
{
    phand->n = aihand->n = table->n = 0;
    int idx = 0;
    /* 10 each */
    for (int i = 0; i < 10; ++i)
        add_card(phand, order[idx++]);
    for (int i = 0; i < 10; ++i)
        add_card(aihand, order[idx++]);
    /* 8 to table */
    for (int i = 0; i < 8; ++i)
        add_card(table, order[idx++]);
    *draw_top = idx;
}

static void resolve_play_and_draw(const char *who, int hand_index, Pile *hand, Pile *table, Pile *captured, const int order[], int *draw_top, int verbose)
{
    /* 1) Play chosen hand card */
    int pos = hand_index;
    int cid = remove_at(hand, pos);
    int month = DECK[cid].month;

    int removed[MAX_SET];
    int rcount = 0;

    if (any_match_month(table, month))
    {
        /* capture all matching on table + played card */
        rcount = find_and_remove_month(table, month, removed, MAX_SET);
        if (verbose)
        {
            printf("%s plays ", who);
            print_card(cid);
            printf(" and captures %d card(s) on table.\n", rcount);
        }
        for (int i = 0; i < rcount; ++i)
            add_card(captured, removed[i]);
        add_card(captured, cid);
    }
    else
    {
        /* no match: place on table */
        if (verbose)
        {
            printf("%s plays ", who);
            print_card(cid);
            printf(" (no match, placed on table).\n");
        }
        add_card(table, cid);
    }

    /* 2) Draw from deck (if any) and resolve */
    if (*draw_top < DECK_SIZE)
    {
        int draw = order[(*draw_top)++];
        int dmonth = DECK[draw].month;

        if (any_match_month(table, dmonth))
        {
            int rem2[MAX_SET];
            int rc2 = find_and_remove_month(table, dmonth, rem2, MAX_SET);
            if (verbose)
            {
                printf("%s draws ", who);
                print_card(draw);
                printf(" and captures %d card(s) on table.\n", rc2);
            }
            for (int i = 0; i < rc2; ++i)
                add_card(captured, rem2[i]);
            add_card(captured, draw);
        }
        else
        {
            if (verbose)
            {
                printf("%s draws ", who);
                print_card(draw);
                printf(" (no match, placed on table).\n");
            }
            add_card(table, draw);
        }
    }
}

static int ai_choose_card(const Pile *hand, const Pile *table)
{
    /* Prefers any card that matches; among matches, prefer capturing higher kinds. */
    int best_i = -1;
    int best_score = -9999;

    for (int i = 0; i < hand->n; ++i)
    {
        int cid = hand->ids[i];
        int month = DECK[cid].month;

        int immediate = any_match_month(table, month);

        int score = 0;
        if (immediate)
        {
            /* weight by kind; KWANG>YEOL>TTI>PI */
            switch (DECK[cid].kind)
            {
            case KWANG:
                score += 5;
                break;
            case YEOL:
                score += 3;
                break;
            case TTI:
                score += 2;
                break;
            default:
                score += 1;
                break;
            }
            score += 1; /* prefer matching over not */
        }
        else
        {
            /* Prefer discarding low-value when no match */
            score -= (DECK[cid].kind == PI ? 0 : 2);
        }

        if (score > best_score)
        {
            best_score = score;
            best_i = i;
        }
    }
    if (best_i < 0)
        best_i = 0;
    return best_i;
}

static void score_captured(const Pile *captured, int *pi_pts, int *tti_pts, int *yeol_pts, int *gwang_pts,
                           int *total_pi, int *total_tti, int *total_yeol, int *total_gwang)
{
    int pi = 0, tti = 0, yeol = 0, gw = 0;
    for (int i = 0; i < captured->n; ++i)
    {
        Card c = DECK[captured->ids[i]];
        switch (c.kind)
        {
        case KWANG:
            gw++;
            break;
        case YEOL:
            yeol++;
            break;
        case TTI:
            tti++;
            break;
        case PI:
            pi += (c.double_pi ? 2 : 1);
            break;
        }
    }
    int ppts = (pi > 9) ? (pi - 9) : 0;
    int rpts = (tti > 4) ? (tti - 4) : 0;
    int ypts = (yeol > 4) ? (yeol - 4) : 0;
    int gpts = 0;
    if (gw >= 5)
        gpts = 15;
    else if (gw == 4)
        gpts = 4;
    else if (gw == 3)
        gpts = 3;

    *pi_pts = ppts;
    *tti_pts = rpts;
    *yeol_pts = ypts;
    *gwang_pts = gpts;
    *total_pi = pi;
    *total_tti = tti;
    *total_yeol = yeol;
    *total_gwang = gw;
}

int main(void)
{
    srand((unsigned)time(NULL));
    build_deck(DECK);

    int order[DECK_SIZE];
    shuffle_ids(order, DECK_SIZE);

    Pile phand = {.n = 0}, aihand = {.n = 0}, table = {.n = 0};
    Pile pcaps = {.n = 0}, aicaps = {.n = 0};
    int draw_top = 0;

    deal(order, &phand, &aihand, &table, &draw_top);

    printf("=== Go-Stop (simplified) ===\n");
    printf("You (Y) vs CPU (C). 10 cards each, 8 on table. Pair by MONTH.\n");
    printf("Kinds: K=Gwang, A=Animal(Yeol), R=Ribbon(Tti), P=Pi, P2=double-Pi.\n");
    printf("At the end: Pi>9, Tti>4, Yeol>4, Gwang(3/4/5) score points.\n\n");

    /* Decide first player randomly */
    int human_turn = (rand() % 2 == 0);
    printf("First player: %s\n\n", human_turn ? "You" : "CPU");

    while (phand.n > 0 || aihand.n > 0)
    {
        if (human_turn && phand.n > 0)
        {
            print_table_grouped(&table);
            print_pile_inline("Your hand: ", &phand, 1);
            print_pile_inline("Your captures: ", &pcaps, 0);
            printf("\n");

            int choice = read_choice(1, phand.n) - 1;
            resolve_play_and_draw("You", choice, &phand, &table, &pcaps, order, &draw_top, 1);
            printf("\n");
        }
        else if (!human_turn && aihand.n > 0)
        {
            int ai_idx = ai_choose_card(&aihand, &table);
            resolve_play_and_draw("CPU", ai_idx, &aihand, &table, &aicaps, order, &draw_top, 1);
            printf("\n");
        }
        human_turn = !human_turn;
        if (phand.n == 0 && aihand.n == 0)
            break;
    }

    /* Final state */
    printf("=== Round complete ===\n");
    print_table_grouped(&table);
    print_pile_inline("Your captures: ", &pcaps, 0);
    print_pile_inline("CPU captures:  ", &aicaps, 0);
    printf("\n");

    int ypi, ytti, yyeol, ygw, y_pi, y_tti, y_yeol, y_gw;
    int cpi, ctti, cyeol, cgw, c_pi, c_tti, c_yeol, c_gw;

    score_captured(&pcaps, &ypi, &ytti, &yyeol, &ygw, &y_pi, &y_tti, &y_yeol, &y_gw);
    score_captured(&aicaps, &cpi, &ctti, &cyeol, &cgw, &c_pi, &c_tti, &c_yeol, &c_gw);

    int ytotal = ypi + ytti + yyeol + ygw;
    int ctotal = cpi + ctti + cyeol + cgw;

    printf("Your counts:  Pi=%d  Tti=%d  Yeol=%d  Gwang=%d\n", y_pi, y_tti, y_yeol, y_gw);
    printf("Your points:  Pi=%d  Tti=%d  Yeol=%d  Gwang=%d  => TOTAL=%d\n", ypi, ytti, yyeol, ygw, ytotal);
    printf("CPU  counts:  Pi=%d  Tti=%d  Yeol=%d  Gwang=%d\n", c_pi, c_tti, c_yeol, c_gw);
    printf("CPU  points:  Pi=%d  Tti=%d  Yeol=%d  Gwang=%d  => TOTAL=%d\n", cpi, ctti, cyeol, cgw, ctotal);
    printf("\n");

    if (ytotal > ctotal)
        printf("You win!\n");
    else if (ytotal < ctotal)
        printf("CPU wins!\n");
    else
        printf("Tie game!\n");

    printf("\nThanks for playing! (Tip: re-run to reshuffle.)\n");
    return 0;
}
