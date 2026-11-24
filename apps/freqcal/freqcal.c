/* SPDX-License-Identifier: MIT
 * FREQCAL - reference-oscillator calibration helper for the IC-705 / IC-9700 (ICAP v1rc app).
 *
 * Listens to a known, GPS-locked carrier (your GPSDO's 10 MHz, RWM, WWV, a GPS-locked beacon)
 * in USB or LSB, measures the audio tone with sub-0.01 Hz precision (coarse DFT search, then a
 * phase-slope fit) and shows how far the radio's reference is off (Hz and ppm).  With a known
 * REF Adjust step size it tells you how many steps to change REF Adjust, e.g. "REF 43 -> 45".
 * A calibration routine (cal=N in FREQCAL.CFG) measures the step size itself.
 *
 * The app never changes any radio setting: it only listens and advises.
 * Built only against the public SDK header icap.h.
 */
#include "icap.h"

/* ------------------------------------------------------------------ libc bits */
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

/* ------------------------------------------------------------------ math (double) */
#define PI   3.14159265358979323846
#define TWOPI 6.28318530717958647692

static inline double fsqrt(double x) { return __builtin_sqrt(x); }   /* vsqrt.f64 */
static inline double fabsd(double x) { return x < 0 ? -x : x; }

static void fsincos(double x, double *s, double *c)
{
    /* range reduction to [-pi, pi], then to [-pi/4, pi/4] by quadrant */
    double k = x / TWOPI;
    long long n = (long long)(k >= 0 ? k + 0.5 : k - 0.5);
    x -= (double)n * TWOPI;
    int q = 0;
    double qf = x / (PI / 2);
    q = (int)(qf >= 0 ? qf + 0.5 : qf - 0.5);
    double r = x - q * (PI / 2);
    double r2 = r * r;
    double sn = r * (1 - r2 / 6 * (1 - r2 / 20 * (1 - r2 / 42 * (1 - r2 / 72 * (1 - r2 / 110 * (1 - r2 / 156))))));
    double cs = 1 - r2 / 2 * (1 - r2 / 12 * (1 - r2 / 30 * (1 - r2 / 56 * (1 - r2 / 90 * (1 - r2 / 132)))));
    switch (q & 3) {
    case 0: *s = sn;  *c = cs;  break;
    case 1: *s = cs;  *c = -sn; break;
    case 2: *s = -sn; *c = -cs; break;
    default:*s = -cs; *c = sn;  break;
    }
}

static double fatan(double x)
{
    int neg = x < 0; if (neg) x = -x;
    int inv = x > 1; if (inv) x = 1 / x;
    /* halve the angle three times: atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) */
    for (int i = 0; i < 3; i++) x = x / (1 + fsqrt(1 + x * x));
    double x2 = x * x, t = x, s = 0;
    for (int k = 0; k < 12; k++) { s += t / (2 * k + 1); t *= -x2; }
    s *= 8;
    if (inv) s = PI / 2 - s;
    return neg ? -s : s;
}

static double fatan2(double y, double x)
{
    if (x > 0) return fatan(y / x);
    if (x < 0) return y >= 0 ? fatan(y / x) + PI : fatan(y / x) - PI;
    return y > 0 ? PI / 2 : (y < 0 ? -PI / 2 : 0);
}

static double flog10(double x)       /* x > 0 */
{
    if (x <= 0) return -99;
    int e = 0;
    while (x >= 2) { x *= 0.5; e++; }
    while (x < 1) { x *= 2; e--; }
    /* ln(x) for x in [1,2): ln(x) = 2 atanh(u), u = (x-1)/(x+1) */
    double u = (x - 1) / (x + 1), u2 = u * u, t = u, s = 0;
    for (int k = 0; k < 14; k++) { s += t / (2 * k + 1); t *= u2; }
    double ln = 2 * s + e * 0.69314718055994530942;
    return ln * 0.43429448190325182765;
}

/* ------------------------------------------------------------------ text helpers */
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

static char *put_u(char *p, unsigned long long v, int width)
{
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && n < 22);
    while (n < width) t[n++] = '0';
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

static __attribute__((unused)) char *put_i(char *p, long long v, int plus)
{
    if (v < 0) { *p++ = '-'; v = -v; } else if (plus) *p++ = '+';
    return put_u(p, (unsigned long long)v, 1);
}

/* fixed-point decimal: dec digits after the point, optional leading '+' */
static char *put_f(char *p, double v, int dec, int plus)
{
    if (v != v) return put_s(p, "nan");
    if (v < 0) { *p++ = '-'; v = -v; } else if (plus) *p++ = '+';
    double sc = 1; for (int i = 0; i < dec; i++) sc *= 10;
    unsigned long long w = (unsigned long long)(v * sc + 0.5);
    unsigned long long ip = w, fp = 0, m = 1;
    for (int i = 0; i < dec; i++) m *= 10;
    if (dec) { ip = w / m; fp = w % m; }
    p = put_u(p, ip, 1);
    if (dec) { *p++ = '.'; p = put_u(p, fp, dec); }
    *p = 0;
    return p;
}

