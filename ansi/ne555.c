/* ne555.c  —  NE555 timer simulator (astable & monostable)
 *
 * - Exact event-driven thresholds at 1/3 and 2/3 VCC (configurable).
 * - Emits uniform CSV samples (time, Vcap, Vout, state, discharge_on).
 * - Prints analytic timing (period, duty, pulse width) to stderr.
 *
 * Compile:  gcc -std=c99 -O2 -lm ne555.c -o ne555
 *
 * Quick examples:
 *   ./ne555 --mode astable --vcc 5 --ra 1000 --rb 1000 --c 1e-7 --T 0.005 --dt 2e-6 > astable.csv
 *   ./ne555 --mode mono    --r 10000 --c 1e-5 --vcc 5 --trig 0.001 --trigw 1e-4 --T 0.06 --dt 1e-4 > mono.csv
 *
 * Notes:
 * - Default thresholds are 1/3 and 2/3 VCC; override via --lofrac / --hifrac.
 * - Output HIGH ~ VCC, LOW ~ 0V. Monostable discharge is clamped to 0V (idealized).
 * - For astable, initial state is HIGH charging from 1/3 VCC by default.
 * - CSV header: time_s,vcap_v,vout_v,state(discrete),discharge_on(0/1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

typedef enum
{
    MODE_ASTABLE,
    MODE_MONO
} Mode;

typedef struct
{
    Mode mode;
    double Vcc;
    double C;

    /* Astable params */
    double RA, RB;

    /* Monostable params */
    double R;

    /* Threshold fractions (defaults 1/3, 2/3) */
    double lofrac, hifrac;

    /* Simulation control */
    double T_end, dt;

    /* Initials (astable) */
    double Vc_init;
    int start_high; /* 1 = start HIGH/charging, 0 = start LOW/discharging */

    /* Monostable trigger */
    double trig_time;  /* seconds */
    double trig_width; /* seconds */

} SimCfg;

/* ------------ helpers ------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --mode astable [--vcc V] --ra RA --rb RB --c C --T T --dt dt [--lofrac a --hifrac b]\n"
            "       [--vcinit V0] [--starthigh 0|1]\n"
            "  %s --mode mono    [--vcc V] --r R --c C --T T --dt dt [--lofrac a --hifrac b]\n"
            "       [--trig t0] [--trigw tw]\n"
            "\n"
            "Defaults:\n"
            "  --mode astable, --vcc 5.0, --lofrac 0.3333333333, --hifrac 0.6666666667\n"
            "  Astable: RA=1e4, RB=1e5,  C=1e-6,  T=0.02, dt=1e-5, Vcinit=VCC/3, starthigh=1\n"
            "  Mono   : R =1e4,  C =1e-5, T=0.05, dt=1e-4, trig=0.001, trigw=0.0001\n"
            "\n",
            prog, prog);
}

static int parse_args(int argc, char **argv, SimCfg *cfg)
{
    /* Defaults */
    cfg->mode = MODE_ASTABLE;
    cfg->Vcc = 5.0;
    cfg->C = 1e-6;
    cfg->RA = 1e4;
    cfg->RB = 1e5;
    cfg->R = 1e4;
    cfg->lofrac = 1.0 / 3.0;
    cfg->hifrac = 2.0 / 3.0;
    cfg->T_end = 0.02;
    cfg->dt = 1e-5;
    cfg->Vc_init = -1.0; /* means auto */
    cfg->start_high = 1;
    cfg->trig_time = 0.001;
    cfg->trig_width = 1e-4;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc)
        {
            i++;
            if (!strcmp(argv[i], "astable"))
                cfg->mode = MODE_ASTABLE;
            else if (!strcmp(argv[i], "mono"))
                cfg->mode = MODE_MONO;
            else
            {
                fprintf(stderr, "Unknown mode %s\n", argv[i]);
                return -1;
            }
        }
        else if (!strcmp(argv[i], "--vcc") && i + 1 < argc)
            cfg->Vcc = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--ra") && i + 1 < argc)
            cfg->RA = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--rb") && i + 1 < argc)
            cfg->RB = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--r") && i + 1 < argc)
            cfg->R = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--c") && i + 1 < argc)
            cfg->C = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--T") && i + 1 < argc)
            cfg->T_end = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--dt") && i + 1 < argc)
            cfg->dt = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--lofrac") && i + 1 < argc)
            cfg->lofrac = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--hifrac") && i + 1 < argc)
            cfg->hifrac = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--vcinit") && i + 1 < argc)
            cfg->Vc_init = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--starthigh") && i + 1 < argc)
            cfg->start_high = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trig") && i + 1 < argc)
            cfg->trig_time = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--trigw") && i + 1 < argc)
            cfg->trig_width = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage(argv[0]);
            exit(0);
        }
        else
        {
            fprintf(stderr, "Unknown or incomplete arg: %s\n", argv[i]);
            return -1;
        }
    }

    if (cfg->lofrac <= 0 || cfg->hifrac >= 1 || cfg->lofrac >= cfg->hifrac)
    {
        fprintf(stderr, "Bad thresholds: lofrac < hifrac with 0<lo<hi<1 required.\n");
        return -1;
    }
    if (cfg->Vcc <= 0 || cfg->C <= 0 || cfg->dt <= 0 || cfg->T_end <= 0)
    {
        fprintf(stderr, "Require positive VCC, C, dt, T.\n");
        return -1;
    }
    return 0;
}

