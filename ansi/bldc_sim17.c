/* bldc_sim.c - BLDC motor + controller simulator (6-step trapezoidal)
 * Modes:
 *  - hall       : ideal commutation from true electrical angle
 *  - sensorless : zero-cross detection on floating phase back-EMF + 30° delay
 *
 * Build:
 *   cc -O2 -std=c99 -Wall -Wextra -o bldc_sim bldc_sim.c
 *
 * Examples:
 *   ./bldc_sim --t=1.0 --rpm=1500 --vdc=24 > trace_hall.csv
 *   ./bldc_sim --mode=sensorless --t=1.0 --rpm=1500 --vdc=24 --blank=0.0002 > trace_zc.csv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------- small helpers ---------- */
static double clamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
static double wrap2pi(double x)
{
    while (x >= 2 * M_PI)
        x -= 2 * M_PI;
    while (x < 0)
        x += 2 * M_PI;
    return x;
}
static double rad2deg(double r) { return r * (180.0 / M_PI); }

/* Trapezoid 120° flat per phase, range [-1,1] by electrical angle */
static double trap120(double theta)
{
    double d = rad2deg(wrap2pi(theta));
    if (d >= 30.0 && d < 150.0)
        return 1.0;
    if (d >= 210.0 && d < 330.0)
        return -1.0;
    if (d >= 150.0 && d < 210.0)
        return 1.0 - ((d - 150.0) * (2.0 / 60.0));
    double x = d;
    if (x < 30.0)
        x += 360.0;
    return -1.0 + ((x - 330.0) * (2.0 / 60.0));
}

/* Sector (1..6) from electrical angle */
static int sector60(double theta_e)
{
    int s = (int)floor(rad2deg(wrap2pi(theta_e)) / 60.0) + 1;
    if (s < 1)
        s = 1;
    if (s > 6)
        s = 6;
    return s;
}

/* Hall pattern for logging: 1:001, 2:101, 3:100, 4:110, 5:010, 6:011 */
static int hall_from_sector(int s)
{
    switch (s)
    {
    case 1:
        return 0b001;
    case 2:
        return 0b101;
    case 3:
        return 0b100;
    case 4:
        return 0b110;
    case 5:
        return 0b010;
    default:
        return 0b011;
    }
}

/* Which phase floats in each sector? 0:A,1:B,2:C */
static int float_phase_for_sector(int s)
{
    switch (s)
    {
    case 1:
        return 2; /* C float (A+,B-) */
    case 2:
        return 1; /* B float (A+,C-) */
    case 3:
        return 0; /* A float (B+,C-) */
    case 4:
        return 2; /* C float (B+,A-) */
    case 5:
        return 1; /* B float (C+,A-) */
    case 6:
        return 0; /* A float (C+,B-) */
    default:
        return 2;
    }
}

/* Phase voltages for sector: two legs driven ±(Vdc*duty/2), float at 0.
   Ensures va+vb+vc ≈ 0 (average model). */
static void phase_voltages_from_sector(int sec, double Vdc, double duty,
                                       double *va, double *vb, double *vc)
{
    double v = 0.5 * Vdc * clamp(duty, 0.0, 1.0);
    *va = *vb = *vc = 0.0;
    switch (sec)
    {
    case 1:
        *va = +v;
        *vb = -v;
        break; /* C float */
    case 2:
        *va = +v;
        *vc = -v;
        break; /* B float */
    case 3:
        *vb = +v;
        *vc = -v;
        break; /* A float */
    case 4:
        *vb = +v;
        *va = -v;
        break; /* C float */
    case 5:
        *vc = +v;
        *va = -v;
        break; /* B float */
    case 6:
        *vc = +v;
        *vb = -v;
        break; /* A float */
    default:
        break;
    }
}

/* ---------- Plant params & state ---------- */
typedef struct
{
    double R, L;    /* Ohm, H per phase */
    double Ke, Kt;  /* Vs/rad, Nm/A */
    int pole_pairs; /* electrical per mechanical */
    double J, B;    /* kg*m^2, Nms/rad */
} MotorParams;