static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* parse a decimal number with optional sign and fraction; returns chars used (0 = none) */
static int parse_num(const char *s, double *out)
{
    const char *p = s; double sign = 1, v = 0; int any = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '+') p++; else if (*p == '-') { sign = -1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    if (*p == '.') {
        double f = 0.1; p++;
        while (*p >= '0' && *p <= '9') { v += (*p - '0') * f; f *= 0.1; p++; any = 1; }
    }
    if (!any) return 0;
    *out = sign * v;
    return (int)(p - s);
}

/* ------------------------------------------------------------------ config */
static struct {
    double ref_hz;      /* true frequency of the reference carrier                  */
    int    ref_auto;    /* 1 = pick the standard-frequency station from the dial      */
    double search_hz;   /* coarse search half-width (Hz); 0 = auto: search_ppm * ref  */
    double search_ppm;  /* auto search half-width in ppm of the reference            */
    int    reacq;       /* re-run the coarse search every N good windows (0 = never) */
    double win_s;       /* one measurement window                                   */
    double step_ppm;    /* error change (ppm) per +1 REF Adjust unit; 0 = unknown    */
    int    has_step;
    double ref_now;     /* the REF Adjust value now set (menu %); <0 = unknown        */
    double cal;         /* !=0: run the step calibration with this REF change (menu %) */
    double min_snr;     /* dB (bin S/N) needed to accept a window                    */
    double max_rms;     /* max phase-fit residual (rad); 99 = not used               */
    double min_sep;     /* dB the strongest carrier must exceed a 2nd one             */
    int    log;         /* write FREQCAL.TXT                                         */
} cfg;

static void cfg_defaults(void)
{
    cfg.ref_hz = 10000000.0; cfg.ref_auto = 1; cfg.search_hz = 0; cfg.search_ppm = 1.5; cfg.reacq = 3; cfg.win_s = 20;
    cfg.step_ppm = 0.10; cfg.has_step = 1;   /* IC-705: about 0.1 ppm per 1 % REF Adjust (measured) */
    cfg.ref_now = -1; cfg.cal = 0; cfg.min_snr = 15; cfg.max_rms = 99; cfg.min_sep = 6; cfg.log = 1;
}

static char cfgbuf[2048];

static void cfg_load(void)
{
    int fd = icap_open("FREQCAL.CFG", ICAP_O_READ);
    if (fd < 0) return;
    int n = icap_read(fd, cfgbuf, sizeof cfgbuf - 1);
    icap_close(fd);
    if (n <= 0) return;
    cfgbuf[n] = 0;
    char *line = cfgbuf;
    while (*line) {
        char *e = line; while (*e && *e != '\n' && *e != '\r') e++;
        char save = *e; *e = 0;
        char *h = line; while (*h && *h != '#') h++; *h = 0;           /* strip comment */
        char *eq = line; while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = 0;
            char *k = line; while (*k == ' ' || *k == '\t') k++;
            char *ke = eq; while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
            double v;
            if (parse_num(eq + 1, &v)) {
                if (str_eq(k, "ref")) { cfg.ref_hz = v; cfg.ref_auto = v <= 0; }
                else if (str_eq(k, "search")) cfg.search_hz = v;
                else if (str_eq(k, "searchppm")) cfg.search_ppm = v;
                else if (str_eq(k, "reacq")) cfg.reacq = (int)v;
                else if (str_eq(k, "win")) cfg.win_s = v;
                else if (str_eq(k, "step")) { cfg.step_ppm = v; cfg.has_step = v != 0; }
                else if (str_eq(k, "refnow")) cfg.ref_now = v;
                else if (str_eq(k, "cal")) cfg.cal = v;
                else if (str_eq(k, "minsnr")) cfg.min_snr = v;
                else if (str_eq(k, "maxrms")) cfg.max_rms = v;
                else if (str_eq(k, "minsep")) cfg.min_sep = v;
                else if (str_eq(k, "log")) cfg.log = v != 0;
            }
        }
        *e = save;
        line = e; while (*line == '\n' || *line == '\r') line++;
    }
    if (cfg.search_hz < 0) cfg.search_hz = 0;
    if (cfg.search_hz > 80) cfg.search_hz = 80;
    if (cfg.win_s < 5) cfg.win_s = 5;
    if (cfg.win_s > 120) cfg.win_s = 120;
}

/* ------------------------------------------------------------------ radio state */
struct dial { double f; int usb; int trv; int ok; };

static int read_vfile(const char *name, char *buf, int max)
{
    int fd = icap_open(name, ICAP_O_READ);
    if (fd < 0) return fd;
    int n = icap_read(fd, buf, (uint32_t)(max - 1));
    icap_close(fd);
    if (n < 0) return n;
    buf[n] = 0;
    return n;
}

static const char *field(const char *s, const char *key)   /* "k=v k=v": pointer to value of key */
{
    int kl = 0; while (key[kl]) kl++;
    const char *p = s;
    while (*p) {
        while (*p == ' ') p++;
        int i = 0; while (i < kl && p[i] == key[i]) i++;
        if (i == kl && p[kl] == '=') return p + kl + 1;
        while (*p && *p != ' ') p++;
    }
    return 0;
}