/* time to reach Vtarget when charging toward Vcc with R*C */
static double t_to_reach_charge(double V0, double Vtarget, double Vcc, double RC)
{
    /* V(t) = Vcc + (V0 - Vcc) * exp(-t/RC) */
    if (fabs(V0 - Vtarget) < 1e-15)
        return 0.0;
    double num = Vtarget - Vcc;
    double den = V0 - Vcc;
    if ((num / den) <= 0)
    {
        /* Moving toward target only if Vtarget>V0 (when V0<Vcc) */
        if (Vtarget <= V0)
            return INFINITY;
    }
    double ratio = num / den;
    if (ratio <= 0)
        return INFINITY; /* impossible with monotonic charge */
    double t = -RC * log(ratio);
    return (t >= 0) ? t : INFINITY;
}

/* time to reach Vtarget when discharging to 0 through R*C */
static double t_to_reach_discharge(double V0, double Vtarget, double RC)
{
    /* V(t) = V0 * exp(-t/RC) */
    if (Vtarget >= V0)
        return INFINITY;
    if (Vtarget <= 0)
        return (V0 > 0) ? RC * 50 /* huge */ : 0;
    double t = -RC * log(Vtarget / V0);
    return (t >= 0) ? t : INFINITY;
}

/* clamp small negatives to zero due to float error */
static double clamp0(double x) { return (x < 0 && x > -1e-15) ? 0.0 : x; }

/* ------------ simulation ------------- */

static void simulate_astable(const SimCfg *cfg)
{
    double Vhi = cfg->hifrac * cfg->Vcc;
    double Vlo = cfg->lofrac * cfg->Vcc;
    double RCchg = (cfg->RA + cfg->RB) * cfg->C;
    double RCdis = (cfg->RB) * cfg->C;

    /* Analytic timing */
    double Thigh = (log((cfg->Vcc - Vlo) / (cfg->Vcc - Vhi))) * RCchg; /* == ln(2)*(RA+RB)C at 1/3..2/3 */
    double Tlow = (log(Vhi / Vlo)) * RCdis;                            /* == ln(2)*RB*C */
    double Tper = Thigh + Tlow;
    double duty = Thigh / Tper;
    fprintf(stderr,
            "[astable] expected: Thigh=%.9g s, Tlow=%.9g s, T=%.9g s, f=%.9g Hz, duty=%.4f\n",
            Thigh, Tlow, Tper, (Tper > 0 ? 1.0 / Tper : 0.0), duty);

    double Vc = (cfg->Vc_init >= 0 ? cfg->Vc_init : Vlo); /* default start at 1/3 VCC */
    int out_high = cfg->start_high ? 1 : 0;               /* HIGH -> charging, LOW -> discharging */
    int discharge_on = out_high ? 0 : 1;

    printf("time_s,vcap_v,vout_v,state,discharge_on\n");

    double t = 0.0;
    for (double t_out = 0.0; t_out <= cfg->T_end + 1e-12; t_out += cfg->dt)
    {
        /* advance internal state from t to t_out, processing any threshold events exactly */
        double remaining = t_out - t;
        while (remaining > 1e-15)
        {
            if (out_high)
            {
                /* charging toward Vcc with RA+RB until Vhi */
                double t_event = t_to_reach_charge(Vc, Vhi, cfg->Vcc, RCchg);
                if (t_event > remaining)
                {
                    /* no event within this slice: evolve */
                    double alpha = exp(-remaining / RCchg);
                    Vc = cfg->Vcc + (Vc - cfg->Vcc) * alpha;
                    t += remaining;
                    remaining = 0.0;
                }
                else
                {
                    /* jump to event exactly, flip state */
                    if (isfinite(t_event))
                    {
                        Vc = Vhi;
                        t += t_event;
                        remaining -= t_event;
                    }
                    else
                    {
                        /* should not happen; break to avoid loop */
                        t += remaining;
                        remaining = 0.0;
                    }
                    out_high = 0;
                    discharge_on = 1; /* go LOW, start discharging */
                }
            }
            else
            {
                /* discharging toward 0 through RB until Vlo */
                double t_event = t_to_reach_discharge(Vc, Vlo, RCdis);
                if (t_event > remaining)
                {
                    double alpha = exp(-remaining / RCdis);
                    Vc = Vc * alpha;
                    t += remaining;
                    remaining = 0.0;
                }
                else
                {
                    if (isfinite(t_event))
                    {
                        Vc = Vlo;
                        t += t_event;
                        remaining -= t_event;
                    }
                    else
                    {
                        t += remaining;
                        remaining = 0.0;
                    }
                    out_high = 1;
                    discharge_on = 0; /* go HIGH, start charging */
                }
            }
        }

        double Vout = out_high ? cfg->Vcc : 0.0;
        if (Vc < 0)
            Vc = 0;
        if (Vc > cfg->Vcc)
            Vc = cfg->Vcc; /* numeric safety */
        printf("%.10g,%.10g,%.10g,%d,%d\n", t_out, Vc, Vout, out_high, discharge_on);
    }
}