typedef struct
{
    double ia, ib, ic; /* phase currents */
    double omega_m;    /* mech rad/s */
    double theta_m;    /* mech angle rad */
} MotorState;

/* ---------- Controller ---------- */
typedef struct
{
    double target_rpm;
    double Kp, Ki;
    double integ;
    double duty;
    double ilim; /* A, current limit (per-phase max abs) */
} CtrlState;

/* Controller step with simple current limit clamp */
static double ctrl_step(CtrlState *cs, double rpm_meas,
                        double imax_abs, double dt)
{
    double e = cs->target_rpm - rpm_meas;
    cs->integ += cs->Ki * e * dt;
    double u = cs->Kp * e + cs->integ;
    double duty = clamp(u, 0.0, 1.0);

    /* Current limit: scale duty down if any phase exceeds ilim */
    if (cs->ilim > 0.0 && imax_abs > cs->ilim)
    {
        double scale = cs->ilim / (imax_abs + 1e-9);
        scale = clamp(scale, 0.0, 1.0);
        duty *= scale;
        /* anti-windup: roll back integrator if we’re clipping for current */
        if ((u > duty && e > 0) || (u < duty && e < 0))
        {
            cs->integ -= cs->Ki * e * dt;
        }
    }
    else if (u != duty)
    {
        /* classic anti-windup for duty saturation */
        if ((u > 1.0 && e > 0) || (u < 0.0 && e < 0))
        {
            cs->integ -= cs->Ki * e * dt;
        }
    }

    cs->duty = duty;
    return duty;
}

/* ---------- Sensorless ZC commutation state ---------- */
typedef struct
{
    int sector;          /* 1..6 currently applied */
    double blank_until;  /* no ZC valid before this time */
    double last_zc_time; /* last ZC detect time */
    double commute_due;  /* next scheduled commutation time, or <0 if none */
    int last_sign;       /* sign of floating-phase bemf last step */
} ZCState;

/* ---------- One integration step of the plant ---------- */
static void motor_step(const MotorParams *mp, MotorState *ms,
                       double va, double vb, double vc,
                       double dt, double *Te_out)
{
    /* Electrical angle for bemf shape */
    double theta_e = wrap2pi(mp->pole_pairs * ms->theta_m);
    double fa = trap120(theta_e);
    double fb = trap120(theta_e - 2.0 * M_PI / 3.0);
    double fc = trap120(theta_e + 2.0 * M_PI / 3.0);

    /* Back-EMFs */
    double ea = mp->Ke * ms->omega_m * fa;
    double eb = mp->Ke * ms->omega_m * fb;
    double ec = mp->Ke * ms->omega_m * fc;

    /* RL dynamics (line-to-neutral average model) */
    double dia = (va - mp->R * ms->ia - ea) / mp->L;
    double dib = (vb - mp->R * ms->ib - eb) / mp->L;
    double dic = (vc - mp->R * ms->ic - ec) / mp->L;

    ms->ia += dia * dt;
    ms->ib += dib * dt;
    ms->ic += dic * dt;

    /* Electromagnetic torque */
    double Te = mp->Kt * (fa * ms->ia + fb * ms->ib + fc * ms->ic) / 1.5;
    if (Te_out)
        *Te_out = Te;

    /* Mechanical */
    double domega = (Te - mp->B * ms->omega_m) / mp->J;
    ms->omega_m += domega * dt;
    if (ms->omega_m < 0)
        ms->omega_m = 0;
    ms->theta_m = wrap2pi(ms->theta_m + ms->omega_m * dt);
}