static void get_dial(struct dial *d)
{
    char b[128];
    d->ok = 0; d->usb = 1; d->trv = 0; d->f = 0;
    if (read_vfile("@FREQ", b, sizeof b) > 0) {
        const char *v; double x;
        if ((v = field(b, "f")) && parse_num(v, &x)) { d->f = x; d->ok = 1; }
        if ((v = field(b, "usb")) && parse_num(v, &x)) d->usb = x != 0;
        if ((v = field(b, "trv")) && parse_num(v, &x)) d->trv = (int)x;
    }
    if (!d->ok) {                                /* older firmware: MAIN dial, assume USB */
        struct icap_radio rs;
        if (icap_radio_state(&rs) == 0 && rs.freq_hz) { d->f = rs.freq_hz; d->ok = 1; }
    }
}

/* ------------------------------------------------------------------ DSP */
#define FS      8000
#define D1      40                    /* 8000 -> 200 Hz complex          */
#define FS1     (FS / D1)
#define D2      20                    /* 200 -> 10 Hz complex            */
#define FS2     (FS1 / D2)
#define ACQ_S   4
#define ACQ_N   (FS1 * ACQ_S)         /* 800 complex samples for the search */

static float acq_re[ACQ_N], acq_im[ACQ_N];

struct nco { double c, s, dc, ds; int n; };
static void nco_set(struct nco *o, double f, double fs)
{
    double s, c; fsincos(TWOPI * f / fs, &s, &c);
    o->c = 1; o->s = 0; o->dc = c; o->ds = s; o->n = 0;
}
static inline void nco_step(struct nco *o)
{
    double c = o->c * o->dc - o->s * o->ds;
    double s = o->c * o->ds + o->s * o->dc;
    o->c = c; o->s = s;
    if (++o->n >= 1024) { double m = 1.0 / fsqrt(c * c + s * s); o->c *= m; o->s *= m; o->n = 0; }
}

enum { CAL_OFF, CAL_A, CAL_WAIT, CAL_SETTLE, CAL_B, CAL_DONE };
static int cal_st = CAL_OFF; static double cal_a, cal_b; static int cal_n; static uint32_t cal_t;
/* 5 s block tones: baseline statistics (CAL_A) and step detection (CAL_WAIT) */
static double blkA_s, blkA_q; static int blkA_n; static int jump_hits, jump_sign;
#define CAL_TIMEOUT_MS 180000u
static void log_line(const char *s);

enum { ST_WAIT, ST_ACQ, ST_FINE };

static struct {
    int st;
    double tone0;          /* expected tone (Hz) from ref and dial    */
    struct nco lo;         /* 8 kHz mixer at tone0                    */
    double a1r, a1i; int n1;
    int nacq;              /* samples collected for the search        */
    double fc;             /* coarse offset from tone0 (Hz)           */
    double snr_db;
    struct nco lo2;        /* 200 Hz derotator at fc                  */
    double a2r, a2i; int n2;
    /* phase-slope fit */
    int    nfit; double ph_prev, unwrap; double st_, sp, stt, stp, spp;
    double amp_sum;
    double blk_d; int blk_k;   /* 5 s block of phase steps */
    int    need;           /* fit points per window                   */
    uint32_t t_start;
} m;

static void dsp_reset(double tone0)
{
    memset(&m, 0, sizeof m);
    m.tone0 = tone0;
    nco_set(&m.lo, tone0, FS);
    m.st = ST_ACQ;
    m.need = (int)(cfg.win_s * FS2 + 0.5);
    m.t_start = icap_ticks();
}


static double search_width(void)
{
    double w = cfg.search_hz > 0 ? cfg.search_hz : cfg.search_ppm * 1e-6 * cfg.ref_hz;
    if (w < 3) w = 3;
    if (w > 80) w = 80;
    return w;
}
/* coarse DFT search over +-search_hz in 0.05 Hz steps on the 200 Hz stream */
static void acquire(void)
{
    double best = -1, sum = 0; int nb = 0; double fbest = 0;
    int steps = (int)(search_width() / 0.05 + 0.5);
    static float pw[3300];
    if (steps > 1600) steps = 1600;
    for (int k = -steps; k <= steps; k++) {
        double f = k * 0.05;
        double s, c; fsincos(-TWOPI * f / FS1, &s, &c);
        double pr = 1, pi = 0, xr = 0, xi = 0;
        for (int i = 0; i < ACQ_N; i++) {
            xr += acq_re[i] * pr - acq_im[i] * pi;
            xi += acq_re[i] * pi + acq_im[i] * pr;
            double t = pr * c - pi * s; pi = pr * s + pi * c; pr = t;
        }
        double p = xr * xr + xi * xi;
        pw[k + steps] = (float)p;
        if (p > best) { best = p; fbest = f; }
        if ((k & 63) == 0) { if (icap_should_stop()) return; icap_yield(); }
    }
    /* noise: mean power of the bins away from the peak (> 1 Hz) */
    for (int k = -steps; k <= steps; k++) {
        double f = k * 0.05;
        if (fabsd(f - fbest) > 1.0) { sum += pw[k + steps]; nb++; }
    }
    double noise = nb ? sum / nb : 1e-30;
    m.snr_db = 10 * flog10(best / (noise > 1e-30 ? noise : 1e-30));
    m.fc = fbest;
}

