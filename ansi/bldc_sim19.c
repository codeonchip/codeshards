/* bldc_sim.c - BLDC simulator with 6-step (hall/sensorless + startup) and FOC (SVPWM)
 *
 * Build:  cc -O2 -std=c99 -Wall -Wextra -o bldc_sim bldc_sim.c
 *
 * Modes:
 *   --mode=hall        Ideal commutation from true electrical sector
 *   --mode=sensorless  ZC on floating phase +30° delay, with align→ramp→handover
 *   --mode=foc         PI speed -> iq*, PI id/iq -> v_d/v_q, SVPWM average model
 *
 * CSV columns:
 *   t,rpm,mode,sector,hall,zc,ia,ib,ic,Te,theta_e_deg,duty,da,db,dc
 *
 * Notes:
 * - Average-voltage model; keep dt small (e.g., 50 µs).
 * - Trapezoidal back-EMF (120° flat); torque uses same shape.
 * - SVPWM implemented via phase d duties and neutral-centering (subtract average).
 */

#if 0

cc -O2 -std=c99 -Wall -Wextra -o bldc_sim bldc_sim.c

#(1) Hall - perfect 6 - step:
./bldc_sim --mode=hall --t=1.0 --rpm=1500 --vdc=24 > hall.csv

#(2) Sensorless with startup(align 60ms, ramp 300ms, handover near 200rpm):
./bldc_sim --mode=sensorless --t=1.5 --rpm=1500 --vdc=24 \
  --align=0.06 --ramp=0.30 --f1=400 --d_align=0.25 --d_ramp=0.45 --handover=200 \
  > zc.csv

#(3) FOC(id *= 0, speed loop → iq *), tweak gains as needed:
./bldc_sim --mode=foc --t=1.0 --rpm=1500 --vdc=24 \
  --Kp_spd=0.003 --Ki_spd=1.0 --Kp_id=8 --Ki_id=200 --Kp_iq=8 --Ki_iq=200 \
  > foc.csv

#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------- helpers ---------- */
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
static double fsign(double x) { return (x >= 0.0) ? +1.0 : -1.0; }
static double max3(double a, double b, double c)
{
    double m = a > b ? a : b;
    return m > c ? m : c;
}
static double abs3max(double a, double b, double c)
{
    a = fabs(a);
    b = fabs(b);
    c = fabs(c);
    return max3(a, b, c);
}

/* 120° trap in [-1,1] by electrical angle */
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

static int sector60(double theta_e)
{
    int s = (int)floor(rad2deg(wrap2pi(theta_e)) / 60.0) + 1;
    if (s < 1)
        s = 1;
    if (s > 6)
        s = 6;
    return s;
}
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
static int float_phase_for_sector(int s)
{
    switch (s)
    {
    case 1:
        return 2;
    case 2:
        return 1;
    case 3:
        return 0;
    case 4:
        return 2;
    case 5:
        return 1;
    default:
        return 0;
    }
}

/* ---------- plant ---------- */
typedef struct
{
    double R, L;   /* per-phase */
    double Ke, Kt; /* Vs/rad, Nm/A */
    int pole_pairs;
    double J, B; /* kg*m^2, Nms/rad */
} MotorParams;

typedef struct
{
    double ia, ib, ic; /* phase currents */
    double omega_m;    /* mech rad/s */
    double theta_m;    /* mech rad */
} MotorState;

/* integrate plant one step with given phase-to-neutral voltages */
static void motor_step(const MotorParams *mp, MotorState *ms,
                       double va, double vb, double vc,
                       double load_torque, double dt,
                       double *Te_out)
{
    double theta_e = wrap2pi(mp->pole_pairs * ms->theta_m);
    double fa = trap120(theta_e);
    double fb = trap120(theta_e - 2.0 * M_PI / 3.0);
    double fc = trap120(theta_e + 2.0 * M_PI / 3.0);

    double ea = mp->Ke * ms->omega_m * fa;
    double eb = mp->Ke * ms->omega_m * fb;
    double ec = mp->Ke * ms->omega_m * fc;

    /* RL */
    double dia = (va - mp->R * ms->ia - ea) / mp->L;
    double dib = (vb - mp->R * ms->ib - eb) / mp->L;
    double dic = (vc - mp->R * ms->ic - ec) / mp->L;

    ms->ia += dia * dt;
    ms->ib += dib * dt;
    ms->ic += dic * dt;

    /* electromagnetic torque */
    double Te = mp->Kt * (fa * ms->ia + fb * ms->ib + fc * ms->ic) / 1.5;
    if (Te_out)
        *Te_out = Te;

    /* mechanics */
    double domega = (Te - mp->B * ms->omega_m - load_torque) / mp->J;
    ms->omega_m += domega * dt;
    if (ms->omega_m < 0)
        ms->omega_m = 0;
    ms->theta_m = wrap2pi(ms->theta_m + ms->omega_m * dt);
}

