/* bldc_sim.c - Text-only BLDC motor control simulator (6-step trapezoidal)
 *
 * Models:
 *  - Electrical: v = R*i + L*di/dt + e_trap(θ_e, ω)
 *  - Mechanical: J*dω/dt = T_e - B*ω - T_load
 *  - 6-step commutation with PWM duty (0..1) from a PI speed controller
 *  - Hall sensors derived from electrical sector (60° per sector)
 *
 * CSV columns (per log step):
 *   t, rpm, duty, sector, hall, ia, ib, ic, torque, theta_e_deg
 *
 * Build:
 *   cc -O2 -std=c99 -Wall -Wextra -o bldc_sim bldc_sim.c
 *
 * Example:
 *   ./bldc_sim --t=1.0 --rpm=1500 --vdc=24 --load=0.05 > trace.csv
 *
 * Notes:
 *  - Parameters are representative small-motor defaults. Adjust to your plant.
 *  - Controller is simple PI (no current loop). Keep dt small for stability.
 *  - Back-EMF & torque use the same trapezoid shape f(θ) in [-1,1].
 *  - All angles are radians internally; electrical angle θ_e = p * θ_m.
 */

#if 0
Tips to play with
Increase --rpm to see the PI loop raise duty until back-EMF + losses balance.
Try heavier load: --load=0.2 (rpm will sag unless you bump gains).
Raise --pp for more electrical commutations per mechanical rev (e.g., 7 pole pairs).
Sweep R/L to see current ripple effects; decrease --L to make it “spikier.”
Log faster with --logdt=0.0005 (more rows), or slower to keep files small.
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------- Helpers ---------- */
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

/* Trapezoidal waveform f_trap(θ) ∈ [-1,1], 120° flat top, 60° linear transitions.
   Definition by electrical angle (radians). */
static double trap120(double theta)
{
    double deg = rad2deg(wrap2pi(theta)); /* 0..360 */
    if (deg >= 30.0 && deg < 150.0)
        return 1.0; /* +flat */
    if (deg >= 210.0 && deg < 330.0)
        return -1.0; /* -flat */
    /* Ramps over 60° */
    if (deg >= 150.0 && deg < 210.0)
    {
        /* 150..210: 1 -> -1 */
        return 1.0 - ((deg - 150.0) * (2.0 / 60.0));
    }
    /* deg in [330..360) U [0..30): -1 -> +1 */
    double x = deg;
    if (x < 30.0)
        x += 360.0;
    return -1.0 + ((x - 330.0) * (2.0 / 60.0));
}

/* Sector 1..6, each 60° of electrical angle. Sector 1: [0..60°) */
static int sector60(double theta_e)
{
    int s = (int)floor(rad2deg(wrap2pi(theta_e)) / 60.0) + 1;
    if (s < 1)
        s = 1;
    if (s > 6)
        s = 6;
    return s;
}

/* Hall pattern for sector (typical): 1:001, 2:101, 3:100, 4:110, 5:010, 6:011 */
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

/* ---------- Parameters & State ---------- */
typedef struct
{
    /* Motor/electrical */
    double R;       /* Ohm per phase */
    double L;       /* H per phase */
    double Ke;      /* V*s/rad (phase BEMF constant, trapezoid amplitude) */
    double Kt;      /* Nm/A  (torque constant; using same shape as BEMF) */
    int pole_pairs; /* electrical per mechanical ratio */
    /* Mechanical */
    double J; /* kg*m^2 */
    double B; /* Nms/rad */
} MotorParams;

typedef struct
{
    /* Phase currents */
    double ia, ib, ic;
    /* Mechanics */
    double omega_m; /* mech rad/s */
    double theta_m; /* mech angle rad [0..2π) */
} MotorState;

typedef struct
{
    /* Controller setpoints & outputs */
    double target_rpm;
    /* PI controller internals */
    double Kp, Ki;
    double integ;
    double duty; /* 0..1 */
} CtrlState;

/* ---------- Commutation & Voltages ---------- */
/* Given sector (1..6), Vdc, duty (0..1), compute phase voltages.
   Two phases are driven ±(Vdc*duty/2), one floating at 0, so va+vb+vc=0. */
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
        *vc = 0.0;
        break; /* A+ B- (C float) */
    case 2:
        *va = +v;
        *vc = -v;
        *vb = 0.0;
        break; /* A+ C- */
    case 3:
        *vb = +v;
        *vc = -v;
        *va = 0.0;
        break; /* B+ C- */
    case 4:
        *vb = +v;
        *va = -v;
        *vc = 0.0;
        break; /* B+ A- */
    case 5:
        *vc = +v;
        *va = -v;
        *vb = 0.0;
        break; /* C+ A- */
    case 6:
        *vc = +v;
        *vb = -v;
        *va = 0.0;
        break; /* C+ B- */
    default:
        break;
    }
}