static void fit_reset(void)
{
    m.nfit = 0; m.unwrap = 0; m.st_ = m.sp = m.stt = m.stp = m.spp = 0; m.amp_sum = 0;
    m.blk_d = 0; m.blk_k = 0;
    m.t_start = icap_ticks();
}

static void fine_start(void)
{
    nco_set(&m.lo2, m.fc, FS1);
    m.a2r = m.a2i = 0; m.n2 = 0;
    fit_reset();
    m.st = ST_FINE;
}

static char *put_f(char *p, double v, int dec, int plus);
static char *put_s(char *p, const char *s);

/* one 5 s block tone; returns 1 when the running window must restart */
static int cal_block(double tone)
{
    if (cal_st == CAL_A) { blkA_s += tone; blkA_q += tone * tone; blkA_n++; return 0; }
    if (cal_st == CAL_WAIT && blkA_n >= 3) {
        double mean = blkA_s / blkA_n, var = blkA_q / blkA_n - mean * mean;
        double sd = var > 0 ? fsqrt(var) : 0, thr = 4 * sd;
        if (thr < 0.2) thr = 0.2;
        double dev = tone - mean; int sg = dev > 0 ? 1 : -1;
        if (fabsd(dev) > thr) {
            if (jump_hits && sg != jump_sign) jump_hits = 0;
            jump_sign = sg; jump_hits++;
        } else jump_hits = 0;
        if (jump_hits >= 2) {
            char b[96], *p = put_s(b, "CAL change seen: ");
            p = put_f(p, dev, 3, 1); p = put_s(p, " Hz (thr "); p = put_f(p, thr, 3, 0); put_s(p, ")");
            log_line(b);
            cal_st = CAL_SETTLE; jump_hits = 0;
        }
        return 0;
    }
    if (cal_st == CAL_SETTLE) { cal_st = CAL_B; cal_n = 0; cal_b = 0; return 1; }
    return 0;
}

/* result of one window */
struct res { double tone, df, rms, amp, sep_db, df2; int ok, two; };

#define ZB_MAX 1200                 /* 120 s at 10 Hz */
static float zb_re[ZB_MAX], zb_im[ZB_MAX];

/* zoom DFT of the window around 0 Hz: strongest carrier (interpolated) and the next one */
static void zoom_peaks(int n, struct res *out)
{
    static float pw[601];
    const double span = 1.5, stepf = 0.005; int nb = (int)(2 * span / stepf + 0.5) + 1; if (nb > 601) nb = 601;
    int ib = 0; double pb = -1;
    for (int k = 0; k < nb; k++) {
        double f = -span + k * stepf, sn, cs; fsincos(-TWOPI * f / FS2, &sn, &cs);
        double pr = 1, pi = 0, xr = 0, xi = 0;
        for (int i = 0; i < n; i++) {
            xr += zb_re[i] * pr - zb_im[i] * pi; xi += zb_re[i] * pi + zb_im[i] * pr;
            double t = pr * cs - pi * sn; pi = pr * sn + pi * cs; pr = t;
        }
        double p = xr * xr + xi * xi; pw[k] = (float)p;
        if (p > pb) { pb = p; ib = k; }
    }
    double d = 0;                                     /* parabolic interpolation on log power */
    if (ib > 0 && ib < nb - 1 && pw[ib - 1] > 0 && pw[ib + 1] > 0) {
        double a = flog10(pw[ib - 1]), b = flog10(pw[ib]), c = flog10(pw[ib + 1]), q = a - 2 * b + c;
        if (q < 0) d = 0.5 * (a - c) / q;
    }
    out->df = -span + (ib + d) * stepf;
    /* second carrier: highest local maximum more than 0.25 Hz away from the first */
    double p2 = 0; int i2 = -1;
    for (int k = 1; k < nb - 1; k++) {
        double f = -span + k * stepf;
        if (fabsd(f - out->df) < 0.25) continue;
        if (pw[k] >= pw[k - 1] && pw[k] >= pw[k + 1] && pw[k] > p2) { p2 = pw[k]; i2 = k; }
    }
    out->sep_db = p2 > 0 ? 10 * flog10(pb / p2) : 99;
    out->df2 = i2 >= 0 ? -span + i2 * stepf : 0;
}