/* ---------- CLI parsing ---------- */
static int argd(const char *a, const char *key, double *out)
{
    size_t n = strlen(key);
    if (strncmp(a, key, n) == 0 && a[n] == '=')
    {
        *out = atof(a + n + 1);
        return 1;
    }
    return 0;
}
static int args(const char *a, const char *key, const char **out)
{
    size_t n = strlen(key);
    if (strncmp(a, key, n) == 0 && a[n] == '=')
    {
        *out = a + n + 1;
        return 1;
    }
    return 0;
}
static void usage(const char *p)
{
    fprintf(stderr,
            "Usage: %s [--mode=hall|sensorless] [--t=sec] [--dt=s] [--logdt=s]\n"
            "          [--rpm=target] [--vdc=V] [--load=Nm] [--ilim=A] [--blank=s]\n"
            "          [--pp=N] [--R=ohm] [--L=H] [--Ke=Vs/rad] [--Kt=Nm/A] [--J=kgm2] [--B=Nms/rad]\n"
            "Defaults: --mode=hall --t=1.0 --dt=0.00005 --logdt=0.001 --rpm=1500 --vdc=24 --load=0.05\n"
            "          --ilim=0 (off) --blank=0.0002 --pp=4 --R=0.5 --L=0.0002 --Ke=0.06 --Kt=0.06 --J=1e-4 --B=1e-4\n",
            p);
}

