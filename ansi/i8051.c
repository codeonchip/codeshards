/* i8051.c — Intel 8051 emulator (C99)
 *
 * Goals:
 *  - Load Intel HEX (or run a built-in blink demo).
 *  - Emulate core CPU, 128B IRAM, SFR window (0x80–0xFF), 64KB code.
 *  - Implement a useful subset of instructions (enough for basic apps).
 *  - Print port changes; optional instruction trace.
 *
 * Not a full 8051: no interrupts/timers/serial yet (easy to add later).
 * Instruction timings approximate classic 12 clocks/machine cycle parts.
 *
 * Build:   gcc -std=c99 -O2 -Wall -Wextra i8051.c -o i8051
 * Example: ./i8051 --steps 2000 --mhz 12
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* -------- 8051 core state -------- */
typedef struct
{
    /* Program memory (up to 64 KiB) and internal RAM (128 + SFR window) */
    uint8_t code[65536];
    uint8_t iram[128]; /* 0x00–0x7F */
    uint8_t sfr[128];  /* 0x80–0xFF mapped at index [addr-0x80] */

    /* Registers */
    uint16_t PC;
    uint8_t A, B;
    uint8_t PSW; /* CY=bit7, AC=6, F0=5, RS1=4, RS0=3, OV=2, P=0 */
    uint8_t SP;
    uint16_t DPTR;

    /* Bookkeeping */
    uint64_t instrs; /* instruction count */
    uint64_t clocks; /* clock cycles (12 per machine cycle default) */
    bool trace;
    double mhz;
} Mcu;

/* SFR addresses */
enum
{
    SFR_P0 = 0x80,
    SFR_SP = 0x81,
    SFR_DPL = 0x82,
    SFR_DPH = 0x83,
    SFR_PCON = 0x87,
    SFR_TCON = 0x88,
    SFR_P1 = 0x90,
    SFR_SCON = 0x98,
    SFR_P2 = 0xA0,
    SFR_IE = 0xA8,
    SFR_P3 = 0xB0,
    SFR_IP = 0xB8,
    SFR_PSW = 0xD0,
    SFR_ACC = 0xE0,
    SFR_B = 0xF0
};

/* Helpers to access IRAM/SFR (no XDATA in this tiny core) */
static inline uint8_t read_data(Mcu *m, uint8_t addr)
{
    if (addr < 0x80)
        return m->iram[addr];
    return m->sfr[addr - 0x80];
}
static inline void write_data(Mcu *m, uint8_t addr, uint8_t v)
{
    if (addr < 0x80)
        m->iram[addr] = v;
    else
    {
        /* Side-effects for key SFRs */
        uint8_t old = m->sfr[addr - 0x80];
        m->sfr[addr - 0x80] = v;
        if (addr == SFR_ACC)
            m->A = v;
        else if (addr == SFR_B)
            m->B = v;
        else if (addr == SFR_PSW)
            m->PSW = v;
        else if (addr == SFR_DPL)
            m->DPTR = (m->DPTR & 0xFF00) | v;
        else if (addr == SFR_DPH)
            m->DPTR = (m->DPTR & 0x00FF) | (v << 8);
        else if (addr == SFR_SP)
            m->SP = v;

        /* Print GPIO changes */
        if ((addr == SFR_P0 || addr == SFR_P1 || addr == SFR_P2 || addr == SFR_P3) && old != v)
        {
            int port = (addr - 0x80) / 8; /* rough id */
            fprintf(stderr, "PORT change: P%d = 0x%02X\n",
                    (addr == SFR_P0) ? 0 : (addr == SFR_P1) ? 1
                                       : (addr == SFR_P2)   ? 2
                                                            : 3,
                    v);
        }
    }
}

