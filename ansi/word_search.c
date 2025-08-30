/* word_search.c - simple terminal word search game (C99)
   Features:
   - 12x12 grid with up to 20 words
   - Words hidden in 8 directions (including reverse)
   - Case-insensitive guessing; found letters shown in lowercase
   - Optional RNG seed via argv to get reproducible puzzles
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define GRID_N 12
#define MAX_WORDS 20
#define MAX_WORD_LEN 20
#define MAX_PLACE_TRIES 5000

typedef struct
{
    int dx, dy;
} Dir;

static const Dir DIRS[8] = {
    {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};

typedef struct
{
    char text[MAX_WORD_LEN + 1];
    int placed; /* 1 if successfully placed */
    int found;  /* 1 if user has found it */
} Word;

/* Default word list — you can edit these. All letters only, no spaces. */
static const char *DEFAULT_WORDS[] = {
    "EMBEDDED", "PYTHON", "CIRCUIT", "SENSOR", "MOTOR",
    "KERNEL", "THREAD", "VECTOR", "MEMORY", "DISPLAY",
    "BUTTON", "CANBUS", "PWM", "FOC", "DEBUG",
    "DRIVER", "SAFETY", "SYSTEM", "LOGIC", "ALPHA"};
static const int DEFAULT_WORD_COUNT = 20;

/* Utility: uppercase letters of s in place, remove non A-Z if any */
static void sanitize_word(char *s)
{
    size_t w = 0, n = strlen(s);
    for (size_t i = 0; i < n; ++i)
    {
        if (isalpha((unsigned char)s[i]))
        {
            s[w++] = (char)toupper((unsigned char)s[i]);
        }
    }
    s[w] = '\0';
}

/* Check if word fits at (x,y) with direction d on grid of size N */
static int can_place(char grid[GRID_N][GRID_N], int N, const char *w, int x, int y, Dir d)
{
    int L = (int)strlen(w);
    int cx = x, cy = y;
    for (int i = 0; i < L; ++i)
    {
        if (cx < 0 || cy < 0 || cx >= N || cy >= N)
            return 0;
        char g = grid[cy][cx];
        if (g != 0 && g != w[i])
            return 0; /* 0 = empty; otherwise must match same letter */
        cx += d.dx;
        cy += d.dy;
    }
    return 1;
}

/* Place word letters onto grid (assumes can_place was true) */
static void do_place(char grid[GRID_N][GRID_N], int N, const char *w, int x, int y, Dir d)
{
    int L = (int)strlen(w);
    int cx = x, cy = y;
    for (int i = 0; i < L; ++i)
    {
        grid[cy][cx] = w[i];
        cx += d.dx;
        cy += d.dy;
    }
}

/* Try to place a word randomly somewhere; returns 1 on success */
static int place_word_random(char grid[GRID_N][GRID_N], int N, const char *w)
{
    int tries = 0;
    while (tries++ < MAX_PLACE_TRIES)
    {
        Dir d = DIRS[rand() % 8];
        int x = rand() % N;
        int y = rand() % N;
        if (can_place(grid, N, w, x, y, d))
        {
            do_place(grid, N, w, x, y, d);
            return 1;
        }
    }
    return 0;
}

/* Fill remaining empty cells with random A..Z letters */
static void fill_random(char grid[GRID_N][GRID_N], int N)
{
    for (int y = 0; y < N; ++y)
    {
        for (int x = 0; x < N; ++x)
        {
            if (grid[y][x] == 0)
            {
                grid[y][x] = 'A' + (rand() % 26);
            }
        }
    }
}

/* Print grid with coordinates; lowercase letters indicate found cells */
static void print_grid(char grid[GRID_N][GRID_N], int N)
{
    printf("\n    ");
    for (int x = 0; x < N; ++x)
        printf("%2d ", x);
    printf("\n   +");
    for (int x = 0; x < N; ++x)
        printf("---");
    printf("+\n");
    for (int y = 0; y < N; ++y)
    {
        printf("%2d |", y);
        for (int x = 0; x < N; ++x)
        {
            char c = grid[y][x];
            printf(" %c ", c ? c : '.');
        }
        printf("|\n");
    }
    printf("   +");
    for (int x = 0; x < N; ++x)
        printf("---");
    printf("+\n");
}