static int fine_point(double zr, double zi, struct res *out)
{
    double ph = fatan2(zi, zr);
    if (m.nfit == 0) { m.ph_prev = ph; m.unwrap = ph; }
    else {
        double d = ph - m.ph_prev;
        while (d > PI) d -= TWOPI;
        while (d < -PI) d += TWOPI;
        m.unwrap += d; m.ph_prev = ph;
        m.blk_d += d;
        if (++m.blk_k >= 5 * FS2) {
            double tone = m.tone0 + m.fc + (m.blk_d / m.blk_k) * FS2 / TWOPI;
            m.blk_d = 0; m.blk_k = 0;
            if (cal_block(tone)) { fit_reset(); return 0; }
        }
    }
    double t = (double)m.nfit / FS2;
    m.st_ += t; m.sp += m.unwrap; m.stt += t * t; m.stp += t * m.unwrap; m.spp += m.unwrap * m.unwrap;
    m.amp_sum += fsqrt(zr * zr + zi * zi);
    if (m.nfit < ZB_MAX) { zb_re[m.nfit] = (float)zr; zb_im[m.nfit] = (float)zi; }
    m.nfit++;
    if (m.nfit < m.need) return 0;
    double n = m.nfit, den = n * m.stt - m.st_ * m.st_;
    double slope = (n * m.stp - m.st_ * m.sp) / den;              /* rad/s */
    double icpt = (m.sp - slope * m.st_) / n;
    double sse = m.spp - 2 * icpt * m.sp - 2 * slope * m.stp + n * icpt * icpt + 2 * icpt * slope * m.st_ + slope * slope * m.stt;
    if (sse < 0) sse = 0;
    out->df = slope / TWOPI;                                 /* phase-fit estimate (single carrier) */
    zoom_peaks(m.nfit < ZB_MAX ? m.nfit : ZB_MAX, out);     /* strongest carrier, robust to a 2nd one */
    out->two = out->sep_db < cfg.min_sep;
    out->tone = m.tone0 + m.fc + out->df;
    out->rms = fsqrt(sse / n);
    out->amp = m.amp_sum / n;
    out->ok = 1;
    return 1;
}

/* one 8 kHz sample in; returns 1 when a window result is ready */
static int dsp_sample(int16_t x, struct res *out)
{
    double v = x;
    m.a1r += v * m.lo.c; m.a1i -= v * m.lo.s;      /* x * conj(lo) */
    nco_step(&m.lo);
    if (++m.n1 < D1) return 0;
    double zr = m.a1r / D1, zi = m.a1i / D1;
    m.a1r = m.a1i = 0; m.n1 = 0;
    if (m.st == ST_ACQ) {
        if (m.nacq < ACQ_N) { acq_re[m.nacq] = (float)zr; acq_im[m.nacq] = (float)zi; m.nacq++; }
        return 0;                                  /* acquire() is run from the main loop */
    }
    if (m.st != ST_FINE) return 0;
    double r = zr * m.lo2.c + zi * m.lo2.s, i = zi * m.lo2.c - zr * m.lo2.s;   /* * conj(lo2) */
    nco_step(&m.lo2);
    m.a2r += r; m.a2i += i;
    if (++m.n2 < D2) return 0;
    double ar = m.a2r / D2, ai = m.a2i / D2;
    m.a2r = m.a2i = 0; m.n2 = 0;
    return fine_point(ar, ai, out);
}

/* ------------------------------------------------------------------ results / UI */
#define HIST 6
static double hist[HIST]; static int nhist;
static int logfd = -1;
static char L[8][64];

static void line(int i) { icap_set_line(i, L[i]); }

static double expected_tone(const struct dial *d)
{
    return d->usb ? cfg.ref_hz - d->f : d->f - cfg.ref_hz;
}

/* radio reference error (ppm), positive = radio reads high (its LO is above the dial) */
static double err_ppm(const struct dial *d, double tone)
{
    double actual = d->usb ? cfg.ref_hz - tone : cfg.ref_hz + tone;   /* where the radio really is */
    return (actual - d->f) / d->f * 1e6;
}

static void hist_add(double e) {
    if (nhist < HIST) hist[nhist++] = e;
    else { for (int i = 1; i < HIST; i++) hist[i - 1] = hist[i]; hist[HIST - 1] = e; }
}
static void hist_stats(double *mean, double *sd)
{
    /* median and robust spread (1.4826 * median absolute deviation): one bad window cannot pull the result */
    double v[HIST], dv[HIST]; int n = nhist;
    for (int i = 0; i < n; i++) v[i] = hist[i];
    for (int i = 1; i < n; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    *mean = n ? (n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2])) : 0;
    for (int i = 0; i < n; i++) dv[i] = fabsd(v[i] - *mean);
    for (int i = 1; i < n; i++) { double x = dv[i]; int j = i - 1; while (j >= 0 && dv[j] > x) { dv[j + 1] = dv[j]; j--; } dv[j + 1] = x; }
    *sd = n > 1 ? 1.4826 * (n & 1 ? dv[n / 2] : 0.5 * (dv[n / 2 - 1] + dv[n / 2])) : 0;
}

static void log_line(const char *s)
{
    if (logfd < 0) return;
    int n = 0; while (s[n]) n++;
    icap_write(logfd, s, (uint32_t)n);
    icap_write(logfd, "\r\n", 2);
}