/* Direct SFR register variables kept coherent */
static void sync_from_sfr(Mcu *m)
{
    m->A = m->sfr[SFR_ACC - 0x80];
    m->B = m->sfr[SFR_B - 0x80];
    m->PSW = m->sfr[SFR_PSW - 0x80];
    m->SP = m->sfr[SFR_SP - 0x80];
    m->DPTR = (m->sfr[SFR_DPH - 0x80] << 8) | m->sfr[SFR_DPL - 0x80];
}
static void sync_to_sfr(Mcu *m)
{
    m->sfr[SFR_ACC - 0x80] = m->A;
    m->sfr[SFR_B - 0x80] = m->B;
    m->sfr[SFR_PSW - 0x80] = m->PSW;
    m->sfr[SFR_SP - 0x80] = m->SP;
    m->sfr[SFR_DPL - 0x80] = (uint8_t)(m->DPTR & 0xFF);
    m->sfr[SFR_DPH - 0x80] = (uint8_t)(m->DPTR >> 8);
}

/* Bits in PSW */
#define PSW_P (1u << 0)
#define PSW_OV (1u << 2)
#define PSW_RS0 (1u << 3)
#define PSW_RS1 (1u << 4)
#define PSW_AC (1u << 6)
#define PSW_CY (1u << 7)

/* Register bank: R0..R7 at 0x00, 0x08, 0x10, 0x18 based on RS1:RS0 */
static inline uint8_t bank_base(const Mcu *m)
{
    return ((m->PSW >> 3) & 0x03) * 8;
}
static inline uint8_t *reg_ptr(Mcu *m, int r)
{
    return &m->iram[bank_base(m) + (r & 7)];
}

/* Fetch helpers */
static inline uint8_t fetch8(Mcu *m) { return m->code[m->PC++]; }
static inline uint16_t fetch16(Mcu *m)
{
    uint16_t hi = fetch8(m);
    uint16_t lo = fetch8(m);
    return (hi << 8) | lo;
}

/* Parity flag recompute (odd parity of ACC) */
static inline void set_parity(Mcu *m)
{
    uint8_t a = m->A;
    int ones = 0;
    for (int i = 0; i < 8; i++)
        ones += ((a >> i) & 1);
    if (ones & 1)
        m->PSW |= PSW_P;
    else
        m->PSW &= ~PSW_P;
}

/* Stack push/pop */
static inline void push(Mcu *m, uint8_t v)
{
    m->SP++;
    write_data(m, m->SP, v);
}
static inline uint8_t pop(Mcu *m)
{
    uint8_t v = read_data(m, m->SP);
    m->SP--;
    return v;
}

/* Relatively accurate machine cycles for a few opcodes (rough) */
static int cycles_lookup(uint8_t op)
{
    switch (op)
    {
    case 0x00:
        return 1; /* NOP */
    case 0x02:
    case 0x12:
        return 2; /* LJMP/LCALL */
    case 0x22:
    case 0x32:
        return 2; /* RET/RETI */
    case 0x80:
        return 2; /* SJMP */
    case 0x90:
        return 2; /* MOV DPTR,#imm16 */
    case 0x75:
    case 0x74:
        return 2; /* MOV direct/#, MOV A,# */
    case 0x78 ... 0x7F:
        return 2; /* MOV Rn,# */
    case 0xE4:
    case 0xF4:
        return 1; /* CLR A, CPL A */
    case 0xA3:
        return 2; /* INC DPTR */
    case 0xE5:
    case 0xF5:
        return 2; /* MOV A,dir / MOV dir,A */
    case 0xD5:
        return 2; /* DJNZ dir,rel */
    case 0xD8 ... 0xDF:
        return 2; /* DJNZ Rn,rel */
    case 0x30:
    case 0x20:
    case 0xB2:
    case 0xC2:
    case 0xD2:
        return 2;
    case 0x04:
    case 0x14:
    case 0x24:
    case 0x94:
        return 1;
    default:
        return 2; /* default guess */
    }
}