/* Print words with found markers */
static void print_words(Word *words, int count)
{
    printf("\nWords to find (%d):\n", count);
    for (int i = 0; i < count; ++i)
    {
        printf("  [%c] %s\n", words[i].found ? 'X' : ' ', words[i].text);
    }
    printf("\nType a word to mark it found, or commands: :list  :grid  :reveal  :quit\n");
}

/* Compare c (grid char, which may be upper or lower) to target uppercase letter */
static int cmpeq(char grid_c, char up_letter)
{
    if (!isalpha((unsigned char)grid_c))
        return 0;
    return (char)toupper((unsigned char)grid_c) == up_letter;
}

/* Try to match word w starting at (x,y) going in dir d; return length if matches else 0 */
static int match_at(const char grid[GRID_N][GRID_N], int N, const char *w, int x, int y, Dir d)
{
    int L = (int)strlen(w);
    int cx = x, cy = y;
    for (int i = 0; i < L; ++i)
    {
        if (cx < 0 || cy < 0 || cx >= N || cy >= N)
            return 0;
        if (!cmpeq(grid[cy][cx], (char)toupper((unsigned char)w[i])))
            return 0;
        cx += d.dx;
        cy += d.dy;
    }
    return L;
}

/* Lowercase letters along the matched path */
static void lowercase_path(char grid[GRID_N][GRID_N], int x, int y, Dir d, int L)
{
    int cx = x, cy = y;
    for (int i = 0; i < L; ++i)
    {
        if (isalpha((unsigned char)grid[cy][cx]))
        {
            grid[cy][cx] = (char)tolower((unsigned char)grid[cy][cx]);
        }
        cx += d.dx;
        cy += d.dy;
    }
}