/* ---------- Main ---------- */
int main(int argc, char **argv)
{
    /* Defaults */
    const char *mode = "hall";
    double sim_t = 1.0, dt = 0.00005, logdt = 0.001, target_rpm = 1500.0;
    double Vdc = 24.0, load_torque = 0.05, ilim = 0.0, blank = 0.0002;

    MotorParams mp = {.R = 0.5, .L = 0.0002, .Ke = 0.06, .Kt = 0.06, .pole_pairs = 4, .J = 1e-4, .B = 1e-4};

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--help"))
        {
            usage(argv[0]);
            return 0;
        }
        else if (args(argv[i], "--mode", &mode))
        {
        }
        else if (argd(argv[i], "--t", &sim_t))
        {
        }
        else if (argd(argv[i], "--dt", &dt))
        {
        }
        else if (argd(argv[i], "--logdt", &logdt))
        {
        }
        else if (argd(argv[i], "--rpm", &target_rpm))
        {
        }
        else if (argd(argv[i], "--vdc", &Vdc))
        {
        }
        else if (argd(argv[i], "--load", &load_torque))
        {
        }
        else if (argd(argv[i], "--ilim", &ilim))
        {
        }
        else if (argd(argv[i], "--blank", &blank))
        {
        }
        else if (argd(argv[i], "--pp", (double *)&mp.pole_pairs))
        {
            mp.pole_pairs = (int)mp.pole_pairs;
        }
        else if (argd(argv[i], "--R", &mp.R))
        {
        }
        else if (argd(argv[i], "--L", &mp.L))
        {
        }
        else if (argd(argv[i], "--Ke", &mp.Ke))
        {
        }
        else if (argd(argv[i], "--Kt", &mp.Kt))
        {
        }
        else if (argd(argv[i], "--J", &mp.J))
        {
        }
        else if (argd(argv[i], "--B", &mp.B))
        {
        }
        else
        {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    MotorState ms = {0};
    CtrlState cs = {0};
    cs.target_rpm = target_rpm;
    cs.Kp = 0.0015;
    cs.Ki = 0.5;
    cs.integ = 0.0;
    cs.ilim = ilim;

    ZCState zc = {0};
    zc.sector = 1; /* will be overwritten below for sensorless */
    zc.blank_until = 0.0;
    zc.last_zc_time = -1.0;
    zc.commute_due = -1.0;
    zc.last_sign = 0;

    /* Initialize sector (and hall for logging) from true angle */
    double theta_e0 = wrap2pi(mp.pole_pairs * ms.theta_m);
    int sec_true = sector60(theta_e0);
    if (!strcmp(mode, "sensorless"))
    {
        zc.sector = sec_true; /* start reasonable */
        /* initialize sign based on floating phase bemf */
        int fidx = float_phase_for_sector(zc.sector);
        double fa = trap120(theta_e0);
        double fb = trap120(theta_e0 - 2.0 * M_PI / 3.0);
        double fc = trap120(theta_e0 + 2.0 * M_PI / 3.0);
        double ef = (fidx == 0 ? fa : (fidx == 1 ? fb : fc)) * ms.omega_m * mp.Ke;
        zc.last_sign = (ef >= 0.0) ? +1 : -1;
    }

    /* CSV header */
    printf("# t,rpm,duty,mode,sector,hall,zc,ia,ib,ic,torque,theta_e_deg\n");

    double t = 0.0, next_log = 0.0;
    while (t < sim_t)
    {
        /* Measurements */
        double rpm = ms.omega_m * (60.0 / (2.0 * M_PI));
        double imax = fmax(fabs(ms.ia), fmax(fabs(ms.ib), fabs(ms.ic)));

        /* Controller */
        double duty = ctrl_step(&cs, rpm, imax, dt);

        /* Decide sector used for applying voltages */
        double theta_e = wrap2pi(mp.pole_pairs * ms.theta_m);
        int sector_for_volt = sec_true; /* default: hall/true */
        int zc_event = 0;

        if (!strcmp(mode, "sensorless"))
        {
            /* Update true sector for logging only */
            sec_true = sector60(theta_e);

            /* Sensorless ZC detection on floating phase bemf sign */
            int fidx = float_phase_for_sector(zc.sector);
            double fa = trap120(theta_e);
            double fb = trap120(theta_e - 2.0 * M_PI / 3.0);
            double fc = trap120(theta_e + 2.0 * M_PI / 3.0);
            double ef = (fidx == 0 ? fa : (fidx == 1 ? fb : fc)) * mp.Ke * ms.omega_m;

            int sign = (ef >= 0.0) ? +1 : -1;
            if (t > zc.blank_until && sign != zc.last_sign)
            {
                zc_event = 1;
                /* schedule commutation at +30° */
                if (zc.last_zc_time >= 0.0)
                {
                    double half_period = t - zc.last_zc_time; /* ~180° for same phase crossings */
                    double delay = half_period / 6.0;         /* 30° is 1/6 of 180° */
                    zc.commute_due = t + delay;
                }
                else
                {
                    double omega_e = fmax(1e-6, mp.pole_pairs * ms.omega_m);
                    zc.commute_due = t + (M_PI / 6.0) / omega_e; /* 30° at current speed */
                }
                zc.last_zc_time = t;
                zc.last_sign = sign;
            }

            /* If time to commute, advance sector and blank ZC */
            if (zc.commute_due >= 0.0 && t >= zc.commute_due)
            {
                zc.sector = (zc.sector % 6) + 1;
                zc.commute_due = -1.0;
                zc.blank_until = t + blank;
                /* update sign for new floating phase immediately */
                int nf = float_phase_for_sector(zc.sector);
                double fa2 = trap120(theta_e);
                double fb2 = trap120(theta_e - 2.0 * M_PI / 3.0);
                double fc2 = trap120(theta_e + 2.0 * M_PI / 3.0);
                double ef2 = (nf == 0 ? fa2 : (nf == 1 ? fb2 : fc2)) * mp.Ke * ms.omega_m;
                zc.last_sign = (ef2 >= 0.0) ? +1 : -1;
            }

            sector_for_volt = zc.sector;
        }
        else
        {
            /* Hall-perfect commutation uses true sector */
            sec_true = sector60(theta_e);
            sector_for_volt = sec_true;
        }

        /* Voltages from chosen sector */
        double va, vb, vc;
        phase_voltages_from_sector(sector_for_volt, Vdc, duty, &va, &vb, &vc);

        /* Integrate plant one step */
        double Te;
        motor_step(&mp, &ms, va, vb, vc, dt, &Te);

        /* Log at logdt */
        if (t >= next_log)
        {
            int hall = hall_from_sector(sector60(theta_e));
            printf("%.6f,%.2f,%.4f,%s,%d,%d,%d,%.4f,%.4f,%.4f,%.5f,%.2f\n",
                   t, rpm, duty, (!strcmp(mode, "sensorless") ? "sensorless" : "hall"),
                   sector_for_volt, hall, zc_event,
                   ms.ia, ms.ib, ms.ic, Te, rad2deg(theta_e));
            next_log += logdt;
        }

        t += dt;
    }

    return 0;
}