/* Intel HEX loader (very small; supports types 00,01,04) */
static bool load_hex(Mcu *m, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[1024];
    uint32_t base = 0;
    while (fgets(line, sizeof line, f))
    {
        size_t n = strlen(line);
        if (n < 11 || line[0] != ':')
        {
            fclose(f);
            return false;
        }
        unsigned cnt, addr, type;
        unsigned chk = 0;
        sscanf(line + 1, "%2x%4x%2x", &cnt, &addr, &type);
        chk = cnt + (addr >> 8) + (addr & 0xFF) + type;
        unsigned pos = 9;
        for (unsigned i = 0; i < cnt; i++)
        {
            unsigned byte;
            sscanf(line + pos, "%2x", &byte);
            pos += 2;
            uint32_t a = base + addr + i;
            if (a < 65536)
                m->code[a] = (uint8_t)byte;
            chk += byte;
        }
        unsigned filechk;
        sscanf(line + pos, "%2x", &filechk);
        chk = ((~chk + 1) & 0xFF);
        if (filechk != chk)
        {
            fclose(f);
            fprintf(stderr, "HEX checksum error\n");
            return false;
        }
        if (type == 0x01)
            break; /* EOF */
        if (type == 0x04)
        { /* ext linear addr */
            unsigned hi;
            sscanf(line + 9, "%4x", &hi);
            base = ((uint32_t)hi) << 16;
        }
    }
    fclose(f);
    return true;
}