static void stamp(char *p)
{
    struct icap_time t;
    if (icap_clock_now(&t) == 0) {
        p = put_u(p, t.year, 4); *p++ = '-'; p = put_u(p, t.mon, 2); *p++ = '-'; p = put_u(p, t.day, 2);
        *p++ = ' '; p = put_u(p, t.hour, 2); *p++ = ':'; p = put_u(p, t.min, 2); *p++ = ':'; p = put_u(p, t.sec, 2);
        *p = 0;
    } else { p = put_s(p, "t="); put_u(p, icap_ticks() / 1000, 1); }
}


static void show_advice(void)
{
    double mean, sd; hist_stats(&mean, &sd);
    char *p;
    if (nhist) {
        p = put_s(L[5], "median("); p = put_u(p, (unsigned)nhist, 1); p = put_s(p, "): ");
        p = put_f(p, mean, 4, 1); p = put_s(p, " ppm sd "); p = put_f(p, sd, 4, 0);
        if (fabsd(mean) > 1.0) put_s(p, " CHECK");
    } else put_s(L[5], "avg: -");
    line(5);
    if (cfg.has_step && nhist) {
        double d = -mean / cfg.step_ppm;                         /* % change needed */
        long long r = (long long)(d * 10 + (d >= 0 ? 0.5 : -0.5)); /* in 0.1 % */
        if (r == 0) put_s(L[6], "OK: REF Adjust is right");
        else {
            p = put_s(L[6], mean < 0 ? "radio LOW -> INCREASE REF " : "radio HIGH -> DECREASE REF ");
            p = put_s(p, "~"); p = put_f(p, r < 0 ? -r / 10.0 : r / 10.0, 1, 0); put_s(p, " %");
        }
        /* what the average error means on the VHF/UHF bands (radio's own error, no transverter) */
        p = put_s(L[7], "144 MHz: "); p = put_f(p, mean * 144.0, 1, 1); p = put_s(p, " Hz  432 MHz: ");
        p = put_f(p, mean * 432.0, 1, 1); put_s(p, " Hz");
    } else if (!cfg.has_step) {
        if (cal_st != CAL_OFF) put_s(L[6], "step unknown: calibration running");
        else put_s(L[6], "step unknown: set cal=1.0 or step=");
        if (cfg.ref_now >= 0) { char *q = put_s(L[7], "REF Adjust now "); q = put_f(q, cfg.ref_now, 1, 0); put_s(q, "%"); }
        else put_s(L[7], "in FREQCAL.CFG (see README)");
    }
    line(6); line(7);
}

/* ------------------------------------------------------------------ calibration state */

static void cal_result(struct res *r, double e)
{
    (void)r;
    char b[64], *p;
    if (cal_st == CAL_A) {
        cal_a += e; cal_n++;
        if (cal_n >= 2) {
            cal_a /= cal_n; cal_n = 0; cal_st = CAL_WAIT; cal_t = icap_ticks(); jump_hits = 0;
            if (cfg.ref_now >= 0) {
                p = put_s(b, "Set REF Adjust to ");
                p = put_f(p, cfg.ref_now + cfg.cal, 1, 0); p = put_s(p, "%\n(now "); p = put_f(p, cfg.ref_now, 1, 0); p = put_s(p, "%, ");
            } else { p = put_s(b, "Change REF Adjust by\n"); p = put_f(p, cfg.cal, 1, 1); p = put_s(p, "% ("); }
            put_s(p, "exactly)\nMENU > SET > Function >\nREF Adjust. Keep the dial.\nFREQCAL detects the change\nby itself (3 min).");
            icap_popup(b);
            p = put_s(b, "CAL A = "); p = put_f(p, cal_a, 5, 1); put_s(p, " ppm"); log_line(b);
        }
    } else if (cal_st == CAL_B) {
        cal_b += e; cal_n++;
        if (cal_n >= 2) {
            cal_b /= cal_n;
            double step = (cal_b - cal_a) / cfg.cal;
            cfg.step_ppm = step; cfg.has_step = step != 0; cal_st = CAL_DONE;
            if (cfg.ref_now >= 0) cfg.ref_now += cfg.cal;
            nhist = 0; hist_add(cal_b);
            p = put_s(b, "CAL B = "); p = put_f(p, cal_b, 5, 1); put_s(p, " ppm"); log_line(b);
            p = put_s(b, "step="); p = put_f(p, step, 5, 1); put_s(p, "  (ppm per +1 % REF Adjust)"); log_line(b);
            p = put_s(b, "Step measured:\n"); p = put_f(p, step, 5, 1);
            p = put_s(p, " ppm per %\nPut in FREQCAL.CFG: step=\n(value above) and cal=0.");
            if (step < 0) put_s(p, "\nNegative: if you set it\nLOWER, flip the sign.");
            icap_popup(b);
        }
    }
}