static void simulate_mono(const SimCfg *cfg)
{
    double Vhi = cfg->hifrac * cfg->Vcc;
    double Vlo = cfg->lofrac * cfg->Vcc;
    (void)Vlo; /* not used in ideal mono run */
    double RC = cfg->R * cfg->C;

    /* Analytic pulse width for step from 0 -> 2/3 VCC is ln(3)*R*C */
    double Tpulse = log((cfg->Vcc) / (cfg->Vcc - Vhi)) * RC; /* = ln(3) RC if hi=2/3 Vcc */
    fprintf(stderr, "[monostable] expected pulse width: %.9g s (≈ ln(3)*R*C)\n", Tpulse);

    /* States */
    double Vc = 0.0;
    int out_high = 0;
    int discharge_on = 1; /* holds cap at ~0 before trigger */

    printf("time_s,vcap_v,vout_v,state,discharge_on\n");

    double t = 0.0;
    for (double t_out = 0.0; t_out <= cfg->T_end + 1e-12; t_out += cfg->dt)
    {
        double remaining = t_out - t;
        while (remaining > 1e-15)
        {
            /* Trigger handling: when within this slice and currently LOW -> set HIGH */
            double t_to_trig = INFINITY, t_to_trig_end = INFINITY;
            if (!out_high)
            {
                if (t < cfg->trig_time && t + remaining >= cfg->trig_time)
                {
                    t_to_trig = cfg->trig_time - t;
                }
                if (t < cfg->trig_time + cfg->trig_width && t + remaining >= cfg->trig_time + cfg->trig_width)
                {
                    t_to_trig_end = cfg->trig_time + cfg->trig_width - t;
                }
            }

            if (out_high)
            {
                /* charging toward Vcc until Vhi */
                double t_event = t_to_reach_charge(Vc, Vhi, cfg->Vcc, RC);
                if (t_event > remaining)
                {
                    double alpha = exp(-remaining / RC);
                    Vc = cfg->Vcc + (Vc - cfg->Vcc) * alpha;
                    t += remaining;
                    remaining = 0.0;
                }
                else
                {
                    if (isfinite(t_event))
                    {
                        Vc = Vhi;
                        t += t_event;
                        remaining -= t_event;
                    }
                    else
                    {
                        t += remaining;
                        remaining = 0.0;
                    }
                    out_high = 0;
                    discharge_on = 1; /* output LOW, discharge clamps */
                    Vc = 0.0;         /* idealized fast discharge */
                }
            }
            else
            {
                /* Output LOW: cap clamped to ~0. Just wait for trigger or time to pass */
                double advance = remaining;
                if (isfinite(t_to_trig) && t_to_trig >= 0 && t_to_trig < advance)
                    advance = t_to_trig;
                /* (We don't need t_to_trig_end for idealized level-trigger: falling edge irrelevant) */
                t += advance;
                remaining -= advance;
                if (advance == t_to_trig)
                {
                    /* Trigger event: go HIGH, release clamp, start charging from ~0V */
                    out_high = 1;
                    discharge_on = 0;
                    Vc = 0.0;
                }
            }
        }

        double Vout = out_high ? cfg->Vcc : 0.0;
        if (Vc < 0)
            Vc = 0;
        if (Vc > cfg->Vcc)
            Vc = cfg->Vcc;
        printf("%.10g,%.10g,%.10g,%d,%d\n", t_out, Vc, Vout, out_high, discharge_on);
    }
}

/* --------------- main ---------------- */

int main(int argc, char **argv)
{
    SimCfg cfg;
    if (parse_args(argc, argv, &cfg) != 0)
    {
        usage(argv[0]);
        return 1;
    }

    if (cfg.mode == MODE_ASTABLE)
    {
        if (cfg.Vc_init < 0)
            cfg.Vc_init = cfg.Vcc * cfg.lofrac;
        if (cfg.RA <= 0 || cfg.RB <= 0)
        {
            fprintf(stderr, "RA and RB must be > 0\n");
            return 1;
        }
        simulate_astable(&cfg);
    }
    else
    {
        if (cfg.R <= 0)
        {
            fprintf(stderr, "R must be > 0\n");
            return 1;
        }
        simulate_mono(&cfg);
    }
    return 0;
}