/* Tiny printf for tracing */
static void tlog(Mcu *m, const char *fmt, ...)
{
    if (!m->trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Execute one instruction; return false on unimplemented opcode */
static bool step(Mcu *m)
{
    sync_from_sfr(m);
    uint16_t pc0 = m->PC;
    uint8_t op = fetch8(m);
    int cyc = cycles_lookup(op);

    switch (op)
    {
    /* --- Core control flow --- */
    case 0x00: /* NOP */
        break;
    case 0x02:
    { /* LJMP addr16 */
        uint16_t a = fetch16(m);
        m->PC = a;
    }
    break;
    case 0x12:
    { /* LCALL addr16 */
        uint16_t a = fetch16(m);
        push(m, (uint8_t)((m->PC >> 8) & 0xFF));
        push(m, (uint8_t)(m->PC & 0xFF));
        m->PC = a;
    }
    break;
    case 0x22:
    { /* RET */
        uint8_t lo = pop(m), hi = pop(m);
        m->PC = ((uint16_t)hi << 8) | lo;
    }
    break;
    case 0x32:
    { /* RETI (no interrupts yet; treat like RET) */
        uint8_t lo = pop(m), hi = pop(m);
        m->PC = ((uint16_t)hi << 8) | lo;
    }
    break;
    case 0x80:
    { /* SJMP rel */
        int8_t rel = (int8_t)fetch8(m);
        m->PC = (uint16_t)(m->PC + rel);
    }
    break;

    /* --- MOV immediate/direct/regs --- */
    case 0x90:
    { /* MOV DPTR,#imm16 */
        uint16_t imm = fetch16(m);
        m->DPTR = imm;
    }
    break;
    case 0x74:
    { /* MOV A,#imm */
        m->A = fetch8(m);
        set_parity(m);
    }
    break;
    case 0x75:
    { /* MOV direct,#imm */
        uint8_t d = fetch8(m), imm = fetch8(m);
        write_data(m, d, imm);
    }
    break;
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
    case 0x7F:
    { /* MOV Rn,#imm */
        uint8_t imm = fetch8(m);
        *reg_ptr(m, op & 7) = imm;
    }
    break;
    case 0xE5:
    { /* MOV A, direct */
        uint8_t d = fetch8(m);
        m->A = read_data(m, d);
        set_parity(m);
    }
    break;
    case 0xF5:
    { /* MOV direct, A */
        uint8_t d = fetch8(m);
        write_data(m, d, m->A);
    }
    break;

    /* --- INC/DEC DJNZ --- */
    case 0x04:
        m->A++;
        set_parity(m);
        break; /* INC A */
    case 0x14:
        m->A--;
        set_parity(m);
        break; /* DEC A */
    case 0x05:
    { /* INC direct */
        uint8_t d = fetch8(m);
        uint8_t v = read_data(m, d) + 1;
        write_data(m, d, v);
    }
    break;
    case 0x15:
    { /* DEC direct */
        uint8_t d = fetch8(m);
        uint8_t v = read_data(m, d) - 1;
        write_data(m, d, v);
    }
    break;
    case 0xD5:
    { /* DJNZ direct, rel */
        uint8_t d = fetch8(m);
        int8_t rel = (int8_t)fetch8(m);
        uint8_t v = read_data(m, d) - 1;
        write_data(m, d, v);
        if (v != 0)
            m->PC = (uint16_t)(m->PC + rel);
    }
    break;
    case 0xD8:
    case 0xD9:
    case 0xDA:
    case 0xDB:
    case 0xDC:
    case 0xDD:
    case 0xDE:
    case 0xDF:
    { /* DJNZ Rn, rel */
        int r = op & 7;
        int8_t rel = (int8_t)fetch8(m);
        uint8_t *pr = reg_ptr(m, r);
        (*pr)--;
        if (*pr != 0)
            m->PC = (uint16_t)(m->PC + rel);
    }
    break;

    /* --- Arithmetic --- */
    case 0x24:
    { /* ADD A,#imm */
        uint8_t imm = fetch8(m);
        uint16_t sum = (uint16_t)m->A + imm;
        /* AC & OV crude calc */
        m->PSW = (m->PSW & ~(PSW_CY | PSW_AC | PSW_OV));
        if (((m->A & 0x0F) + (imm & 0x0F)) > 0x0F)
            m->PSW |= PSW_AC;
        if (sum & 0x100)
            m->PSW |= PSW_CY;
        uint8_t res = (uint8_t)sum;
        if (((m->A ^ imm ^ 0x80) & (m->A ^ res) & 0x80))
            m->PSW |= PSW_OV;
        m->A = res;
        set_parity(m);
    }
    break;
    case 0x94:
    { /* SUBB A,#imm */
        uint8_t imm = fetch8(m);
        uint8_t cy = (m->PSW & PSW_CY) ? 1 : 0;
        uint16_t diff = (uint16_t)m->A - imm - cy;
        m->PSW &= ~(PSW_CY | PSW_AC | PSW_OV);
        if (((m->A & 0x0F) - (imm & 0x0F) - cy) & 0x10)
            m->PSW |= PSW_AC;
        if (diff & 0x100)
            m->PSW |= PSW_CY;
        uint8_t res = (uint8_t)diff;
        if (((m->A ^ imm) & (m->A ^ res) & 0x80))
            m->PSW |= PSW_OV;
        m->A = res;
        set_parity(m);
    }
    break;

    /* --- A / bit ops --- */
    case 0xE4:
        m->A = 0;
        set_parity(m);
        break; /* CLR A */
    case 0xF4:
        m->A = ~m->A;
        set_parity(m);
        break; /* CPL A */

    /* Bit-addressed SFR bits (0x80–0xFF only) */
    case 0xC2:
    { /* CLR bit */
        uint8_t bitaddr = fetch8(m);
        uint8_t addr = (bitaddr & 0xF8); /* byte address */
        uint8_t b = bitaddr & 7;
        uint8_t v = read_data(m, addr);
        v &= ~(1u << b);
        write_data(m, addr, v);
    }
    break;
    case 0xD2:
    { /* SETB bit */
        uint8_t bitaddr = fetch8(m);
        uint8_t addr = (bitaddr & 0xF8);
        uint8_t b = bitaddr & 7;
        uint8_t v = read_data(m, addr);
        v |= (1u << b);
        write_data(m, addr, v);
    }
    break;
    case 0xB2:
    { /* CPL bit */
        uint8_t bitaddr = fetch8(m);
        uint8_t addr = (bitaddr & 0xF8);
        uint8_t b = bitaddr & 7;
        uint8_t v = read_data(m, addr);
        v ^= (1u << b);
        write_data(m, addr, v);
    }
    break;

    /* --- Conditional branches on bit --- */
    case 0x20:
    { /* JB bit, rel */
        uint8_t bitaddr = fetch8(m);
        int8_t rel = (int8_t)fetch8(m);
        uint8_t v = (read_data(m, (bitaddr & 0xF8)) >> (bitaddr & 7)) & 1u;
        if (v)
            m->PC = (uint16_t)(m->PC + rel);
    }
    break;
    case 0x30:
    { /* JNB bit, rel */
        uint8_t bitaddr = fetch8(m);
        int8_t rel = (int8_t)fetch8(m);
        uint8_t v = (read_data(m, (bitaddr & 0xF8)) >> (bitaddr & 7)) & 1u;
        if (!v)
            m->PC = (uint16_t)(m->PC + rel);
    }
    break;

    /* Unimplemented — stop cleanly so you know what to add next */
    default:
        fprintf(stderr, "Unimplemented opcode 0x%02X at 0x%04X\n", op, pc0);
        return false;
    }

    m->instrs++;
    m->clocks += (uint64_t)(cyc * 12); /* classic parts: 12 clocks per mach cycle */
    sync_to_sfr(m);

    if (m->trace)
    {
        fprintf(stderr, "PC=%04X OP=%02X  A=%02X B=%02X PSW=%02X SP=%02X DPTR=%04X\n",
                pc0, op, m->A, m->B, m->PSW, m->SP, m->DPTR);
    }
    return true;
}

/* Initialize reset state */
static void reset(Mcu *m)
{
    memset(m, 0, sizeof *m);
    m->SP = 0x07;
    m->PC = 0x0000;
    m->mhz = 12.0;
    /* Put registers into SFR mirror */
    sync_to_sfr(m);
}

/* Built-in blink demo (assembled bytes):
   P1.0 toggling with two nested DJNZ loops as delay.
   Equivalent assembly (rough):
        MOV P1,#0FFh
   L1:  CPL P1.0
        MOV R7,#200
   L2:  MOV R6,#250
   L3:  DJNZ R6,L3
        DJNZ R7,L2
        SJMP L1
*/
static void load_builtin_blink(Mcu *m)
{
    uint8_t prog[] = {
        0x75, 0x90, 0xFF,    /* MOV P1,#0xFF */
        0xB2, 0x90,          /* CPL P1.0 (bit addr 0x90) */
        0x7F, 200,           /* MOV R7,#200 */
        0x7E, 250,           /* MOV R6,#250 */
        0xDE, (uint8_t)(-2), /* DJNZ R6, -2 (to same MOV R6,#...) -> simple delay loop */
        0xDF, (uint8_t)(-5), /* DJNZ R7, -5 (reload R6 and loop) */
        0x80, (uint8_t)(-11) /* SJMP to CPL P1.0 */
    };
    memcpy(m->code, prog, sizeof prog);
}

/* CLI */
static void usage(const char *p)
{
    fprintf(stderr,
            "Usage: %s [--hex file.hex] [--steps N] [--mhz F] [--trace]\n"
            "If no --hex provided, runs a built-in P1.0 blink program.\n",
            p);
}

int main(int argc, char **argv)
{
    Mcu m;
    reset(&m);
    const char *hex = NULL;
    uint64_t steps = 2000;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--hex") && i + 1 < argc)
            hex = argv[++i];
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc)
            steps = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--mhz") && i + 1 < argc)
            m.mhz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--trace"))
            m.trace = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (hex)
    {
        if (!load_hex(&m, hex))
        {
            fprintf(stderr, "Failed to load HEX: %s\n", hex);
            return 1;
        }
        fprintf(stderr, "Loaded HEX: %s\n", hex);
    }
    else
    {
        load_builtin_blink(&m);
        fprintf(stderr, "No HEX given; running built-in P1.0 blink demo.\n");
    }

    /* Ensure SFR mirrors reflect core registers */
    write_data(&m, SFR_PSW, m.PSW);
    write_data(&m, SFR_SP, m.SP);
    write_data(&m, SFR_DPL, (uint8_t)(m.DPTR & 0xFF));
    write_data(&m, SFR_DPH, (uint8_t)(m.DPTR >> 8));
    write_data(&m, SFR_ACC, m.A);
    write_data(&m, SFR_B, m.B);

    for (uint64_t i = 0; i < steps; i++)
    {
        if (!step(&m))
            break;
    }

    double seconds = (m.clocks / (m.mhz * 1e6));
    fprintf(stderr, "Executed %llu instructions, ~%llu clocks (%.6f s @ %.3f MHz)\n",
            (unsigned long long)m.instrs, (unsigned long long)m.clocks, seconds, m.mhz);

    return 0;
}