/* ------------------------------------------------------------------ standard-frequency stations */
static const struct { double f; const char *n; } stdf[] = {
    {  2500000, "WWV/BPM" }, {  3330000, "CHU" }, {  4996000, "RWM" }, {  5000000, "WWV/BPM" },
    {  7850000, "CHU" },     {  9996000, "RWM" }, { 10000000, "WWV/BPM/GPSDO" }, { 14670000, "CHU" },
    { 14996000, "RWM" },     { 15000000, "WWV/BPM" }, { 20000000, "WWV" }, { 25000000, "WWV" },
};
#define NSTD ((int)(sizeof stdf / sizeof stdf[0]))
static const char *ref_name = "";

static void show_title(void)
{
    char *p = put_s(L[0], "FREQCAL ref "); p = put_f(p, cfg.ref_hz / 1e6, 6, 0); p = put_s(p, " MHz ");
    put_s(p, ref_name);
    line(0);
}

/* choose the reference for this dial; 0 = nothing usable (line 2 then holds a tuning hint) */
static int pick_ref(const struct dial *d)
{
    if (!cfg.ref_auto) {
        double t = expected_tone(d);
        if (t >= 200 && t <= 3300) return 1;
        char *p = put_s(L[2], "tune ");
        p = put_f(p, (d->usb ? cfg.ref_hz - 1500 : cfg.ref_hz + 1500) / 1e3, 1, 0); put_s(p, d->usb ? " kHz USB" : " kHz LSB");
        return 0;
    }
    int best = -1; double bd = 1e18, near = 1e18; int nb = 0;
    for (int i = 0; i < NSTD; i++) {
        double t = d->usb ? stdf[i].f - d->f : d->f - stdf[i].f;
        if (t >= 200 && t <= 3300 && fabsd(t - 1500) < bd) { bd = fabsd(t - 1500); best = i; }
        if (fabsd(stdf[i].f - d->f) < near) { near = fabsd(stdf[i].f - d->f); nb = i; }
    }
    if (best < 0) {
        char *p = put_s(L[2], "tune ");
        p = put_f(p, (stdf[nb].f - 1500) / 1e3, 1, 0); p = put_s(p, " kHz USB for "); put_s(p, stdf[nb].n);
        return 0;
    }
    if (cfg.ref_hz != stdf[best].f || ref_name != stdf[best].n) {
        cfg.ref_hz = stdf[best].f; ref_name = stdf[best].n; nhist = 0;    /* new station: fresh average */
        show_title();
    }
    return 1;
}