/* Search the grid for word (both forward and reverse). If found, lowercase it and return 1 */
static int search_and_mark(char grid[GRID_N][GRID_N], int N, const char *word)
{
    int L = (int)strlen(word);
    if (L <= 0)
        return 0;

    char fwd[MAX_WORD_LEN + 1], rev[MAX_WORD_LEN + 1];
    strncpy(fwd, word, MAX_WORD_LEN);
    fwd[MAX_WORD_LEN] = '\0';
    sanitize_word(fwd);

    /* build reverse */
    for (int i = 0; i < L; ++i)
        rev[i] = fwd[L - 1 - i];
    rev[L] = '\0';

    for (int y = 0; y < N; ++y)
    {
        for (int x = 0; x < N; ++x)
        {
            for (int k = 0; k < 8; ++k)
            {
                int m = match_at((const char (*)[GRID_N])grid, N, fwd, x, y, DIRS[k]);
                if (m)
                {
                    lowercase_path(grid, x, y, DIRS[k], m);
                    return 1;
                }
                int r = match_at((const char (*)[GRID_N])grid, N, rev, x, y, DIRS[k]);
                if (r)
                {
                    lowercase_path(grid, x, y, DIRS[k], r);
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Trim trailing newline from fgets buffer */
static void chomp(char *s)
{
    size_t n = strlen(s);
    if (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[n - 1] = '\0';
}

int main(int argc, char **argv)
{
    unsigned int seed = (unsigned int)time(NULL);
    if (argc >= 2)
    {
        seed = (unsigned int)strtoul(argv[1], NULL, 10);
    }
    srand(seed);

    char grid[GRID_N][GRID_N] = {{0}};
    Word words[MAX_WORDS];
    int word_count = DEFAULT_WORD_COUNT;
    if (word_count > MAX_WORDS)
        word_count = MAX_WORDS;

    /* load default words and sanitize */
    for (int i = 0; i < word_count; ++i)
    {
        strncpy(words[i].text, DEFAULT_WORDS[i], MAX_WORD_LEN);
        words[i].text[MAX_WORD_LEN] = '\0';
        sanitize_word(words[i].text);
        words[i].placed = 0;
        words[i].found = 0;
    }

    /* Randomize word order so long words tend to get a chance */
    for (int i = word_count - 1; i > 0; --i)
    {
        int j = rand() % (i + 1);
        Word tmp = words[i];
        words[i] = words[j];
        words[j] = tmp;
    }

    /* Place words, trying longest ones first helps; do a simple sort by length desc */
    for (int i = 0; i < word_count; ++i)
    {
        for (int j = i + 1; j < word_count; ++j)
        {
            if ((int)strlen(words[j].text) > (int)strlen(words[i].text))
            {
                Word t = words[i];
                words[i] = words[j];
                words[j] = t;
            }
        }
    }

    for (int i = 0; i < word_count; ++i)
    {
        if (place_word_random(grid, GRID_N, words[i].text))
        {
            words[i].placed = 1;
        }
    }
    fill_random(grid, GRID_N);

    /* Sort back alphabetically for display (optional) */
    for (int i = 0; i < word_count; ++i)
    {
        for (int j = i + 1; j < word_count; ++j)
        {
            if (strcmp(words[j].text, words[i].text) < 0)
            {
                Word t = words[i];
                words[i] = words[j];
                words[j] = t;
            }
        }
    }

    printf("WORD SEARCH — %dx%d (seed=%u)\n", GRID_N, GRID_N, seed);
    print_grid(grid, GRID_N);
    print_words(words, word_count);

    char input[256];
    int remaining = 0;
    for (int i = 0; i < word_count; ++i)
        if (words[i].placed)
            remaining++;

    while (1)
    {
        if (remaining <= 0)
        {
            printf("\n🎉 All placed words found! Great job!\n");
            break;
        }

        printf("> ");
        if (!fgets(input, sizeof(input), stdin))
            break;
        chomp(input);
        if (!input[0])
            continue;

        if (input[0] == ':')
        {
            if (strcmp(input, ":quit") == 0 || strcmp(input, ":q") == 0)
            {
                printf("Bye!\n");
                break;
            }
            else if (strcmp(input, ":grid") == 0)
            {
                print_grid(grid, GRID_N);
            }
            else if (strcmp(input, ":list") == 0)
            {
                print_words(words, word_count);
            }
            else if (strcmp(input, ":reveal") == 0)
            {
                /* Mark all placed words as found by searching each one */
                for (int i = 0; i < word_count; ++i)
                {
                    if (words[i].placed && !words[i].found)
                    {
                        if (search_and_mark(grid, GRID_N, words[i].text))
                        {
                            words[i].found = 1;
                        }
                    }
                }
                remaining = 0;
                print_grid(grid, GRID_N);
                print_words(words, word_count);
            }
            else
            {
                printf("Commands: :list  :grid  :reveal  :quit\n");
            }
            continue;
        }

        /* Treat input as a guessed word */
        char guess[MAX_WORD_LEN + 1];
        strncpy(guess, input, MAX_WORD_LEN);
        guess[MAX_WORD_LEN] = '\0';
        sanitize_word(guess);
        if (!guess[0])
        {
            printf("Please enter letters (A–Z).\n");
            continue;
        }

        /* Check it is in our list and not already found/not placed */
        int idx = -1;
        for (int i = 0; i < word_count; ++i)
        {
            if (strcmp(words[i].text, guess) == 0)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0)
        {
            printf("“%s” is not in the word list.\n", input);
            continue;
        }
        if (!words[idx].placed)
        {
            printf("“%s” could not be placed in this puzzle. Try another.\n", words[idx].text);
            continue;
        }
        if (words[idx].found)
        {
            printf("You already found “%s”.\n", words[idx].text);
            continue;
        }

        if (search_and_mark(grid, GRID_N, guess))
        {
            words[idx].found = 1;
            remaining--;
            printf("✅ Found: %s (%d remaining)\n", words[idx].text, remaining);
            print_grid(grid, GRID_N);
        }
        else
        {
            printf("Nope—%s is in the list but I can’t find it there. Keep looking!\n", words[idx].text);
        }
    }

    return 0;
}