/* voltages for 6-step sector (average model, 2 legs ±, one float=0) */
static void voltages_sixstep(int sector, double Vdc, double duty,
                             double *va, double *vb, double *vc)
{
    double v = 0.5 * Vdc * clamp(duty, 0.0, 1.0);
    *va = *vb = *vc = 0.0;
    switch (sector)
    {
    case 1:
        *va = +v;
        *vb = -v;
        break; /* C float */
    case 2:
        *va = +v;
        *vc = -v;
        break;
    case 3:
        *vb = +v;
        *vc = -v;
        break;
    case 4:
        *vb = +v;
        *va = -v;
        break;
    case 5:
        *vc = +v;
        *va = -v;
        break;
    case 6:
        *vc = +v;
        *vb = -v;
        break;
    default:
        break;
    }
}

/* ---------- simple PIs ---------- */
typedef struct
{
    double kp, ki, integ, out_min, out_max;
} PI;

static double pi_step(PI *pi, double err, double dt)
{
    double u = pi->kp * err + pi->integ;
    /* pre-sat output */
    double out = clamp(u, pi->out_min, pi->out_max);
    /* anti-windup: only integrate when not saturating in error direction */
    if (!((u > pi->out_max && err > 0) || (u < pi->out_min && err < 0)))
    {
        pi->integ += pi->ki * err * dt;
    }
    return out;
}

/* ---------- CLI ---------- */
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
            "Usage: %s --mode=hall|sensorless|foc [common opts] [mode opts]\n"
            "Common:\n"
            "  --t=1.0 --dt=0.00005 --logdt=0.001 --rpm=1500 --vdc=24 --load=0.05\n"
            "  --pp=4 --R=0.5 --L=0.0002 --Ke=0.06 --Kt=0.06 --J=1e-4 --B=1e-4 --ilim=0\n"
            "Sensorless startup:\n"
            "  --align=0.06  (s) align hold   --d_align=0.25 (duty during align)\n"
            "  --ramp=0.30   (s) ramp length  --f1=400       (elec Hz target at ramp end)\n"
            "  --d_ramp=0.45 (duty at ramp end)\n"
            "  --blank=0.0002 (s ZC blanking) --handover=200 (rpm threshold)\n"
            "FOC control:\n"
            "  --Kp_spd=0.003 --Ki_spd=1.0 --Kp_id=8 --Ki_id=200 --Kp_iq=8 --Ki_iq=200\n"
            "  --idref=0       --iqmax=30     --vmax=0.577*Vdc (default) \n",
            p);
}