/* ------------------------------------------------------------------ main */
static int16_t abuf[512];

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);
    icap_clear();
    if (svc->abi_version < 3 || icap_audio_rate() != FS) {
        icap_print("FREQCAL needs ICAP v1rc firmware");
        icap_print("with 8 kHz receive audio.");
        while (!icap_should_stop()) icap_sleep_ms(200);
        return 1;
    }
    cfg_defaults();
    cfg_load();
    if (cfg.log) logfd = icap_open("FREQCAL.TXT", ICAP_O_WRITE);
    if (cfg.cal > 0.05 || cfg.cal < -0.05) cal_st = CAL_A;

    char *p;
    show_title();
    log_line("# time dial_hz sb trv tone_hz df_hz err_hz err_ppm snr_db rms_rad sep2_db tone2_hz ok");

    struct dial d = { 0 }, dn;
    struct res r;
    uint32_t last_dial_check = 0, last_ui = 0;
    icap_audio_read(abuf, 1);                      /* start the audio service */

    get_dial(&d);
    if (d.ok && pick_ref(&d)) dsp_reset(expected_tone(&d)); else m.st = ST_WAIT;

    while (!icap_should_stop()) {
        uint32_t now = icap_ticks();

        /* follow the dial; restart the measurement when it moves */
        if (now - last_dial_check > 500) {
            last_dial_check = now;
            get_dial(&dn);
            struct icap_radio rs; int tx = icap_radio_state(&rs) == 0 && rs.is_tx;
            if (tx) { put_s(L[2], "TX - paused"); line(2); m.st = ST_WAIT; }
            else if (dn.ok && (dn.f != d.f || dn.usb != d.usb || m.st == ST_WAIT)) {
                d = dn;
                if (!pick_ref(&d)) { m.st = ST_WAIT; line(2); }
                else dsp_reset(expected_tone(&d));
            }
            p = put_s(L[1], "dial "); p = put_u(p, (unsigned long long)(d.f + 0.5), 1);
            p = put_s(p, d.usb ? " USB" : " LSB");
            if (d.trv) { p = put_s(p, " TRV"); p = put_u(p, (unsigned)d.trv, 1); }
            p = put_s(p, " tone "); put_f(p, expected_tone(&d), 1, 0);
            line(1);
        }

        /* pull audio */
        for (int guard = 0; guard < 32 && m.st != ST_WAIT; guard++) {
            int n = icap_audio_read(abuf, (int)(sizeof abuf / sizeof abuf[0]));
            if (n == ICAP_E_OVERRUN) {               /* gap breaks the phase fit: restart the window */
                if (m.st == ST_FINE) fine_start(); else dsp_reset(m.tone0);
                continue;
            }
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (dsp_sample(abuf[i], &r)) {
                    double e = err_ppm(&d, r.tone);
                    int good = m.snr_db >= cfg.min_snr && r.rms < cfg.max_rms && !r.two;
                    char b[160];
                    /* log */
                    p = b; stamp(p); while (*p) p++;
                    *p++ = ' '; p = put_u(p, (unsigned long long)(d.f + 0.5), 1);
                    p = put_s(p, d.usb ? " U " : " L "); p = put_u(p, (unsigned)d.trv, 1);
                    *p++ = ' '; p = put_f(p, r.tone, 4, 0);
                    *p++ = ' '; p = put_f(p, r.df, 4, 1);
                    *p++ = ' '; p = put_f(p, e * d.f / 1e6, 3, 1);
                    *p++ = ' '; p = put_f(p, e, 5, 1);
                    *p++ = ' '; p = put_f(p, m.snr_db, 1, 0);
                    *p++ = ' '; p = put_f(p, r.rms, 3, 0);
                    *p++ = ' '; p = put_f(p, r.sep_db > 98 ? 99 : r.sep_db, 1, 0);
                    *p++ = ' '; p = put_f(p, m.tone0 + m.fc + r.df2, 3, 0);
                    p = put_s(p, good ? " 1" : " 0");
                    log_line(b);
                    /* screen */
                    p = put_s(L[3], "tone "); p = put_f(p, r.tone, 3, 0); p = put_s(p, " err ");
                    p = put_f(p, e, 4, 1); put_s(p, " ppm");
                    p = put_s(L[4], "     "); p = put_f(p, e * d.f / 1e6, 2, 1); p = put_s(p, " Hz  S/N ");
                    p = put_f(p, m.snr_db, 0, 0); p = put_s(p, " dB");
                    if (!good) {
                        if (m.snr_db < cfg.min_snr) put_s(p, " (low S/N)");
                        else if (r.two) { p = put_s(p, " (2 carriers "); p = put_f(p, r.sep_db, 0, 0); put_s(p, " dB)"); }
                        else { p = put_s(p, " (rms "); p = put_f(p, r.rms, 2, 0); put_s(p, ")"); }
                    }
                    line(3); line(4);
                    if (good) {
                        if (d.trv == 0) hist_add(e);
                        if (cal_st == CAL_A || cal_st == CAL_B) cal_result(&r, e);
                    }
                    if (d.trv) { put_s(L[6], "TRV: error = radio + LO, no REF"); put_s(L[7], "advice (calibrate on a direct band)"); line(6); line(7); }
                    else show_advice();
                    /* next window: keep tracking the measured offset */
                    if (good) {
                        static int nwin = 0;
                        m.fc += r.df;
                        if (fabsd(m.fc) > search_width() || (cfg.reacq > 0 && ++nwin >= cfg.reacq)) { nwin = 0; dsp_reset(m.tone0); break; }
                        fine_start();
                    }
                    else dsp_reset(m.tone0);
                    break;
                }
            }
        }

        if (m.st == ST_ACQ && m.nacq >= ACQ_N) {
            put_s(L[2], "searching carrier..."); line(2);
            acquire();
            if (m.snr_db < cfg.min_snr) {
                p = put_s(L[2], "no clear carrier (S/N "); p = put_f(p, m.snr_db, 0, 0); put_s(p, " dB)");
                line(2);
                dsp_reset(m.tone0);
            } else fine_start();
        }

        if (cal_st == CAL_WAIT && now - cal_t > CAL_TIMEOUT_MS) {
            cal_st = CAL_OFF;
            icap_popup("No REF change detected\nin 3 minutes.\nCalibration stopped.\nRestart FREQCAL to retry.");
            log_line("CAL timeout: no change detected");
        }

        if (now - last_ui > 1000) {
            last_ui = now;
            if (m.st == ST_FINE) {
                unsigned el = (unsigned)(m.nfit / FS2), tot = (unsigned)(m.need / FS2);
                p = put_s(L[2], "measuring "); p = put_u(p, el, 1); *p++ = '/'; p = put_u(p, tot, 1); p = put_s(p, " s");
                if (cal_st == CAL_A) put_s(p, "  CAL: before");
                else if (cal_st == CAL_WAIT) {
                    uint32_t el2 = now - cal_t; unsigned left = el2 < CAL_TIMEOUT_MS ? (unsigned)((CAL_TIMEOUT_MS - el2) / 1000) : 0;
                    p = put_s(p, "  CAL: REF "); p = put_f(p, cfg.cal, 1, 1); p = put_s(p, "% ("); p = put_u(p, left, 1); put_s(p, " s)");
                }
                else if (cal_st == CAL_SETTLE) put_s(p, "  CAL: change seen");
                else if (cal_st == CAL_B) put_s(p, "  CAL: after");
                line(2);
            } else if (m.st == ST_ACQ) {
                p = put_s(L[2], "collecting "); p = put_u(p, (unsigned)(m.nacq / FS1), 1); put_s(p, "/4 s");
                line(2);
            }
        }
        icap_sleep_ms(20);
    }
    if (logfd >= 0) icap_close(logfd);
    return 0;
}