/* ---------- Electromechanical Step ---------- */
static void motor_step(const MotorParams *mp, MotorState *ms,
                       double Vdc, double duty, double load_torque, double dt)
{
    /* Electrical angles */
    double theta_e = wrap2pi(mp->pole_pairs * ms->theta_m);

    /* Back-EMFs per phase: e = Ke * ω_elec? For phase EMF we use ω_mech:
       With trapezoid using electrical angle, amplitude ∝ ω_mech. */
    double fa = trap120(theta_e);
    double fb = trap120(theta_e - 2.0 * M_PI / 3.0);
    double fc = trap120(theta_e + 2.0 * M_PI / 3.0);

    double ea = mp->Ke * ms->omega_m * fa;
    double eb = mp->Ke * ms->omega_m * fb;
    double ec = mp->Ke * ms->omega_m * fc;

    /* Commutation sector & applied phase voltages */
    int sec = sector60(theta_e);
    double va, vb, vc;
    phase_voltages_from_sector(sec, Vdc, duty, &va, &vb, &vc);

    /* Phase current derivatives (simple RL each to "virtual neutral"):
       di/dt = (v - R*i - e) / L  (approx: star with sum(v)=0 keeps consistency) */
    double dia = (va - mp->R * ms->ia - ea) / mp->L;
    double dib = (vb - mp->R * ms->ib - eb) / mp->L;
    double dic = (vc - mp->R * ms->ic - ec) / mp->L;

    /* Integrate (Euler) */
    ms->ia += dia * dt;
    ms->ib += dib * dt;
    ms->ic += dic * dt;

    /* Electromagnetic torque using same trapezoid shape */
    double Te = mp->Kt * (fa * ms->ia + fb * ms->ib + fc * ms->ic) / 1.5; /* /1.5 keeps magnitudes sane */

    /* Mechanical dynamics */
    double domega = (Te - mp->B * ms->omega_m - load_torque) / mp->J;
    ms->omega_m += domega * dt;
    if (ms->omega_m < 0.0)
        ms->omega_m = 0.0; /* no reverse in this simple model */

    ms->theta_m = wrap2pi(ms->theta_m + ms->omega_m * dt);
}

/* ---------- PI Speed Controller ---------- */
static double ctrl_step(CtrlState *cs, double rpm_meas, double dt)
{
    double e = cs->target_rpm - rpm_meas;
    cs->integ += cs->Ki * e * dt;
    double u = cs->Kp * e + cs->integ;
    /* Saturate duty 0..1 with simple anti-windup clamp */
    double duty = clamp(u, 0.0, 1.0);
    if ((u != duty))
    {
        /* stop integrating in the direction of windup */
        if ((u > 1.0 && e > 0.0) || (u < 0.0 && e < 0.0))
        {
            cs->integ -= cs->Ki * e * dt;
        }
    }
    cs->duty = duty;
    return duty;
}

/* ---------- CLI ---------- */
static int argd(const char *a, const char *key, double *out)
{
    /* parse --key=val */
    size_t n = strlen(key);
    if (strncmp(a, key, n) == 0 && a[n] == '=')
    {
        *out = atof(a + n + 1);
        return 1;
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--t=sec] [--dt=s] [--logdt=s] [--rpm=target] [--vdc=V] [--load=Nm]\n"
            "            [--pp=N] [--R=ohm] [--L=H] [--Ke=Vs/rad] [--Kt=Nm/A] [--J=kgm2] [--B=Nms/rad]\n"
            "Defaults: --t=1.0 --dt=0.00005 --logdt=0.001 --rpm=1500 --vdc=24 --load=0.05\n"
            "          --pp=4 --R=0.5 --L=0.0002 --Ke=0.06 --Kt=0.06 --J=1e-4 --B=1e-4\n",
            prog);
}

/* ---------- Main ---------- */
int main(int argc, char **argv)
{
    /* Defaults (small 24V motor vibe) */
    double sim_t = 1.0;
    double dt = 0.00005;
    double logdt = 0.001;
    double target_rpm = 1500.0;
    double Vdc = 24.0;
    double load_torque = 0.05;

    MotorParams mp = {
        .R = 0.5, .L = 0.0002, .Ke = 0.06, .Kt = 0.06, .pole_pairs = 4, .J = 1e-4, .B = 1e-4};

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--help"))
        {
            usage(argv[0]);
            return 0;
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
    /* Tunable PI gains (rough): try increasing Kp first, then Ki */
    cs.Kp = 0.0015; /* duty per rpm error */
    cs.Ki = 0.5;    /* duty per rpm*s */

    /* CSV header */
    printf("# t,rpm,duty,sector,hall,ia,ib,ic,torque,theta_e_deg\n");

    double t = 0.0, next_log = 0.0;

    while (t < sim_t)
    {
        /* Measure */
        double rpm = ms.omega_m * (60.0 / (2.0 * M_PI));
        /* Controller */
        double duty = ctrl_step(&cs, rpm, dt);

        /* Step plant once with current duty */
        double theta_e = wrap2pi(mp.pole_pairs * ms.theta_m);
        int sec_before = sector60(theta_e);
        int hall = hall_from_sector(sec_before);

        /* One step integration */
        double ia_prev = ms.ia, ib_prev = ms.ib, ic_prev = ms.ic;
        motor_step(&mp, &ms, Vdc, duty, load_torque, dt);

        /* Compute torque for logging using post-step values */
        double fa = trap120(wrap2pi(mp.pole_pairs * ms.theta_m));
        double fb = trap120(wrap2pi(mp.pole_pairs * ms.theta_m) - 2.0 * M_PI / 3.0);
        double fc = trap120(wrap2pi(mp.pole_pairs * ms.theta_m) + 2.0 * M_PI / 3.0);
        double Te = mp.Kt * (fa * ms.ia + fb * ms.ib + fc * ms.ic) / 1.5;

        if (t >= next_log)
        {
            double theta_e_deg = rad2deg(theta_e);
            printf("%.6f,%.2f,%.4f,%d,%d,%.4f,%.4f,%.4f,%.5f,%.2f\n",
                   t, rpm, duty, sec_before, hall,
                   ia_prev, ib_prev, ic_prev, Te, theta_e_deg);
            next_log += logdt;
        }

        t += dt;
    }

    return 0;
}