/* ---------- main ---------- */
int main(int argc, char **argv)
{
    /* defaults */
    const char *mode = "hall";
    double sim_t = 1.0, dt = 0.00005, logdt = 0.001;
    double target_rpm = 1500.0, Vdc = 24.0, load_torque = 0.05, ilim = 0.0;

    /* sensorless startup defaults */
    double t_align = 0.06, t_ramp = 0.30, f1 = 400.0; /* electrical Hz at ramp end */
    double duty_align = 0.25, duty_ramp = 0.45, blank = 0.0002, handover_rpm = 200.0;

    /* foc defaults */
    double Kp_spd = 0.003, Ki_spd = 1.0;
    double Kp_id = 8.0, Ki_id = 200.0;
    double Kp_iq = 8.0, Ki_iq = 200.0;
    double id_ref = 0.0, iq_max = 30.0;  /* A */
    double vmax_scale = 1.0 / sqrt(3.0); /* ~0.577*Vdc linear SVPWM */

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

        else if (argd(argv[i], "--align", &t_align))
        {
        }
        else if (argd(argv[i], "--ramp", &t_ramp))
        {
        }
        else if (argd(argv[i], "--f1", &f1))
        {
        }
        else if (argd(argv[i], "--d_align", &duty_align))
        {
        }
        else if (argd(argv[i], "--d_ramp", &duty_ramp))
        {
        }
        else if (argd(argv[i], "--blank", &blank))
        {
        }
        else if (argd(argv[i], "--handover", &handover_rpm))
        {
        }

        else if (argd(argv[i], "--Kp_spd", &Kp_spd))
        {
        }
        else if (argd(argv[i], "--Ki_spd", &Ki_spd))
        {
        }
        else if (argd(argv[i], "--Kp_id", &Kp_id))
        {
        }
        else if (argd(argv[i], "--Ki_id", &Ki_id))
        {
        }
        else if (argd(argv[i], "--Kp_iq", &Kp_iq))
        {
        }
        else if (argd(argv[i], "--Ki_iq", &Ki_iq))
        {
        }
        else if (argd(argv[i], "--idref", &id_ref))
        {
        }
        else if (argd(argv[i], "--iqmax", &iq_max))
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

    /* state */
    MotorState ms = {0};

    /* speed PI (for 6-step duty or FOC iq*) */
    PI spd = {.kp = Kp_spd, .ki = Ki_spd, .integ = 0.0, .out_min = 0.0, .out_max = 1.0};

    /* current PIs (FOC) output volts */
    PI pi_id = {.kp = Kp_id, .ki = Ki_id, .integ = 0.0, .out_min = -1e9, .out_max = +1e9};
    PI pi_iq = {.kp = Kp_iq, .ki = Ki_iq, .integ = 0.0, .out_min = -1e9, .out_max = +1e9};

    /* sensorless ZC state */
    int zc_sector = 1;
    double blank_until = 0.0, last_zc_time = -1.0, commute_due = -1.0;
    int last_sign = 0;
    enum
    {
        ZC_START_ALIGN,
        ZC_START_RAMP,
        ZC_CLOSED
    } zc_phase = ZC_START_ALIGN;
    double forced_theta_e = 0.0; /* for open-loop sector */

    /* CSV header */
    printf("# t,rpm,mode,sector,hall,zc,ia,ib,ic,Te,theta_e_deg,duty,da,db,dc\n");

    /* simulation loop */
    double t = 0.0, next_log = 0.0;
    while (t < sim_t)
    {
        double theta_e_true = wrap2pi(mp.pole_pairs * ms.theta_m);
        int sec_true = sector60(theta_e_true);
        int hall = hall_from_sector(sec_true);
        double rpm = ms.omega_m * (60.0 / (2.0 * M_PI));
        double duty = 0.0, da = 0.0, db = 0.0, dc = 0.0;
        int zc_event = 0;

        /* ======== control & voltages by mode ======== */
        double va = 0, vb = 0, vc = 0;

        if (!strcmp(mode, "hall"))
        {
            /* speed PI -> duty (0..1), clamp by current limit if desired */
            double e_rpm = target_rpm - rpm;
            duty = clamp(pi_step(&spd, e_rpm, dt), 0.0, 1.0);

            /* simple current limit: scale duty if needed */
            if (ilim > 0.0)
            {
                double imax = abs3max(ms.ia, ms.ib, ms.ic);
                if (imax > ilim)
                    duty *= ilim / (imax + 1e-9);
            }
            voltages_sixstep(sec_true, Vdc, duty, &va, &vb, &vc);
        }
        else if (!strcmp(mode, "sensorless"))
        {
            /* open-loop startup + ZC handover → then ZC-based commutation */
            if (zc_phase == ZC_START_ALIGN)
            {
                duty = clamp(duty_align, 0.0, 1.0);
                /* hold electrical angle to a fixed sector to align */
                zc_sector = 1;
                voltages_sixstep(zc_sector, Vdc, duty, &va, &vb, &vc);
                if (t >= t_align)
                {
                    zc_phase = ZC_START_RAMP;
                    forced_theta_e = 0.0;
                }
            }
            else if (zc_phase == ZC_START_RAMP)
            {
                /* ramp electrical frequency 0 → 2π*f1 */
                double alpha = clamp((t - t_align) / fmax(1e-9, t_ramp), 0.0, 1.0);
                double f_e = f1 * alpha;               /* Hz */
                double omega_e_cmd = 2.0 * M_PI * f_e; /* rad/s */
                forced_theta_e = wrap2pi(forced_theta_e + omega_e_cmd * dt);
                zc_sector = sector60(forced_theta_e);
                duty = clamp(duty_align + (duty_ramp - duty_align) * alpha, 0.0, 1.0);
                voltages_sixstep(zc_sector, Vdc, duty, &va, &vb, &vc);

                if (rpm >= handover_rpm)
                {
                    /* initialize ZC detector from current sector/sign */
                    int fidx = float_phase_for_sector(zc_sector);
                    double fa = trap120(theta_e_true);
                    double fb = trap120(theta_e_true - 2.0 * M_PI / 3.0);
                    double fc = trap120(theta_e_true + 2.0 * M_PI / 3.0);
                    double ef = (fidx == 0 ? fa : (fidx == 1 ? fb : fc)) * mp.Ke * ms.omega_m;
                    last_sign = (ef >= 0) ? +1 : -1;
                    last_zc_time = -1.0;
                    commute_due = -1.0;
                    blank_until = t + blank;
                    zc_phase = ZC_CLOSED;
                }
            }
            else
            { /* ZC_CLOSED */
                /* outer speed PI → duty */
                double e_rpm = target_rpm - rpm;
                duty = clamp(pi_step(&spd, e_rpm, dt), 0.0, 1.0);
                if (ilim > 0.0)
                {
                    double imax = abs3max(ms.ia, ms.ib, ms.ic);
                    if (imax > ilim)
                        duty *= ilim / (imax + 1e-9);
                }

                /* detect ZC on floating phase bemf sign */
                int fidx = float_phase_for_sector(zc_sector);
                double fa = trap120(theta_e_true);
                double fb = trap120(theta_e_true - 2.0 * M_PI / 3.0);
                double fc = trap120(theta_e_true + 2.0 * M_PI / 3.0);
                double ef = (fidx == 0 ? fa : (fidx == 1 ? fb : fc)) * mp.Ke * ms.omega_m;
                int sign = (ef >= 0) ? +1 : -1;

                if (t > blank_until && sign != last_sign)
                {
                    zc_event = 1;
                    if (last_zc_time >= 0.0)
                    {
                        double halfT = t - last_zc_time; /* ~180° crossing interval */
                        double delay = halfT / 6.0;      /* +30° */
                        commute_due = t + delay;
                    }
                    else
                    {
                        double omega_e = fmax(1e-6, mp.pole_pairs * ms.omega_m);
                        commute_due = t + (M_PI / 6.0) / omega_e;
                    }
                    last_zc_time = t;
                    last_sign = sign;
                }
                if (commute_due >= 0.0 && t >= commute_due)
                {
                    zc_sector = (zc_sector % 6) + 1;
                    commute_due = -1.0;
                    blank_until = t + blank;
                    /* refresh sign for new floating phase immediately */
                    int nf = float_phase_for_sector(zc_sector);
                    double fa2 = trap120(theta_e_true);
                    double fb2 = trap120(theta_e_true - 2.0 * M_PI / 3.0);
                    double fc2 = trap120(theta_e_true + 2.0 * M_PI / 3.0);
                    double ef2 = (nf == 0 ? fa2 : (nf == 1 ? fb2 : fc2)) * mp.Ke * ms.omega_m;
                    last_sign = (ef2 >= 0) ? +1 : -1;
                }
                voltages_sixstep(zc_sector, Vdc, duty, &va, &vb, &vc);
            }
        }
        else if (!strcmp(mode, "foc"))
        {
            /* ---- measurements ---- */
            /* Clarke */
            double i_alpha = ms.ia;
            double i_beta = (ms.ia + 2.0 * ms.ib) / sqrt(3.0);
            /* Park (use true θ_e for simulation) */
            double ce = cos(theta_e_true), se = sin(theta_e_true);
            double i_d = ce * i_alpha + se * i_beta;
            double i_q = -se * i_alpha + ce * i_beta;

            /* outer speed PI → iq* */
            double e_rpm = target_rpm - rpm;
            double iq_ref = pi_step(&spd, e_rpm, dt) * iq_max; /* map 0..1 → 0..iq_max */
            iq_ref = clamp(iq_ref, -iq_max, iq_max);

            /* inner PI for id, iq (volts). No feedforward for simplicity. */
            pi_id.out_min = pi_iq.out_min = -1e9;
            pi_id.out_max = pi_iq.out_max = +1e9;

            double vd = pi_step(&pi_id, (id_ref - i_d), dt);
            double vq = pi_step(&pi_iq, (iq_ref - i_q), dt);

            /* voltage vector limit to linear SVPWM region: Vmax ≈ Vdc/√3 */
            double Vmax = vmax_scale * Vdc;
            double Vmag = hypot(vd, vq);
            if (Vmag > Vmax)
            {
                double scale = Vmax / (Vmag + 1e-12);
                vd *= scale;
                vq *= scale;
                /* simple anti-windup: back off integrators when clamped */
                /* (cheap: freeze integrators by undoing last integration) */
                /* Not strictly necessary; PIs above already gate by saturation */
            }

            /* i_dq -> i_αβ inverse Park */
            double v_alpha = ce * vd - se * vq;
            double v_beta = se * vd + ce * vq;

            /* αβ -> phase-to-neutral voltages target (for logging only) */
            double vaN = v_alpha;
            double vbN = -0.5 * v_alpha + (sqrt(3.0) / 2.0) * v_beta;
            double vcN = -0.5 * v_alpha - (sqrt(3.0) / 2.0) * v_beta;

            /* Map to PWM duties (0..1). Center by subtracting average later. */
            da = clamp(0.5 + vaN / Vdc, 0.0, 1.0);
            db = clamp(0.5 + vbN / Vdc, 0.0, 1.0);
            dc = clamp(0.5 + vcN / Vdc, 0.0, 1.0);

            /* current limit (rough): if any phase over ilim, scale duties */
            if (ilim > 0.0)
            {
                double imax = abs3max(ms.ia, ms.ib, ms.ic);
                if (imax > ilim)
                {
                    double sc = ilim / (imax + 1e-9);
                    da = 0.5 + (da - 0.5) * sc;
                    db = 0.5 + (db - 0.5) * sc;
                    dc = 0.5 + (dc - 0.5) * sc;
                }
            }

            /* Convert duties → centered phase voltages for plant (sum=0) */
            double davg = (da + db + dc) / 3.0;
            va = Vdc * (da - davg);
            vb = Vdc * (db - davg);
            vc = Vdc * (dc - davg);

            /* A single 'duty' magnitude metric for CSV (normalized voltage use) */
            duty = clamp(Vmag / (Vmax + 1e-12), 0.0, 1.0);
        }
        else
        {
            fprintf(stderr, "Unknown mode '%s'\n", mode);
            return 1;
        }

        /* ======== integrate plant ======== */
        double Te = 0.0;
        motor_step(&mp, &ms, va, vb, vc, load_torque, dt, &Te);

        /* recompute theta_e for logging */
        theta_e_true = wrap2pi(mp.pole_pairs * ms.theta_m);
        int sector_log = (!strcmp(mode, "foc")) ? sector60(theta_e_true)
                                                : (!strcmp(mode, "sensorless") ? zc_sector : sec_true);

        /* log */
        if (t >= next_log)
        {
            printf("%.6f,%.2f,%s,%d,%d,%d,%.4f,%.4f,%.4f,%.5f,%.2f,%.4f,%.4f,%.4f,%.4f\n",
                   t, rpm, mode, sector_log, hall, zc_event,
                   ms.ia, ms.ib, ms.ic, Te, rad2deg(theta_e_true),
                   duty, da, db, dc);
            next_log += logdt;
        }

        t += dt;
    }

    return 0;
}
