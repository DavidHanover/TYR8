/* Host test for tyr8: builds the plugin source for the PC and checks what it promises.
 *   ./tyr8_test            run the checks (exit 1 on any FAIL)
 *   ./tyr8_test out DIR    also write the WAVs in DIR (44.1 kHz, as the OP-1 hears it after its 2:1 decimation, without its
 *                          35 Hz high-pass) and DIR/frames.json (what the screen drew; render_frames.py makes PNGs) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness/op1_host.h"
#include "../plugin/tyr8/tyr8.c"

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128]; int16_t host_knobs[8][8]; int16_t host_t2[8][8]; int host_slot;
const uint8_t *host_sb[8]; int16_t *host_sb_data[8];

/* ---- the fake sampler buffers (as in UZ-1's test): slot s's recording ---- */
#define CAP 88200                                 /* 2 s at 44.1 kHz */
static uint32_t sbmem[8][0x1040 / 4];
static int16_t smpb[8][8 + CAP];
static void sb_make(int s)
{
    host_sb_data[s] = smpb[s];
    *(int32_t *)((uint8_t *)sbmem[s] + 0x1038) = CAP + 8;
    host_sb[s] = (const uint8_t *)sbmem[s];
    int16_t *pk = (int16_t *)((uint8_t *)sbmem[s] + 4);
    for (int b = 0; b < OP1_PEAK_BUCKETS; b++) {
        int m = 0;
        for (int i = b * 256; i < b * 256 + 256 && i < CAP; i++) { const int a = abs(smpb[s][8 + i]); if (a > m) m = a; }
        pk[b] = (int16_t)m;
    }
}
/* a kick's attack: silence, then a click (noise) decaying over len samples */
static void make_click(int s, int pre, int len)
{
    memset(smpb[s], 0, sizeof smpb[s]);
    uint32_t r = 777;
    for (int i = 0; i < len; i++) { r = r * 1664525u + 1013904223u; smpb[s][8 + pre + i] = (int16_t)(((int32_t)(r >> 16) - 32768) * exp(-5.0 * i / len) * 0.9); }
    sb_make(s);
}
static void make_tone(int s, int pre, int len, double hz)
{
    memset(smpb[s], 0, sizeof smpb[s]);
    for (int i = 0; i < len; i++) smpb[s][8 + pre + i] = (int16_t)lrint(20000 * sin(2 * M_PI * hz * i / 44100));
    sb_make(s);
}
static void no_samples(void) { for (int s = 0; s < 8; s++) host_sb[s] = 0; src_force = 1; }
#define KEY_C4 (261.6256 / 4)                     /* on() takes the sounding pitch, two octaves under the key */
static void smp_rec(int lvl, int tune, int src)
{ host_eng_rec[0][REC_SLVL] = (int16_t)lvl; host_eng_rec[0][REC_STUNE] = (int16_t)(tune ? tune + 25 : 0); host_eng_rec[0][REC_SRC] = (int16_t)src;
  host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); src_force = 1; }
/* energy above ~2 kHz (a first difference) over [a, b) */
static double treble(size_t a, size_t b);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- audio driver: six voices as the firmware runs them ---- */
static int sag_w;                             /* sag: word 3 (orange) since 0.9.9 */
static int16_t P[8];
static void sag(int v) { sag_w = v; P[3] = (int16_t)v; host_knobs[0][3] = (int16_t)v; }
static void glide_rec(int v) { host_eng_rec[0][REC_GLIDE] = (int16_t)v; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
static void rise(int v) { host_eng_rec[0][REC_RISE] = (int16_t)v; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
static void sub(int v) { host_eng_rec[0][REC_SUB] = (int16_t)v; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
static void mode(int m) { host_eng_rec[0][0] = (int16_t)m; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
#define SINE 8192                             /* the shape knob's plain sine: a quarter up */
#define T2_LO 64                              /* R246's range of the T2 attack, decay and release words */
#define T2_HI 16320
static int tone_w = 32767, atk_w = T2_LO, rel_w = T2_HI;   /* tone open, no attack, release HOLD */
static int active[6]; static double key_hz[6];      /* the key's own frequency, as the firmware gives it */
static int16_t *rec; static size_t rec_n, rec_cap; static int raw_peak;
static unsigned out_voices;        /* which voices put out sound since it was cleared */
static int multi_blocks;           /* blocks in which more than one voice put out sound */
static double porta_hz[6]; static int porta_n[6], porta_i[6];   /* the OP-1's pitch sliding in from porta_hz over porta_n blocks */
static int kill_voice = -1;        /* a voice the OP-1 has killed: not rendered */
static uint32_t audio_hash = 2166136261u;
static int rec_get_mode(void);     /* FNV-1a over every sample rendered: a speed-up must leave it as it was */

/* the sound, in the 0.9.3 helper's terms: knock length and decay 0..32767, sustain and glide 0..127. Tone, attack and
 * release come from tone_w, atk_w and rel_w (T2 words) */
static void set(int osc, int amt, int pdec, int adec, int drive, int clip, int sus, int glide)
{
    P[0] = osc; P[1] = amt; P[2] = tone_w; P[3] = (int16_t)sag_w; P[4] = 0x5300; P[5] = drive; P[6] = clip; P[7] = pdec;
    glide_rec(glide * 258);
    host_t2[0][0] = atk_w; host_t2[0][1] = T2_LO + adec * (T2_HI - T2_LO) / 32767; host_t2[0][2] = sus * 258; host_t2[0][3] = rel_w;
}
static void tone(int open) { tone_w = open; P[2] = open; }
static int t2_of(double frac) { return T2_LO + (int)lrint(frac * (T2_HI - T2_LO)); }   /* a T2 knob at frac of its turn */
/* a Shift + turn through the input hook: its word writes land in host_eng_words, so carry them over to the sound */
static int shift_turn(int a, int delta)
{
    for (int k = 5; k < 8; k++) host_eng_words[0][k] = host_knobs[0][k];
    host_shift_held = 1;
    const int took = ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, a, delta);
    host_shift_held = 0;
    for (int k = 5; k < 8; k++) host_knobs[0][k] = host_eng_words[0][k];
    return took;
}
/* a plain orange turn (sag) through the input hook: word 3 and the record */
static int orange_turn(int delta)
{
    host_eng_words[0][3] = host_knobs[0][3];
    const int took = ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 3, delta);
    sag(host_eng_words[0][3]);
    return took;
}
static void reset(void)
{
    memset((void *)held, 0, sizeof held); owner = -1; memset(tv, 0, sizeof tv);
    rv = -1; own_v = -1; own_stamp = 0; rv_stale = 0;
    memset(active, 0, sizeof active); rec_n = 0;
}
static void on(int v, double f, int velocity)
{ key_hz[v] = f * (1 << OCT_DOWN); active[v] = 1; host_voice_vel[v] = (int16_t)(velocity << 8); ENGINE_tyr8.ext.voice.note_on(OP1_VOICE(v), P); }
static void offk(int v) { ENGINE_tyr8.ext.voice.note_off(OP1_VOICE(v)); }
static void run_ms(double ms)
{
    int blocks = (int)(ms * 88.2 / 128 + 0.5);
    for (int b = 0; b < blocks; b++) {
        int32_t sum[128] = { 0 }; int16_t o[128]; unsigned out_voices_blk = 0;
        for (int v = 0; v < 6; v++) {
            if (!active[v]) continue;
            if (ENGINE_tyr8.ext.finished(OP1_VOICE(v))) { active[v] = 0; continue; }
            for (int n = 0; n < 128; n++) {
                double f = key_hz[v];
                if (porta_i[v] < porta_n[v]) f = porta_hz[v] * pow(key_hz[v] / porta_hz[v], (porta_i[v] * 128 + n) / (128.0 * porta_n[v]));
                host_pitch_inc[n] = (int32_t)(f / 88200.0 * 4294967296.0);
            }
            if (porta_i[v] < porta_n[v]) porta_i[v]++;
            if (v == kill_voice) continue;
            ENGINE_tyr8.ext.voice.render(OP1_VOICE(v), o, P);
            int any = 0; for (int n = 0; n < 128; n++) { sum[n] += o[n]; any |= o[n]; }
            if (any) { if (out_voices_blk) multi_blocks++; out_voices_blk |= 1u << v; out_voices |= 1u << v; }
        }
        if (rec_n + 128 > rec_cap) { rec_cap = rec_cap ? rec_cap * 2 : 1 << 20; rec = realloc(rec, rec_cap * 2); }
        /* recorded as if at full scale (x 32767 / OUT_GAIN), so the checks read levels against the voice's own ceiling;
         * raw_peak keeps what the OP-1 gets */
        for (int n = 0; n < 128; n++) { if (abs(sum[n]) > raw_peak) raw_peak = abs(sum[n]);
            rec[rec_n++] = op1_sat16((int32_t)lrint(sum[n] * (32767.0 / OUT_GAIN))); audio_hash = (audio_hash ^ (uint16_t)sum[n]) * 16777619u; }
    }
}
/* the peak of what was recorded, at the OP-1's level, after the synth bus's 35 Hz high-pass (two poles) */
static double bus_peak(void)
{
    const double a = exp(-2 * M_PI * 35 / 88200.0); double px = 0, y1 = 0, qx = 0, y2 = 0, hp = 0;
    for (size_t i = 0; i < rec_n; i++) {
        const double x = rec[i] * (OUT_GAIN / 32767.0);
        y1 = a * (y1 + x - px); px = x; y2 = a * (y2 + y1 - qx); qx = y1;
        if (fabs(y2) > hp) hp = fabs(y2); }
    return hp;
}
static int peak(size_t a, size_t b) { int p = 0; for (size_t i = a; i < b && i < rec_n; i++) if (abs(rec[i]) > p) p = abs(rec[i]); return p; }
static size_t at(double ms) { return (size_t)(ms * 88.2); }
static double treble(size_t a, size_t b) { double e = 0; for (size_t i = a + 1; i < b && i < rec_n; i++) e += (double)(rec[i] - rec[i - 1]) * (rec[i] - rec[i - 1]); return e; }
/* frequency over [a, b) from rising zero crossings */
static double freq(size_t a, size_t b)
{
    double first = -1, last = -1; int n = 0;
    for (size_t i = a + 1; i < b && i < rec_n; i++)
        if (rec[i - 1] < 0 && rec[i] >= 0) {
            double t = i - 1 + (double)-rec[i - 1] / (rec[i] - rec[i - 1]);
            if (first < 0) first = t;
            last = t; n++;
        }
    return n > 1 ? (n - 1) * 88200.0 / (last - first) : 0;
}
/* how far one cycle is from a scaled copy of the cycle before it, in dB under the signal (a clean decaying tone: very
 * far; noise that does not repeat: close), over [a, b) for a tone of f Hz */
static double tail_snr(size_t a, size_t b, double f)
{
    const size_t T = (size_t)(88200 / f + 0.5);
    double xy = 0, xx = 0, yy = 0;
    for (size_t i = a; i < b && i + T < rec_n; i++) { xy += (double)rec[i] * rec[i + T]; xx += (double)rec[i] * rec[i]; yy += (double)rec[i + T] * rec[i + T]; }
    if (xx == 0 || yy == 0) return 0;
    const double k = xy / xx, err = yy - 2 * k * xy + k * k * xx;
    return 10 * log10(yy / (err > 1e-9 ? err : 1e-9));
}
/* how much of the signal over [a, b) is a sine at f Hz: 1 = all of it */
static double rms_of(size_t a, size_t b) { double e = 0; for (size_t i = a; i < b && i < rec_n; i++) e += (double)rec[i] * rec[i]; return b > a ? e / (b - a) : 0; }
static double sine_part(size_t a, size_t b, double f)
{
    double re = 0, im = 0, e = 0;
    for (size_t i = a; i < b && i < rec_n; i++) { const double w = 2 * M_PI * f * i / 88200; re += rec[i] * cos(w); im += rec[i] * sin(w); e += (double)rec[i] * rec[i]; }
    const double n = (double)((b < rec_n ? b : rec_n) - a);
    return e > 0 ? sqrt(2 * (re * re + im * im) / n / e) : 0;
}
static const char *outdir;
static void wav(const char *name)
{
    if (!outdir) return;
    char path[512]; snprintf(path, sizeof path, "%s/%s.wav", outdir, name);
    FILE *f = fopen(path, "wb"); if (!f) return;
    uint32_t n = (uint32_t)(rec_n / 2), bytes = n * 2, v;
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    uint32_t h[4] = { 16, 0x00010001, 44100, 88200 }; fwrite(h, 4, 4, f); uint16_t t[2] = { 2, 16 }; fwrite(t, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    for (uint32_t i = 0; i < n; i++) { int16_t s = (int16_t)((rec[2 * i] + rec[2 * i + 1]) / 2); fwrite(&s, 2, 1, f); }
    fclose(f);
}

/* ---- screen mocks: every line and filled polygon the screen draws, in order ---- */
typedef struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } hline;
typedef struct { int n; op1_pt p[96]; uint8_t c[4]; } hpoly;
static hline lines[4096]; static int nlines;
static hpoly polys[256]; static int npolys;
static int ops[8192], nops;                   /* >= 0: a line, < 0: polygon -1 - i */
static int dyn_from;                          /* the first op of the moving parts (the labels are a still layer) */
void host_mark_dynamic(void) { dyn_from = nops; }
uint16_t host_frame565[400 * 256];
void host_screen_draw(void *scr, int dt) { (void)scr; (void)dt; }
void *host_raster(void) { return (void *)1; }
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{
    (void)r; const op1_pt *a = p0, *b = p1;
    if (nlines < 4096 && nops < 8192) { hline *l = &lines[nlines]; l->x0 = a->x; l->y0 = a->y; l->x1 = b->x; l->y1 = b->y; l->w = w; memcpy(l->c, c, 4); ops[nops++] = nlines++; }
}
void host_fill_poly(const op1_pt *p, int n, const uint8_t *c)
{
    if (npolys < 256 && nops < 8192 && n <= 96) { hpoly *q = &polys[npolys]; q->n = n; memcpy(q->p, p, n * sizeof *p); memcpy(q->c, c, 4); ops[nops++] = -1 - npolys++; }
}
static FILE *fj; static int nframes;
static void dump(const char *label)
{
    if (!fj) return;
    fprintf(fj, "%s{\"label\":\"%s\",\"dyn\":%d,\"ops\":[", nframes++ ? ",\n" : "", label, dyn_from);
    for (int i = 0; i < nops; i++) {
        if (i) fputc(',', fj);
        if (ops[i] >= 0) { const hline *l = &lines[ops[i]]; fprintf(fj, "[\"L\",%d,%d,%d,%d,%d,%d,%d,%d]", l->x0, l->y0, l->x1, l->y1, l->w, l->c[0], l->c[1], l->c[2]); }
        else {
            const hpoly *q = &polys[-1 - ops[i]];
            fprintf(fj, "[\"P\",[");
            for (int j = 0; j < q->n; j++) fprintf(fj, "%s%d,%d", j ? "," : "", q->p[j].x, q->p[j].y);
            fprintf(fj, "],%d,%d,%d]", q->c[0], q->c[1], q->c[2]);
        }
    }
    fprintf(fj, "]}");
}
static void frame_of(const op1_screen_t *scr, const char *label)
{
    nlines = npolys = nops = dyn_from = 0;
    for (int i = 0; i < 400 * 256; i++) host_frame565[i] = 0xFFFF;
    scr->update(0, 16);
    dump(label);
}
static void frame(const char *label) { frame_of(&SCREEN_tyr8, label); }

/* is the point (Q10 px), m px around it, inside the moving area the screen declares (DYN in ty_screen_update) */
static int in_dyn(double x, double y, double m)
{
    static const int R[5][4] = { { 106, 0, 216, 159 }, { 0, 22, 105, 127 }, { 0, 145, 105, 159 }, { 217, 22, 319, 127 }, { 217, 145, 319, 159 } };
    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) {
        const double px = x / 1024 + dx * m, py = y / 1024 + dy * m;
        int in = 0;
        for (int k = 0; k < 5 && !in; k++) in = px >= R[k][0] && px < R[k][2] + 1 && py >= R[k][1] && py < R[k][3] + 1;
        if (!in) return 0;
    }
    return 1;
}
/* moving ops of the last frame that leave the moving area (lines with their stroke, fill corners) */
static int dyn_escapes(void)
{
    int bad = 0;
    for (int i = dyn_from; i < nops; i++) {
        if (ops[i] >= 0) {
            const hline *l = &lines[ops[i]];
            for (int k = 0; k <= 8; k++)
                if (!in_dyn(l->x0 + (l->x1 - l->x0) * k / 8.0, l->y0 + (l->y1 - l->y0) * k / 8.0, l->w / 2048.0 + 0.25)) {
                    static int shown; if (shown++ < 6) printf("      escape: line (%.1f, %.1f) - (%.1f, %.1f) colour %02x%02x%02x\n", l->x0 / 1024.0, l->y0 / 1024.0, l->x1 / 1024.0, l->y1 / 1024.0, l->c[0], l->c[1], l->c[2]);
                    bad++; break; }
        } else {
            const hpoly *q = &polys[-1 - ops[i]];
            for (int j = 0; j < q->n; j++) if (!in_dyn(q->p[j].x, q->p[j].y, 0.5)) {
                static int shownp; if (shownp++ < 4) printf("      escape: fill corner (%.1f, %.1f)\n", q->p[j].x / 1024.0, q->p[j].y / 1024.0);
                bad++; break; }
        }
    }
    return bad;
}

/* inside a convex polygon (either winding), Q10 px */
static int in_convex(const op1_pt *p, int n, double x, double y)
{
    int pos = 0, neg = 0;
    for (int i = 0; i < n; i++) {
        const op1_pt a = p[i], b = p[(i + 1) % n];
        const double c = (double)(b.x - a.x) * (y - a.y) - (double)(b.y - a.y) * (x - a.x);
        if (c > 0) pos = 1; else if (c < 0) neg = 1;
    }
    return !(pos && neg);
}
/* does a stroke (Q10 px) come within 2 px of the tyre's silhouette or a road line as last drawn */
static int on_tyre(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    for (int k = 0; k <= 8; k++) {
        const double px = x0 + (x1 - x0) * k / 8.0, py = y0 + (y1 - y0) * k / 8.0;
        for (int d = 0; d < 5; d++) {
            const double ox = d == 1 ? 2048 : d == 2 ? -2048 : 0, oy = d == 3 ? 2048 : d == 4 ? -2048 : 0;
            if (sil_n > 2 && in_convex(sil, sil_n, px + ox, py + oy)) return 1;
        }
        for (int i = 0; i < road_nseg; i++) {                   /* within 2 px of a road line */
            const double ax = road_seg[i][0].x, ay = road_seg[i][0].y, bx = road_seg[i][1].x, by_ = road_seg[i][1].y;
            const double dx = bx - ax, dy = by_ - ay, L2 = dx * dx + dy * dy;
            double u = L2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / L2 : 0;
            u = u < 0 ? 0 : (u > 1 ? 1 : u);
            if (hypot(px - ax - u * dx, py - ay - u * dy) < 2048) return 1;
        }
    }
    return 0;
}
/* the near face's outline as last drawn, measured against the road it stands on (which runs diagonally on screen):
 * how wide (along the road) the part within 0.75 px of its lowest point is, and its height above the road */
static void near_shape(double *low, double *flat_w, double *height)
{
    op1_pt nf[LUG_N / 2 * 5];
    wheel_t w = last_wheel;
    const int n = outline(&w, -(int32_t)(ang >> 8), 0, nf);
    double hmin = 1e9, hmax = -1e9, s0 = 1e9, s1 = -1e9;
    for (int i = 0; i < n; i++) {
        const double sx = (nf[i].x / 1024.0 - CON_X) / 0.894, h = (CON_Y - 0.447 * sx) - nf[i].y / 1024.0;
        if (h < hmin) hmin = h;
        if (h > hmax) hmax = h;
    }
    for (int i = 0; i < n; i++) {
        const double sx = (nf[i].x / 1024.0 - CON_X) / 0.894, h = (CON_Y - 0.447 * sx) - nf[i].y / 1024.0;
        if (h < hmin + 0.75) { if (sx < s0) s0 = sx; if (sx > s1) s1 = sx; }
    }
    *low = hmin; *flat_w = s1 - s0; *height = hmax - hmin;
}

static int rec_get_mode(void) { return mode_of(0); }

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "out")) outdir = argv[2];
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767 * sin(2 * M_PI * i / 8192));

    /* 1. one-shot: sounds, decays, ends by itself, and the voice is given back */
    set(SINE, 14000, 9000, 16000, 6000, 8000, 0, 0); reset();
    on(0, 55, 127); run_ms(40); offk(0); run_ms(3000);
    CHECK(peak(0, at(100)) > 32000, "one-shot is loud at the start (peak %d)", peak(0, at(100)));
    CHECK(peak(at(2500), rec_n) == 0 && !active[0], "one-shot ends and frees its voice (tail peak %d, active %d)", peak(at(2500), rec_n), active[0]);
    CHECK(peak(at(60), at(300)) > 4000, "sustain 0: the note rings on after the key is up (peak %d)", peak(at(60), at(300)));
    wav("oneshot_55hz");

    /* 2. no phase randomness: the same hit twice is the same samples */
    reset(); on(0, 55, 127); run_ms(30); offk(0); run_ms(3000);
    size_t t2 = rec_n; on(3, 55, 127); run_ms(30);
    CHECK(!memcmp(rec, rec + t2, at(30) * 2), "two hits from silence are sample-identical");
    CHECK(rec[0] >= 0 && rec[0] < 3000, "a hit starts at phase 0 (first sample %d)", rec[0]);

    /* 3. pitch envelope: the start is far above the note, the tail is on the note */
    reset(); set(SINE, 32767, 16000, 24000, 0, 0, 0, 0); on(0, 55, 127); run_ms(1500);
    double f_start = freq(0, at(12)), f_tail = freq(at(700), at(1400));
    CHECK(f_start > 200, "knock: the first 12 ms run at %.0f Hz", f_start);
    CHECK(fabs(f_tail - 55) < 0.3, "tail is on pitch: %.2f Hz for 55", f_tail);
    reset(); set(SINE, 0, 16000, 24000, 0, 0, 0, 0); on(0, 55, 127); run_ms(400);
    CHECK(fabs(freq(0, at(300)) - 55) < 0.5, "amount 0: no knock (%.2f Hz)", freq(0, at(300)));

    reset(); set(SINE, 0, 16000, 24000, 0, 0, 0, 0); on(0, 55, 127); run_ms(400);
    CHECK(key_hz[0] == 220 && fabs(freq(0, at(300)) - 55) < 0.5, "two octaves down: the key at %.0f Hz sounds at %.1f Hz", key_hz[0], freq(0, at(300)));

    /* 4. amp decay range: short is gone in 0.25 s, long still sounds after 2 s */
    reset(); set(SINE, 0, 0, 0, 0, 0, 0, 0); on(0, 55, 127); run_ms(400);
    CHECK(peak(at(250), rec_n) < 100, "shortest decay is gone after 250 ms (peak %d)", peak(at(250), rec_n));
    reset(); set(SINE, 0, 0, 32767, 0, 0, 0, 0); on(0, 55, 127); run_ms(2500);
    CHECK(peak(at(2000), rec_n) > 1500, "longest decay still sounds after 2 s (peak %d)", peak(at(2000), rec_n));

    /* 5. sustain: holds while the key is down, falls after */
    reset(); set(SINE, 0, 0, 6000, 0, 0, 127, 0); on(0, 55, 127); run_ms(2000);
    int hold = peak(at(1500), at(2000));
    offk(0); run_ms(3000);
    CHECK(hold > 32000, "sustain 100 %%: full level held after 2 s (peak %d)", hold);
    CHECK(peak(rec_n - at(200), rec_n) == 0 && !active[0], "and it ends after the key is up");
    reset(); set(SINE, 0, 0, 6000, 0, 0, 64, 0); on(0, 55, 127); run_ms(2000);
    /* half the envelope through the two saturations: 0.5 -> 0.48, x 1.76 -> 0.85, -> 0.76 of full scale */
    CHECK(abs(peak(at(1500), at(2000)) - hold * 76 / 100) < hold / 25, "sustain 50 %%: holds lower, 0.76 after the clipper (peak %d of %d)", peak(at(1500), at(2000)), hold);

    /* 6. mono: overlapping keys never add up, the newest plays, the held one comes back */
    reset(); set(SINE, 0, 0, 6000, 0, 0, 127, 0);
    on(0, 55, 127); run_ms(300); on(1, 110, 127); run_ms(300);
    CHECK(peak(0, rec_n) <= 32768 && hold > 32000, "two keys: never louder than one voice (peak %d)", peak(0, rec_n));
    CHECK(fabs(freq(at(400), at(600)) - 110) < 0.5, "the newest key plays (%.1f Hz)", freq(at(400), at(600)));
    offk(1); run_ms(300);
    CHECK(fabs(freq(at(700), at(900)) - 55) < 0.5, "the key still held comes back (%.1f Hz)", freq(at(700), at(900)));
    offk(0); run_ms(1500);
    CHECK(!active[0] && !active[1], "both voices are given back");

    /* 7. legato glide: the pitch slides, the phase does not jump */
    reset(); set(SINE, 0, 0, 6000, 0, 0, 127, 100);
    on(0, 55, 127); run_ms(300); size_t tg = rec_n; on(1, 110, 127); run_ms(1500);
    double f_mid = freq(tg + at(30), tg + at(110)), f_end = freq(tg + at(1200), tg + at(1500));
    int jump = 0; for (size_t i = tg - 200; i < tg + 400; i++) if (abs(rec[i] - rec[i - 1]) > jump) jump = abs(rec[i] - rec[i - 1]);
    CHECK(f_mid > 58 && f_mid < 105, "glide: on the way after 30..110 ms (%.1f Hz)", f_mid);
    CHECK(fabs(f_end - 110) < 0.5, "glide arrives (%.2f Hz)", f_end);
    CHECK(jump < 900, "legato: no step in the wave (largest sample step %d)", jump);
    wav("legato_glide");

    /* 7c. the OP-1 starting two voices for one key (unison): the second voice of a press is not legato over the first,
     * so with the last key up the next press restarts, with its knock, while the old tail still rings */
    reset(); set(SINE, 20000, 3000, 26000, 14000, 12000, 0, 100);
    on(0, 55, 127); on(1, 55, 127); run_ms(150); offk(0); offk(1); run_ms(150);
    { const uint32_t t0 = vis_trig; size_t k0 = rec_n; on(2, 82, 127); on(3, 82, 127); run_ms(200);
      CHECK(vis_trig > t0 && freq(k0, k0 + at(30)) > 120, "unison: the next press restarts over the tail, knock and all (restarts %u, %.0f Hz in its first 30 ms)", vis_trig - t0, freq(k0, k0 + at(30))); }
    offk(2); offk(3); run_ms(2000);

    /* 7b. glide only while a key is down: released, the next note restarts even while the tail rings */
    reset(); set(SINE, 0, 0, 26000, 0, 0, 0, 100); rel_w = T2_HI;
    on(0, 55, 127); run_ms(100); offk(0); run_ms(100); { const uint32_t t0 = vis_trig; on(1, 110, 127); run_ms(200);
      CHECK(vis_trig == t0 + 1 && fabs(freq(rec_n - at(150), rec_n) - 110) < 1, "glide mode, the first key up: the next note restarts at its pitch (%.1f Hz), no glide", freq(rec_n - at(150), rec_n)); }
    offk(1); run_ms(50);

    /* 7d. clicks at a note's start: the energy above ~2 kHz in the 6 ms after it starts, against a pure 55 Hz sine's.
     * Full sub (a clean sine): from silence, and cutting off a ringing note mid-swing */
    if (getenv("TY_CLICK")) {
        sub(32767);
        for (int c = 0; c < 2; c++) {
            reset(); set(SINE, 0, 0, 26000, 14000, 12000, 0, 0);
            size_t k0;
            if (c) { on(0, 55, 127); run_ms(123); offk(0); run_ms(4); }
            k0 = rec_n; on(1, 55, 127); run_ms(40);
            double hf = 0; for (size_t i = k0 + 2; i < k0 + at(6); i++) { const double d2 = rec[i] - 2.0 * rec[i - 1] + rec[i - 2]; hf += d2 * d2; }
            printf("click %s: hf %.0f\n", c ? "over a ringing note" : "from silence", hf / at(6));
        }
        sub(0);
    }

    /* 7f. retrig mode: letting the newest key go restarts the key still held underneath, knock and all; one voice a
     * key, and two (unison) */
    mode(MODE_RETRIG);
    for (int u = 0; u < 2; u++) {
        reset(); set(SINE, 20000, 3000, 26000, 14000, 12000, 127, 0);
        on(0, 55, 127); if (u) on(1, 55, 127); run_ms(150);
        on(2, 82, 127); if (u) on(3, 82, 127); run_ms(150);
        const uint32_t t0 = vis_trig; size_t k0 = rec_n;
        offk(2); if (u) offk(3); run_ms(100);
        CHECK(vis_trig > t0 && freq(k0, k0 + at(30)) > 90, "retrig, %s: letting the newest key go restarts the held one with its knock (restarts %u, %.0f Hz in its first 30 ms)",
              u ? "unison" : "one voice a key", vis_trig - t0, freq(k0, k0 + at(30)));
        offk(0); offk(1); run_ms(2000);
    }
    mode(MODE_GLIDE);

    /* 7g. poly, legato presses: the sound stays on one OP-1 voice the whole time (the OP-1 made a move to the new key's
     * voice a click), following each new key's pitch and restarting with its knock */
    { mode(MODE_RETRIG); reset(); set(SINE, 20000, 3000, 26000, 14000, 12000, 127, 0);
      out_voices = 0; multi_blocks = 0;
      const double hz[5] = { 55, 82, 61, 73, 49 }; double got[5]; unsigned trig = vis_trig;
      for (int i = 0; i < 5; i++) { on(i, hz[i], 127); run_ms(20); if (i) offk(i - 1); run_ms(180); got[i] = freq(rec_n - at(100), rec_n); }
      CHECK(out_voices == 1u && multi_blocks == 0, "poly, five legato presses: the sound stays on its first voice (voices %#x, %d blocks with two)", out_voices, multi_blocks);
      int ok = vis_trig - trig == 5; for (int i = 0; i < 5; i++) ok &= fabs(got[i] - hz[i]) < 1;
      CHECK(ok, "and plays each new key: %.1f %.1f %.1f %.1f %.1f Hz, %u restarts", got[0], got[1], got[2], got[3], got[4], vis_trig - trig);
      offk(4); run_ms(4000);
      CHECK(!active[0] && !active[4], "when it has died away its voice is freed (voice 0 %d, 4 %d)", active[0], active[4]);
      out_voices = 0; on(2, 55, 127); run_ms(100);
      CHECK(out_voices == 1u << 2, "and the next note plays on its own key's voice (%#x)", out_voices);
      /* the OP-1 kills the sound's voice (a steal, a shed): the owner takes the sound over */
      on(3, 82, 127); run_ms(50); offk(2); kill_voice = 2; out_voices = 0; run_ms(200); kill_voice = -1;
      CHECK(out_voices == 1u << 3 && fabs(freq(rec_n - at(100), rec_n) - 82) < 1, "its voice killed, the newest key's voice takes the sound over (%#x, %.1f Hz)", out_voices, freq(rec_n - at(100), rec_n));
      offk(3); run_ms(4000); mode(MODE_GLIDE); }

    /* 8. retrigger over a sounding note: restart without a click */
    reset(); set(SINE, 0, 0, 20000, 0, 0, 0, 0);
    on(0, 55, 127); run_ms(104); offk(0); run_ms(10); size_t tr = rec_n; on(1, 55, 127); run_ms(100);
    jump = 0; for (size_t i = tr - 50; i < tr + 400; i++) if (abs(rec[i] - rec[i - 1]) > jump) jump = abs(rec[i] - rec[i - 1]);
    CHECK(jump < 900, "retrigger: the cut is faded (largest sample step %d)", jump);

    /* 9. clipper: full drive stays inside full scale; hard is squarer than soft */
    double rms[4];
    for (int c = 0; c < 4; c++) {
        reset(); set(SINE, 0, 0, 6000, c > 1 ? 3000 : 32767, c & 1 ? 32767 : 0, 127, 0); on(0, 55, 127); run_ms(600);
        double s = 0; for (size_t i = at(300); i < rec_n; i++) s += (double)rec[i] * rec[i];
        rms[c] = sqrt(s / (rec_n - at(300)));
        CHECK(peak(0, rec_n) <= 32768 && peak(at(300), rec_n) > 32000, "drive %s, %s: peak %d (inside 16 bits)", c > 1 ? "9 %" : "full", c & 1 ? "hard" : "soft", peak(0, rec_n));
        if (c < 2) wav(c ? "drive8_hard" : "drive8_soft");
    }
    CHECK(rms[3] > rms[2] * 1.02, "hard clip is squarer than soft (rms %.0f vs %.0f at 9 %% drive)", rms[3], rms[2]);
    reset(); set(SINE, 0, 0, 6000, 0, 0, 127, 0); on(0, 55, 127); run_ms(600);
    double worst = peak(at(300), rec_n);                         /* drive 1x, soft: x - 4/27 x^3 of a sine */
    CHECK(worst > 32600, "no drive, soft: a sine still reaches full scale (peak %.0f)", worst);

    /* 9b. the fattener: full drive is far brighter than clean, lopsided (even harmonics), and still ends in silence */
    double hf[2], skew = 0;
    for (int c = 0; c < 2; c++) {
        reset(); set(SINE, 0, 0, 6000, c ? 32767 : 0, 0, 127, 0); on(0, 55, 127); run_ms(600);
        double e = 0, de = 0, up = 0;
        for (size_t i = at(300); i < rec_n; i++) { e += (double)rec[i] * rec[i]; de += (double)(rec[i] - rec[i - 1]) * (rec[i] - rec[i - 1]); up += rec[i] > 0; }
        hf[c] = de / e; if (c) skew = up / (rec_n - at(300));
    }
    reset(); set(SINE, 0, 0, 6000, 32767, 32767, 127, 0); on(0, 55, 127); run_ms(50);
    CHECK(peak(0, at(2)) > 24000, "full drive: the hit is there at once (peak %d in the first 2 ms)", peak(0, at(2)));
    CHECK(hf[1] > hf[0] * 20, "full drive: %.0f times the treble of clean", hf[1] / hf[0]);
    CHECK(fabs(skew - 0.5) > 0.03, "full drive is lopsided: %.0f %% of the wave above zero (even harmonics)", skew * 100);
    reset(); set(SINE, 20000, 9000, 9000, 32767, 32767, 0, 0); on(0, 41.2, 127); run_ms(60); offk(0); run_ms(3000);
    CHECK(peak(rec_n - at(500), rec_n) == 0 && !active[0], "full drive one-shot ends in exact silence (tail peak %d)", peak(rec_n - at(500), rec_n));
    wav("fat_oneshot");

    /* 10. triangle, velocity */
    reset(); set(16384, 0, 0, 6000, 0, 0, 127, 0); on(0, 55, 64); run_ms(300);
    int lin = 1; for (size_t i = at(200) + 2; i < at(200) + 300; i++) { int d1 = rec[i] - rec[i - 1], d0 = rec[i - 1] - rec[i - 2]; if ((d1 > 0) == (d0 > 0) && abs(d1 - d0) > 6 * 32767 / OUT_GAIN) lin = 0; }   /* (the rounding, scaled up with rec) */
    CHECK(lin, "shape at half is a triangle (straight flanks)");
    CHECK(abs(peak(at(200), rec_n) - hold * 64 / 127) < hold / 8, "velocity 64 is about half as loud (peak %d)", peak(at(200), rec_n));
    wav("triangle_vel64");

    /* 11. play modes */
    mode(MODE_RETRIG); reset(); set(SINE, 0, 0, 6000, 0, 0, 127, 0);
    on(0, 55, 127); run_ms(300); size_t tm = rec_n; uint32_t trigs = vis_trig; on(1, 110, 127); run_ms(300);
    CHECK(vis_trig == trigs + 1 && abs(rec[tm] - rec[tm - 1]) < 900, "mono retrig: an overlapping note restarts, without a click (step %d)", abs(rec[tm] - rec[tm - 1]));
    mode(MODE_GLIDE); reset(); on(0, 55, 127); run_ms(300); trigs = vis_trig; on(1, 110, 127); run_ms(300);
    CHECK(vis_trig == trigs, "mono glide: the same overlap does not restart");
    mode(MODE_RETRIG); reset(); on(0, 55, 127); run_ms(300); tm = rec_n; on(1, 110, 127); run_ms(300);
    CHECK(peak(tm, rec_n) <= 32768 && fabs(freq(tm + at(100), rec_n) - 110) < 0.5, "mono retrig: still one voice, on the new pitch (%.1f Hz)", freq(tm + at(100), rec_n));
    trigs = vis_trig; offk(1); run_ms(200);
    CHECK(vis_trig == trigs + 1 && fabs(freq(rec_n - at(150), rec_n) - 55) < 0.5, "mono retrig: letting go restarts the key still held (%.1f Hz)", freq(rec_n - at(150), rec_n));
    offk(0); run_ms(1500);
    CHECK(!active[0] && !active[1], "mono retrig: both voices are given back");
    reset(); set(SINE, 0, 0, 6000, 0, 0, 127, 127); on(0, 55, 127); run_ms(300); tm = rec_n; on(1, 110, 127); run_ms(200); size_t tb = rec_n; offk(1); run_ms(200);
    CHECK(fabs(freq(tm + at(10), tm + at(60)) - 110) < 1 && fabs(freq(tb + at(10), tb + at(60)) - 55) < 1, "mono retrig never glides, even with glide at maximum (%.1f, then %.1f Hz)", freq(tm + at(10), tm + at(60)), freq(tb + at(10), tb + at(60)));
    mode(2); reset(); set(SINE, 0, 0, 6000, 0, 0, 127, 0);    /* a sound saved in 0.9.8's POLY (2) */
    on(0, 55, 127); run_ms(300); on(1, 82.5, 127); run_ms(300);
    CHECK(rec_get_mode() == MODE_GLIDE && peak(0, rec_n) <= 32768 && fabs(freq(at(400), rec_n) - 82.5) < 0.5, "a sound saved in the old POLY plays in mono glide: one voice, the newest key (%.1f Hz)", freq(at(400), rec_n));
    offk(0); offk(1); run_ms(1500);
    mode(MODE_GLIDE); host_slot = 0;
    CHECK(ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 3, 1) == 1 && host_eng_rec[0][0] == MODE_RETRIG, "T2 orange push: glide -> retrig");
    ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 3, 0); ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 3, 1);
    CHECK(host_eng_rec[0][0] == MODE_GLIDE, "T2 orange push: retrig -> glide (two modes, no poly)");
    ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 3, 0);
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1) == 1 && host_eng_rec[0][0] == MODE_GLIDE, "a T1 orange push no longer changes the mode");
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 0);
    CHECK(ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 0, 1) == 0 && host_eng_rec[0][0] == MODE_GLIDE, "T2's other pushes are left to the OP-1");
    host_shift_held = 1; ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 3, 1); ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 3, 0); host_shift_held = 0;
    CHECK(host_eng_rec[0][0] == MODE_RETRIG, "a T2 orange push with Shift held still steps the play mode");
    mode(MODE_GLIDE);

    /* 12. attack (T2 blue), tone, sub */
    for (int c = 0; c < 2; c++) {                               /* no drive, then full drive and hard clip */
        mode(MODE_GLIDE); atk_w = t2_of(sqrt(0.1)); reset(); set(SINE, 0, 0, 20000, c ? 32767 : 0, c ? 32767 : 0, 127, 0); on(0, 55, 127); run_ms(300);
        CHECK(peak(0, at(5)) < 2000 && peak(at(40), at(60)) > 11000 && peak(at(40), at(60)) < 21000 && peak(at(150), rec_n) > 32000,
              "attack 100 ms, %s: a slow fade-in (peaks %d at 5 ms, %d at 50 ms, %d after)", c ? "full drive" : "no drive", peak(0, at(5)), peak(at(40), at(60)), peak(at(150), rec_n));
        if (c) wav("attack_100ms");
    }
    atk_w = T2_HI; reset(); set(SINE, 0, 0, 32767, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(1200);
    CHECK(peak(at(400), at(520)) < 20000 && peak(at(1050), rec_n) > 32000, "attack at maximum: a 1 s swell (peaks %d at 0.45 s, %d after 1 s)", peak(at(400), at(520)), peak(at(1050), rec_n));
    atk_w = t2_of(sqrt(0.1)); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 0, 0); on(0, 55, 127); run_ms(104); offk(0); run_ms(10); size_t ta = rec_n; on(1, 55, 127); run_ms(100);
    int stp = 0; for (size_t i = ta - 50; i < ta + 400; i++) if (abs(rec[i] - rec[i - 1]) > stp) stp = abs(rec[i] - rec[i - 1]);
    CHECK(stp < 1500, "attack: a restart over a sounding note does not click (largest sample step %d)", stp);
    atk_w = T2_LO;
    double hfm[3], rmsm[3];
    for (int c = 0; c < 3; c++) {                               /* full drive, hard (75 %: 100 % slams the clipper, which squares a dark wall
                                                                 * bright again): plain, tone closed, sub full */
        sub(c == 2 ? 32767 : 0); tone(c == 1 ? 0 : 32767); reset(); set(SINE, 0, 0, 6000, 32767, 24576, 127, 0); on(0, 55, 127); run_ms(600);
        double e = 0, de = 0;
        for (size_t i = at(300); i < rec_n; i++) { e += (double)rec[i] * rec[i]; de += (double)(rec[i] - rec[i - 1]) * (rec[i] - rec[i - 1]); }
        hfm[c] = de / e; rmsm[c] = sqrt(e / (rec_n - at(300)));
        CHECK(peak(0, rec_n) <= 32768, "%s: inside full scale (peak %d)", c == 0 ? "plain" : c == 1 ? "tone closed" : "sub full", peak(0, rec_n));
        if (c) wav(c == 1 ? "tone_closed" : "sub_full");
    }
    CHECK(hfm[1] < hfm[0] / 20, "tone closed: 1/%.0f of the treble", hfm[0] / hfm[1]);
    CHECK(hfm[2] < hfm[0] * 0.3 && rmsm[2] > 16000, "sub full, full drive: a third of the treble, still loud (treble 1/%.1f, rms %.0f)", hfm[0] / hfm[2], rmsm[2]);
    sub(0); tone(32767);
    reset(); set(SINE, 0, 0, 6000, 14000, 12000, 127, 0); tone(16384); on(0, 55, 127); run_ms(600);
    { double e = 0, de = 0; for (size_t i = at(300); i < rec_n; i++) { e += (double)rec[i] * rec[i]; de += (double)(rec[i] - rec[i - 1]) * (rec[i] - rec[i - 1]); }
      reset(); tone(32767); on(0, 55, 127); run_ms(600);
      double e2 = 0, de2 = 0; for (size_t i = at(300); i < rec_n; i++) { e2 += (double)rec[i] * rec[i]; de2 += (double)(rec[i] - rec[i - 1]) * (rec[i] - rec[i - 1]); }
      reset(); tone(8192); on(0, 55, 127); run_ms(600);
      double e3 = 0, de3 = 0; for (size_t i = at(300); i < rec_n; i++) { e3 += (double)rec[i] * rec[i]; de3 += (double)(rec[i] - rec[i - 1]) * (rec[i] - rec[i - 1]); }
      tone(32767);
      CHECK(de / e < de2 / e2 / 1.3 && de3 / e3 < de / e / 1.5, "tone, default drive: half open has 1/%.2f of the open treble, a quarter 1/%.2f (the whole turn is heard)", (de2 / e2) / (de / e), (de2 / e2) / (de3 / e3)); }

    /* 12a. a new sound (no record): MONO GLIDE, sub 0, glide in the middle (250 ms), no rise */
    { const uint32_t ow = host_eng_rec_owner[0]; host_eng_rec_owner[0] = 0; int16_t r[REC_N]; rec_get(0, r);
      CHECK(r[REC_MODE] == MODE_GLIDE && r[REC_SUB] == 0 && r[REC_GLIDE] == 16384 && r[REC_RISE] == 0, "a new sound: MONO GLIDE, sub 0, glide 250 ms (%d), no rise", r[REC_GLIDE]);
      host_eng_rec_owner[0] = ow; }

    /* 12b. the input hook: a plain turn is the OP-1's, every Shift + turn is TYR8's */
    host_slot = 0; host_shift_held = 0; blue_pg = 0; memset(host_eng_rec[0], 0, sizeof host_eng_rec[0]); mode(MODE_GLIDE);
    memset(host_knobs, 0, sizeof host_knobs); host_knobs[0][5] = 14000; host_knobs[0][6] = 12000; host_knobs[0][7] = 4500;
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, 1024) == 0 && ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 2, 1024) == 0, "a plain turn is the OP-1's");
    CHECK(shift_turn(0, 4 * 1024) == 1 && host_knobs[0][6] == 13024 && host_knobs[0][5] == 14000, "SHAPE page, Shift + blue, four detents: clip %d (+1024), drive untouched", host_knobs[0][6]);
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 0, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 0, 0);
    host_eng_words[0][5] = host_knobs[0][5];
    const int took_d = ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, 2 * 1024);
    host_knobs[0][5] = host_eng_words[0][5];
    CHECK(blue_pg == 1 && took_d && host_knobs[0][5] == 16048 && host_knobs[0][0] == 0, "a blue push: the DRIVE page, a plain turn is drive (two detents: %d), shape untouched", host_knobs[0][5]);
    shift_turn(0, 3 * 1024);
    CHECK(host_eng_rec[0][REC_STUNE] == 25 + 3 && host_knobs[0][6] == 13024, "and Shift + blue the sample's tune (+%d semitones), clip untouched", host_eng_rec[0][REC_STUNE] - 25);
    shift_turn(0, -3 * 1024);
    host_shift_held = 1; ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 0, 1); host_shift_held = 0;
    CHECK(blue_pg == 0 && ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, 1024) == 0, "a push with Shift switches too: back on SHAPE, the plain turn the OP-1's");
    host_eng_rec[0][REC_SHP] = 0;
    shift_turn(1, 300 * 1024);
    CHECK(host_knobs[0][7] == 32767, "Shift + green: knock length, stops at the top (%d)", host_knobs[0][7]);
    shift_turn(2, 10 * 1024); shift_turn(3, 3 * 1024); shift_turn(3, -1024);
    CHECK(host_eng_rec[0][REC_SUB] == 2560 && host_eng_rec[0][REC_GLIDE] == 512 && host_eng_rec[0][REC_MODE] == MODE_GLIDE, "Shift + white: sub %d, Shift + orange: glide %d, the mode kept", host_eng_rec[0][REC_SUB], host_eng_rec[0][REC_GLIDE]);
    shift_turn(2, -100 * 1024);
    CHECK(host_eng_rec[0][REC_SUB] == 0, "sub stops at 0");
    CHECK(ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_TURN, 0, 1024) == 0, "T2's turns are the OP-1's (it turns the envelope words)");
    sag(0); host_eng_rec[0][REC_SUB] = 0; host_eng_rec[0][REC_GLIDE] = 0;
    CHECK(orange_turn(-2 * 1024) == 1 && host_knobs[0][3] == 0 && host_eng_rec[0][REC_RISE] == 2048, "orange below 0: sag rises (record %d), the fall word stays 0", host_eng_rec[0][REC_RISE]);
    orange_turn(5 * 1024);
    CHECK(host_knobs[0][3] == 3072 && host_eng_rec[0][REC_RISE] == 0, "and back up through 0: the fall %d, the rise cleared", host_knobs[0][3]);
    orange_turn(100 * 1024); const int top3 = host_knobs[0][3]; orange_turn(-200 * 1024);
    CHECK(top3 == 32767 && host_knobs[0][3] == 0 && host_eng_rec[0][REC_RISE] == 32767 && host_eng_rec[0][REC_MODE] == MODE_GLIDE, "sag stops at 100 %% and -100 %%, the mode kept");
    sag(0); rise(0);
    memset(host_eng_rec[0], 0, sizeof host_eng_rec[0]); mode(MODE_GLIDE);

    /* 12c. release (T2 orange) and the defaults of a slot with no T2 words */
    rel_w = T2_LO; reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(500); offk(0); size_t tk = rec_n; run_ms(300);
    CHECK(peak(tk + at(40), rec_n) < 300, "release at 0: a held note is gone 40 ms after the key (peak %d)", peak(tk + at(40), rec_n));
    rel_w = t2_of(0.6); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(500); offk(0); tk = rec_n; run_ms(1500);
    CHECK(peak(tk + at(100), tk + at(150)) > 8000 && peak(rec_n - at(100), rec_n) < 200, "release at 60 %%: a tail of a few hundred ms (peaks %d at 0.1 s, %d at 1.5 s)", peak(tk + at(100), tk + at(150)), peak(rec_n - at(100), rec_n));
    rel_w = T2_LO; reset(); set(SINE, 0, 0, 20000, 14000, 12000, 0, 0); on(0, 55, 127); run_ms(40); offk(0); tk = rec_n; run_ms(300);
    CHECK(peak(tk + at(40), rec_n) < 300, "release at 0 cuts a one-shot at the key too (peak %d)", peak(tk + at(40), rec_n));
    rel_w = T2_HI;
    reset(); set(SINE, 14000, 9000, 16000, 6000, 8000, 0, 0); memset(host_t2[0], 0, sizeof host_t2[0]); on(0, 55, 127); run_ms(40); offk(0); run_ms(3000);
    CHECK(peak(at(60), at(300)) > 4000 && peak(at(2500), rec_n) == 0 && !active[0], "no T2 words: the 808 one-shot (peak %d after the key)", peak(at(60), at(300)));

    /* 13. the shape knob is heard at the default drive: sine, triangle and saw differ in treble */
    double sh[3];
    for (int c = 0; c < 3; c++) {
        reset(); set(8192 + c * 8192, 0, 0, 6000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(600);
        double e = 0, de = 0;
        for (size_t i = at(300); i < rec_n; i++) { e += (double)rec[i] * rec[i]; de += (double)(rec[i] - rec[i - 1]) * (rec[i] - rec[i - 1]); }
        sh[c] = de / e;
        if (c == 2) { CHECK(peak(0, rec_n) <= 32768, "saw: inside full scale"); wav("shape_saw"); } else if (c == 0) wav("shape_sine");
    }
    CHECK(sh[2] > sh[0] * 4, "shape at the default drive: saw has %.1f times the treble of sine (triangle %.1f)", sh[2] / sh[0], sh[1] / sh[0]);

    /* 14. the clipper is last: with the tone closed the output is still at full scale, and darker */
    tone(0); reset(); set(SINE, 0, 0, 6000, 32767, 32767, 127, 0); on(0, 55, 127); run_ms(500);
    CHECK(peak(at(200), rec_n) > 32000, "tone closed at full drive: a dark wall, still full scale (peak %d)", peak(at(200), rec_n));
    tone(32767);

    /* 15. feedback at both ends of the shape knob; none in between */
    double per[5];                                               /* how badly one cycle fails to repeat the last, 0 .. */
    for (int c = 0; c < 5; c++) {
        const int shp[5] = { 0, 8192, 16384, 24576, 32767 };
        reset(); set(shp[c], 0, 0, 6000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(700);
        const size_t T = (size_t)(88200 / 55.0 + 0.5); double num = 0, den = 0;
        for (size_t i = at(300); i + T < rec_n; i++) { num += fabs((double)rec[i] - rec[i + T]); den += fabs((double)rec[i]); }
        per[c] = num / den;
        CHECK(peak(0, rec_n) <= 32768, "shape %d %%: inside 16 bits", shp[c] * 100 / 32767);
        if (c == 0) wav("feedback_sine"); else if (c == 4) wav("feedback_saw");
    }
    CHECK(per[1] < 0.02 && per[2] < 0.02 && per[3] < 0.05, "sine, triangle and saw repeat every cycle (%.3f, %.3f, %.3f)", per[1], per[2], per[3]);
    CHECK(per[0] > 0.3 && per[4] > 0.3, "full feedback at either end does not: no steady pitch (%.2f below sine, %.2f above saw)", per[0], per[4]);
    reset(); set(6500, 0, 0, 6000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(700);
    CHECK(fabs(freq(at(300), rec_n) - 55) < 0.5, "a little feedback keeps the pitch (%.2f Hz)", freq(at(300), rec_n));
    reset(); set(0, 12000, 4500, 16000, 14000, 12000, 0, 0); on(0, 55, 127); run_ms(40); offk(0); run_ms(3000);
    size_t tf = rec_n; on(3, 55, 127); run_ms(40);
    CHECK(!memcmp(rec, rec + tf, at(40) * 2), "with full feedback two hits from silence are still sample-identical");

    /* 16. knock: longer and deeper */
    reset(); set(SINE, 32767, 32767, 32767, 0, 0, 127, 0); on(0, 55, 127); run_ms(3000);
    CHECK(freq(0, at(6)) > 500, "knock at maximum starts %.0f Hz up (16x the note is 935)", freq(0, at(6)));
    CHECK(freq(at(450), at(550)) > 65 && fabs(freq(at(2700), rec_n) - 55) < 0.5, "knock length 1.5 s: still %.1f Hz at 0.5 s, home at the end (%.2f)", freq(at(450), at(550)), freq(at(2700), rec_n));
    wav("knock_long");

    /* 17. sag: the pitch falls while the note lasts */
    sag(32767); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(60); offk(0); run_ms(1200);
    double fs0 = freq(0, at(60)), fs1 = freq(at(500), at(700));
    CHECK(fs0 < 55 && fs0 > 50 && fs1 < fs0 * 0.75 && fs1 > 12, "sag at maximum: %.1f Hz at the start, %.1f Hz half a second on", fs0, fs1);
    wav("sag");
    sag(16384); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(60); offk(0); run_ms(1200);
    CHECK(freq(at(500), at(700)) > fs1 * 1.1 && freq(at(500), at(700)) < 54, "sag at half falls less (%.1f Hz)", freq(at(500), at(700)));
    sag(0); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(60); offk(0); run_ms(1200);
    CHECK(fabs(freq(at(500), at(700)) - 55) < 0.3, "sag at 0: none (%.2f Hz)", freq(at(500), at(700)));
    /* the law: 25 %% about 1.9 semitones over the decay's length, 50 %% 4.2, 75 %% 7.2, 100 %% 10.7. Decay 0.6 s here, word 16000:
     * the note's length is the decay's (a fifth of it is the time constant), so measure 0.6 s on */
    { const double want[4] = { 1.85, 4.25, 7.2, 10.7 }; double got[4];
      for (int c = 0; c < 4; c++) {
          sag((c + 1) * 8192 - (c == 3)); reset(); set(SINE, 0, 0, 16000, 0, 0, 127, 0); on(0, 55, 127); run_ms(700);
          got[c] = 12 * log2(55 / freq(at(520), at(680)));
      }
      CHECK(fabs(got[0] - want[0]) < 0.8 && fabs(got[1] - want[1]) < 1 && fabs(got[2] - want[2]) < 1.3 && fabs(got[3] - want[3]) < 1.6,
            "sag 25, 50, 75, 100 %%: %.1f, %.1f, %.1f, %.1f semitones down after the decay's length", got[0], got[1], got[2], got[3]); }
    sag(0); rise(32767); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(60); offk(0); run_ms(1200);
    const double fr0 = freq(0, at(60)), fr1 = freq(at(500), at(700)), fr2 = freq(at(1100), at(1260));
    CHECK(fr0 > 55 && fr0 < 60 && fr1 > fr0 * 1.2 && fr2 > fr1 * 1.1 && fr2 < 100, "sag at -100 %%: it rises, %.1f Hz at the start, %.1f half a second on, %.1f at the end (under its octave cap)", fr0, fr1, fr2);
    wav("sag_rise");
    rise(8192); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(60); offk(0); run_ms(1200);
    CHECK(freq(at(500), at(700)) > 56 && freq(at(500), at(700)) < fr1 * 0.9, "sag at -25 %%: rises less (%.1f Hz)", freq(at(500), at(700)));
    rise(0); sag(0);

    /* the sound this was for: full feedback saw, a long deep knock, sag, drive up, the tone half closed */
    sag(20479); tone(18430); reset(); set(31500, 22000, 20000, 26000, 28000, 26000, 0, 0);
    on(0, 43.65, 127); run_ms(150); offk(0); run_ms(1400); on(1, 38.89, 127); run_ms(150); offk(1); run_ms(5000);
    CHECK(peak(0, rec_n) <= 32768 && !active[0] && !active[1] && peak(rec_n - at(100), rec_n) == 0, "the doom patch stays in range and ends");
    wav("doom");
    { /* headroom for the bus: through two 35 Hz one-pole high-passes, as the OP-1's synth bus (modelled: its order is
       * not known), an ordinary one-shot (the default drive and clip) goes about 1.5 dB over full scale at its onset, the
       * doom patch (full feedback saw, drive and clip up) about 2. The price of -4 dB instead of 0.9.17's -6.5, which the
       * owner found too quiet; at full scale (up to 0.9.16) it was 5.5 and 6 over, and the OP-1 clipped every note */
      const double doom = bus_peak();
      raw_peak = 0; reset(); set(SINE, 0, 0, 16000, 14000, 12000, 0, 0); on(0, 55, 127); run_ms(300); offk(0); run_ms(800);
      const double plain = bus_peak();
      CHECK(raw_peak <= OUT_GAIN + 1 && raw_peak > OUT_GAIN - 200 && plain < 32767 * 1.22 && doom < 32767 * 1.32,
            "headroom: a note peaks at %d (-4 dB); after the bus's 35 Hz high-pass an ordinary one-shot at %+.1f dB, the doom patch at %+.1f dB",
            raw_peak, 20 * log10(plain / 32767), 20 * log10(doom / 32767)); }
    sag(0); tone(32767);

    /* 18. the tail: a clean tone while it is loud enough to have bits for it, then a fast fade to exact silence instead
     * of half a second as a staircase of a few output levels (what a crusher after the synth turns into grit).
     * 882 Hz / 16 = 55.125 Hz is exactly 1600 samples a cycle */
    for (int c = 0; c < 3; c++) {
        const int drv[3] = { 0, 14000, 32767 };
        reset(); set(SINE, 0, 0, 16000, drv[c], 12000, 0, 0); on(0, 55.125, 127); run_ms(40); offk(0); run_ms(3000);
        size_t low = 0, end = rec_n;                             /* where the peak first stays under 96, and the last sound */
        while (end && !rec[end - 1]) end--;
        for (size_t i = at(100); i + at(30) < rec_n; i += at(10)) if (peak(i, i + at(30)) < 96) { low = i; break; }
        const double tq = c == 2 ? 750 : 500, q = tail_snr(at(tq), at(tq + 100), 55.125);   /* full drive: past its flat-out part */
        CHECK(q > 40, "drive %d %%: the tail is a clean tone while it is loud, %.0f dB over its noise at %.2f s (peak %d)", drv[c] * 100 / 32767, q, tq / 1000, peak(at(tq), at(tq + 100)));
        CHECK(low && end > low && (end - low) / 88.2 < 100, "drive %d %%: below -51 dBFS it fades to silence in %.0f ms (ends at %.0f ms)", drv[c] * 100 / 32767, (end - low) / 88.2, end / 88.2);
        int jump = 0; for (size_t i = low; i < end + 10 && i < rec_n; i++) if (abs(rec[i] - rec[i - 1]) > jump) jump = abs(rec[i] - rec[i - 1]);
        CHECK(jump < 60, "drive %d %%: and the fade is smooth, no click into silence (largest step %d)", drv[c] * 100 / 32767, jump);
        if (c == 2) wav("tail_full_drive");
    }
    reset(); set(SINE, 0, 0, 16000, 14000, 12000, 1, 0); on(0, 55, 127); run_ms(3000);
    CHECK(peak(rec_n - at(100), rec_n) > 0 && active[0], "a held note at a tiny sustain (1 %%) is not faded away (peak %d)", peak(rec_n - at(100), rec_n));
    offk(0); run_ms(1000);

    /* 19. clip is heard: at the default drive, hard is far brighter than soft; with no drive it squares the sine */
    double ch[2], cr[2];
    for (int c = 0; c < 2; c++) {
        reset(); set(SINE, 0, 0, 6000, 14000, c ? 32767 : 0, 127, 0); on(0, 55, 127); run_ms(600);
        double e = 0, de = 0;
        for (size_t i = at(300); i < rec_n; i++) { e += (double)rec[i] * rec[i]; de += (double)(rec[i] - rec[i - 1]) * (rec[i] - rec[i - 1]); }
        ch[c] = de / e;
        wav(c ? "clip_hard" : "clip_soft");
        reset(); set(SINE, 0, 0, 6000, 0, c ? 32767 : 0, 127, 0); on(0, 55, 127); run_ms(600);
        e = 0; for (size_t i = at(300); i < rec_n; i++) e += (double)rec[i] * rec[i];
        cr[c] = sqrt(e / (rec_n - at(300))) / 32768;
        CHECK(peak(0, rec_n) <= 32768, "clip %s: inside 16 bits", c ? "hard" : "soft");
    }
    CHECK(ch[1] > ch[0] * 2.5, "clip at the default drive: hard has %.1f times the treble of soft", ch[1] / ch[0]);
    CHECK(cr[0] < 0.85 && cr[1] > 0.93, "clip with no drive: a rounded sine at soft (rms %.2f), nearly square at hard (rms %.2f)", cr[0], cr[1]);
    /* the last quarter slams it: at 100 %% the tail stays square far longer than at 75 %% */
    { double tl[2];
      for (int c = 0; c < 2; c++) {
          reset(); set(SINE, 0, 0, 16000, 0, c ? 32767 : 24576, 127, 0); on(0, 55, 127); run_ms(80); offk(0); run_ms(1500);
          double e = 0; for (size_t i = at(500); i < at(700); i++) e += (double)rec[i] * rec[i];
          tl[c] = sqrt(e / (at(700) - at(500))) / 32768;
          CHECK(peak(0, rec_n) <= 32768, "clip %s: inside 16 bits", c ? "100 %" : "75 %");
          if (c) wav("clip_slam");
      }
      CHECK(tl[1] > tl[0] * 2.5, "clip 100 %% slams the clipper: the tail 0.5 s on is %.1f times louder than at 75 %% (rms %.2f)", tl[1] / tl[0], tl[1]); }

    /* 20. the sub is heard: blended in after the clipper, its clean sine takes over the low end as it goes up. Measured
     * as the share of the output that is a 55 Hz sine, at the default drive and at full drive, sine and saw. (Before
     * 0.9.7 it was mixed in before the clipper, which squared it again: turning it up thinned the sound) */
    for (int c = 0; c < 4; c++) {
        const int shp = c & 1 ? 24576 : SINE, drv = c & 2 ? 32767 : 14000;
        double part[2];
        for (int v = 0; v < 2; v++) {
            sub(v ? 32767 : 0); reset(); set(shp, 0, 4500, 16000, drv, 12000, 127, 0); on(0, 55, 127); run_ms(700);
            part[v] = sine_part(at(300), at(700), 55) * sine_part(at(300), at(700), 55);
        }
        CHECK(part[1] > 0.86 && part[1] > part[0] + 0.07, "sub, %s, drive %d %%: the 55 Hz sine is %.0f %% of the output at sub 0, %.0f %% at full",
              c & 1 ? "saw" : "sine", drv * 100 / 32767, part[0] * 100, part[1] * 100);
        if (c == 3) wav("sub_full_drive_saw");
    }
    for (int c = 0; c < 4; c++) {                               /* and with no drive, whatever the shape, it is the sine */
        const int shp[4] = { 8192, 24576, 0, 32767 };
        sub(32767); tone(0); reset(); set(shp[c], 0, 0, 6000, 0, 0, 127, 0); on(0, 55, 127); run_ms(700);
        CHECK(sine_part(at(300), at(700), 55) > (c < 2 ? 0.985 : 0.72), "sub full, no drive, shape %d %%: %.1f %% of the output's amplitude is a 55 Hz sine", shp[c] * 200 / 32767 - 50, sine_part(at(300), at(700), 55) * 100);
    }
    sub(0); tone(32767);
    /* the octave-down sub: at 110 Hz it is 55 Hz, clean, the whole output at full; at 55 Hz (27.5 down, under the bus's
     * 35 Hz high-pass) it stays at the note; at 85 Hz (42.5 down) half and half; at half the sub, half of it */
    { sub(32767); reset(); set(SINE, 0, 4500, 16000, 14000, 12000, 127, 0); on(0, 110, 127); run_ms(700);
      const double o_full = sine_part(at(300), at(700), 55), n_full = sine_part(at(300), at(700), 110);
      wav("sub_octave_110");
      CHECK(o_full > 0.98 && n_full < 0.05, "sub full at 110 Hz: the output is a 55 Hz sine (%.1f %%), none of the note left (%.1f %%)", o_full * 100, n_full * 100);
      sub(16384); reset(); set(SINE, 0, 4500, 16000, 14000, 12000, 127, 0); on(0, 110, 127); run_ms(700);
      const double o_half = sine_part(at(300), at(700), 55);
      CHECK(o_half > 0.4 && o_half < 0.6, "sub half at 110 Hz: half of it the octave down (%.0f %%)", o_half * 100);
      sub(32767); reset(); set(SINE, 0, 4500, 16000, 14000, 12000, 127, 0); on(0, 85, 127); run_ms(700);
      const double o_mid = sine_part(at(300), at(700), 42.5), n_mid = sine_part(at(300), at(700), 85);
      CHECK(fabs(o_mid - n_mid) < 0.15 && o_mid + n_mid > 1.3, "sub full at 85 Hz, between 40 and 45 Hz down: crossfaded evenly, %.0f %% at 42.5 Hz, %.0f %% at 85 (two equal sines: 71 %% each)", o_mid * 100, n_mid * 100);
      sub(32767); reset(); set(SINE, 0, 4500, 16000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(700);
      CHECK(sine_part(at(300), at(700), 27.5) < 0.02, "sub full at 55 Hz: nothing at 27.5 Hz, under the bus filter (%.1f %%)", sine_part(at(300), at(700), 27.5) * 100);
      /* a glide from 55 up to 110 crosses the floor without a click */
      reset(); set(SINE, 0, 4500, 16000, 14000, 12000, 127, 100); on(0, 55, 127); run_ms(200); size_t g0 = rec_n; on(1, 110, 127); run_ms(500);
      int jump = 0; for (size_t i = g0; i < rec_n; i++) if (abs(rec[i] - rec[i - 1]) > jump) jump = abs(rec[i] - rec[i - 1]);
      CHECK(jump < 600, "a glide up through the floor crossfades the sub smoothly (largest sample step %d)", jump);
      wav("sub_glide_floor");
      offk(0); offk(1); run_ms(500); }
    sub(0);

    /* 21. the sample layer: another slot's recording mixed in at the oscillator's stage on every restarted note */
    {
        smp_rec(0, 0, 0); make_click(2, 3000, 1500);             /* a 34 ms click in slot 3, tyr8 on slot 1 */
        reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(400); offk(0); run_ms(100);
        const uint32_t h_off = audio_hash; size_t n_off = rec_n; int16_t *ref = malloc(n_off * 2); memcpy(ref, rec, n_off * 2);
        no_samples(); audio_hash = h_off;
        reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(400); offk(0); run_ms(100);
        CHECK(!memcmp(ref, rec, n_off * 2), "SAMPLE at 0: the same samples as with no recording anywhere");
        make_click(2, 3000, 1500); smp_rec(SMP_SOLO, 0, 0);
        reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(400); offk(0); run_ms(100);
        const double h_on = treble(0, at(30)), h_ref = 0;
        double e0 = 0; for (size_t i = 1; i < at(30); i++) e0 += (double)(ref[i] - ref[i - 1]) * (ref[i] - ref[i - 1]);
        (void)h_ref;
        int same = 1; for (size_t i = at(60); i < n_off; i++) if (rec[i] != ref[i]) { same = 0; break; }
        CHECK(h_on > 50 * e0 && same, "SAMPLE 75 %% (the top of the layering), no drive: a click over the first 30 ms (treble %.0fx the plain note's), the note itself after it untouched", h_on / (e0 > 0 ? e0 : 1));
        CHECK(raw_peak <= 32767, "and inside full scale");
        wav("sample_click");
        { /* the top of SAMPLE: from 75 % the body (and its sub) fades out between the drive and the clipper; at 100 % the
           * sample alone, still through its drive and the clipper */
          make_tone(2, 3000, 13230, 3000);
          for (int i = 0; i < 13230; i++) smpb[2][8 + 3000 + i] = (int16_t)(smpb[2][8 + 3000 + i] * 3 / 5);   /* 12000 peak */
          sb_make(2);
          #define RAWPK(a, b) (peak(a, b) * (double)OUT_GAIN / 32767)
          smp_rec(32767, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(250);
          const double pk100 = RAWPK(at(20), at(120)), body100 = sine_part(at(20), at(120), KEY_C4);
          CHECK(pk100 > 8000 && body100 < 0.02, "SAMPLE 100 %%: the sample alone (peak %.0f), no body under it (%.1f %% at the note's pitch)", pk100, body100 * 100);
          wav("sample_solo");
          { /* SUB at full is only the clean sine; it steps aside for the sample, and fades with the body at the top of
             * SAMPLE: SUB 100 % and SAMPLE 100 % play the sample, SUB 100 % and SAMPLE 50 % the sample over the sine */
            sub(32767);
            smp_rec(32767, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(150);
            const double both = sine_part(at(20), at(120), 3000), pk_both = RAWPK(at(20), at(120));
            #define AMP3K(a, b) (sine_part(a, b, 3000) * sqrt(treble(a, b) * 0 + rms_of(a, b)))
            smp_rec(16384, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(400);
            const double half = AMP3K(at(20), at(120)), after = sine_part(at(330), at(390), KEY_C4);
            sub(0);
            smp_rec(16384, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(150);
            const double half0 = AMP3K(at(20), at(120));
            #undef AMP3K
            CHECK(both > 0.8 && pk_both > 8000 && half > 0.8 * half0 && after > 0.9,
                  "SUB 100 %%: with SAMPLE 100 %% the sample plays (%.0f %% of it, peak %.0f); at SAMPLE 50 %% it plays over the sine at %.0f %% of its level with SUB 0, and the sine is back after it (%.0f %%)",
                  both * 100, pk_both, half / half0 * 100, after * 100); }
          make_tone(2, 3000, 13230, 200);
          for (int i = 0; i < 13230; i++) smpb[2][8 + 3000 + i] = (int16_t)(smpb[2][8 + 3000 + i] * 3 / 5);
          sb_make(2); src_force = 1;
          smp_rec(32767, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(150);
          const double tr0 = treble(at(20), at(120)) / (double)(peak(at(20), at(120)) + 1) / (peak(at(20), at(120)) + 1);
          smp_rec(32767, 0, 0); reset(); set(SINE, 0, 4500, 16000, 32767, 0, 127, 0); on(0, KEY_C4, 127); run_ms(150);
          const double tr1 = treble(at(20), at(120)) / (double)(peak(at(20), at(120)) + 1) / (peak(at(20), at(120)) + 1);
          CHECK(tr1 > 3 * tr0, "and DRIVE still shapes it: a 200 Hz sample alone, full drive, has %.1fx the treble of no drive", tr1 / tr0);
          wav("sample_solo_driven");
          make_tone(2, 3000, 13230, 3000);
          for (int i = 0; i < 13230; i++) smpb[2][8 + 3000 + i] = (int16_t)(smpb[2][8 + 3000 + i] * 3 / 5);
          sb_make(2); src_force = 1;
          { /* the sampler's gain as the sampler plays it: a quarter of its knob is 0 dB, the top x16 (+24 dB), clipped at
             * full scale before anything of TYR8's. At SAMPLE 100 %, a tone at 0 dB comes out rounded, at the top squared */
            host_sampler_type[2] = 6; host_sampler_rate[2] = RATE_C4; host_knobs[2][0] = 0; host_knobs[2][3] = 32767; host_knobs[2][4] = 0;
            #define FLAT(f) do { const int pk = peak(at(20), at(120)); int nf = 0, nn = 0; \
                for (size_t i = at(20); i < at(120); i++, nn++) { if (abs(rec[i]) >= pk * 0.97) nf++; } f = nf * 100 / nn; } while (0)
            int f0, f24;
            host_knobs[2][7] = 8192; src_force = 1;
            smp_rec(32767, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(150);
            FLAT(f0);
            host_knobs[2][7] = 32767; src_force = 1;
            smp_rec(32767, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(150);
            FLAT(f24);
            #undef FLAT
            CHECK(f0 < 25 && f24 > 60, "the sampler's gain: at 0 dB the tone stays rounded (%d %% of it at its peak), at +24 dB it is squared off (%d %%)", f0, f24);
            wav("sample_solo_slammed");
            host_sampler_type[2] = 0; host_sampler_rate[2] = 0; host_knobs[2][3] = 0; host_knobs[2][7] = 0; src_force = 1; }
          smp_rec(SMP_SOLO, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(250);
          const double body75 = sine_part(at(20), at(120), KEY_C4);
          smp_rec((SMP_SOLO + 32767) / 2, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(250);
          const double body87 = sine_part(at(20), at(120), KEY_C4), pk87 = RAWPK(at(20), at(120));
          CHECK(body75 > 0.3 && body87 > 0.05 && body87 < body75 && pk87 > 0.4 * pk100, "between 75 and 100 %% the body fades out (its share %.0f %% at 75, %.0f %% at 87.5) as the sample comes in", body75 * 100, body87 * 100);
          /* no recording: SAMPLE 100 %% leaves the note as it is, never silence */
          no_samples(); audio_hash = 0;
          smp_rec(32767, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(200);
          const size_t nn = rec_n; int16_t *r1 = malloc(nn * 2); memcpy(r1, rec, nn * 2);
          smp_rec(0, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(200);
          CHECK(!memcmp(r1, rec, nn * 2), "with no recording found, SAMPLE 100 %% plays the note as SAMPLE 0 does");
          free(r1);
          /* at SAMPLE 100 %, a new note cutting the sample: no click */
          make_tone(2, 3000, 13230, 3000);
          for (int i = 0; i < 13230; i++) smpb[2][8 + 3000 + i] = (int16_t)(smpb[2][8 + 3000 + i] * 3 / 5);
          sb_make(2); src_force = 1;
          smp_rec(32767, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(50.3);
          const size_t cut = rec_n; on(0, KEY_C4, 127); run_ms(30);
          int jump = 0; for (size_t i = cut - 4; i < cut + 400 && i < rec_n; i++) { const int d = abs(rec[i] - rec[i - 1]); if (d > jump) jump = d; }
          int own = 0; for (size_t i = at(20); i < at(45); i++) { const int d = abs(rec[i] - rec[i - 1]); if (d > own) own = d; }
          CHECK(jump < own * 1.6, "at SAMPLE 100 %%, a new note cuts the sample without a click (largest step %d, the tone's own %d)", jump, own);
          offk(0); run_ms(600);
          CHECK(!active[0] || ENGINE_tyr8.ext.finished(OP1_VOICE(0)), "and the voice is given back once it has played");
          #undef RAWPK
          make_click(2, 3000, 1500); smp_rec(SMP_SOLO, 0, 0); }
        /* through the drive: driven, the click comes out denser than clean */
        reset(); set(SINE, 0, 4500, 16000, 32767, 0, 127, 0); on(0, KEY_C4, 127); run_ms(400); offk(0); run_ms(100);
        wav("sample_click_driven");
        CHECK(peak(0, at(30)) > 20000, "with full drive it goes through the drive and clipper (peak %d over the click)", peak(0, at(30)));
        /* it plays whole however short the note: a 300 ms recording over a 0.1 s decay */
        make_tone(2, 3000, 13230, 3000); src_force = 1;
        reset(); set(SINE, 0, 4500, 0, 0, 0, 0, 0); on(0, KEY_C4, 127);
        run_ms(5); offk(0); run_ms(600);
        const double s250 = sine_part(at(200), at(280), 3000);
        CHECK(s250 > 0.9 && peak(at(200), at(280)) > 2000, "a 300 ms recording plays whole over a 0.1 s decay (at 200 .. 280 ms it is %.0f %% the recording's 3 kHz)", s250 * 100);
        CHECK(!active[0] && peak(at(320), rec_n) == 0, "and then the voice is given back, silent");
        /* the sampler's own START and END: trimmed to 50 ms on the sampler's page, the layer is 50 ms; START 100 ms
         * in, it starts there. A slot that is not a sampler (its words are another engine's) plays the whole buffer */
        {
            const double f = 32768.0 / (OP1_SAMPLE_MAX - OP1_SAMPLE_GUARD);
            host_sampler_type[2] = 6; host_knobs[2][7] = 8192; host_knobs[2][0] = 0; host_knobs[2][3] = (int16_t)lrint((3000 + 2205) * f + 1);
            reset(); set(SINE, 0, 4500, 0, 0, 0, 0, 0); on(0, KEY_C4, 127); run_ms(5); offk(0); run_ms(400);
            const double cut = sine_part(at(120), at(200), 3000), early = sine_part(at(10), at(40), 3000);
            const int ends = active[0] == 0 && peak(at(200), rec_n) == 0;
            CHECK(early > 0.85 && peak(at(120), at(200)) < 300 && ends, "the sampler trimmed to END 50 ms: the layer stops there (3 kHz %.0f %% at 10 .. 40 ms; peak after %d)", early * 100, peak(at(120), at(200)));
            host_knobs[2][0] = (int16_t)lrint((3000 + 4410) * f); host_knobs[2][3] = 32767;
            src_force = 1; reset(); set(SINE, 0, 4500, 0, 0, 0, 0, 0); on(0, KEY_C4, 127); run_ms(5); offk(0); run_ms(400);
            const double still = sine_part(at(150), at(190), 3000);
            const int gone = peak(at(215), rec_n) < 300;
            CHECK(peak(0, at(3)) > 3000 && still > 0.9 && gone, "START 100 ms in: it starts there at once (peak %d in 3 ms), and the 200 ms left play (%.0f %% at 150 .. 190 ms), then nothing (%d)", peak(0, at(3)), still * 100, peak(at(215), rec_n));
            host_sampler_type[2] = 0; host_knobs[2][0] = 0; host_knobs[2][3] = 3000;
            src_force = 1; reset(); set(SINE, 0, 4500, 0, 0, 0, 0, 0); on(0, KEY_C4, 127); run_ms(5); offk(0); run_ms(400);
            CHECK(sine_part(at(200), at(280), 3000) > 0.9, "a slot that is not a sampler: its words are not markers, the whole recording plays");
            host_knobs[2][3] = 0; src_force = 1; (void)cut;
        }
        /* the pitch: the sampler's for the key played (key follow), then SMP TUNE, -24 .. +24 */
        #define PITCHED(tune, key, hz) (smp_rec(SMP_LVL, tune, 0), reset(), set(SINE, 0, 4500, 16000, 0, 0, 127, 0), on(0, key, 127), run_ms(100), sine_part(at(20), at(100), hz))
        const int SMP_LVL = 32767;
        const double at_c4 = PITCHED(0, KEY_C4, 3000), at_c5 = PITCHED(0, KEY_C4 * 2, 6000), at_g3 = PITCHED(0, KEY_C4 * 0.75, 2250);
        CHECK(at_c4 > 0.5 && at_c5 > 0.5 && at_g3 > 0.5, "key follow: a recording based at middle C plays as recorded on the C4 key (3 kHz %.0f %%), an octave up on C5 (%.0f %%), a fourth down on G3 (%.0f %%)", at_c4 * 100, at_c5 * 100, at_g3 * 100);
        const double up = PITCHED(24, KEY_C4, 12000), dn = PITCHED(-24, KEY_C4, 750), u7 = PITCHED(7, KEY_C4, 3000 * pow(2, 7 / 12.0));
        CHECK(up > 0.45 && dn > 0.5 && u7 > 0.5, "SMP TUNE +24: two octaves up, 12 kHz (%.0f %%); -24: 750 Hz (%.0f %%); +7: a fifth (%.0f %%)", up * 100, dn * 100, u7 * 100);
        /* the sampler's own settings are where it starts: its tune (based at A4 here), its gain, its direction */
        host_sampler_type[2] = 6; host_sampler_rate[2] = 26273978; host_knobs[2][0] = 0; host_knobs[2][3] = 32767; host_knobs[2][4] = 12000; host_knobs[2][7] = 8192;
        const double a4 = PITCHED(0, KEY_C4, 3000 * 261.6256 / 440), a4t = PITCHED(12, KEY_C4, 6000 * 261.6256 / 440);
        CHECK(a4 > 0.5 && a4t > 0.5, "the sampler's tune: based at A4, the C4 key plays it 9 semitones down (%.0f %%), SMP TUNE +12 from there (%.0f %%)", a4 * 100, a4t * 100);
        /* the pitch still sliding in from a higher note (portamento, the OP-1's smoothing): the sample follows it to the
         * key's own, as the sampler does, and does not keep the first block's */
        { porta_hz[0] = KEY_C4 * 4 * (1 << OCT_DOWN); porta_n[0] = 6; porta_i[0] = 0;
          const double sl = PITCHED(0, KEY_C4, 3000 * 261.6256 / 440);
          porta_n[0] = 0;
          CHECK(sl > 0.5 && fabs(sl - a4) < 0.1, "after a note two octaves up, with the pitch sliding in over 9 ms: the sample settles at the key's own pitch (%.0f %%, %.0f %% without the slide)", sl * 100, a4 * 100); }
        #undef PITCHED
        make_click(2, 3000, 1500);
        double e_full, e_q;
        { /* gain: the sampler's word 7 at half its turn is a quarter (its square), heard over a quiet note */
          smp_rec(0, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(40);
          int16_t *base = malloc(rec_n * 2); const size_t bn = rec_n; memcpy(base, rec, bn * 2);
          double e[2];
          for (int q = 0; q < 2; q++) {
              host_knobs[2][7] = q ? 4096 : 8192;
              smp_rec(8192, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(40);
              e[q] = 0; for (size_t k = 0; k < bn; k++) e[q] += ((double)rec[k] - base[k]) * ((double)rec[k] - base[k]);
          }
          free(base); e_full = e[0]; e_q = e[1];
          CHECK(fabs(sqrt(e_q / e_full) - 0.25) < 0.03, "the sampler's gain at half: the layer at a quarter (%.3f)", sqrt(e_q / e_full)); }
        host_knobs[2][7] = 8192;
        { /* direction: the click forwards is loud then quiet; the sampler set to reverse plays it quiet then loud;
           * a flip saved by 0.9.27 .. 0.9.33 (record 11) is not read: it plays the sampler's way */
          double fw[2], rv[2], fl[2];
          #define HALVES(o) do { smp_rec(32767, 0, 0); host_eng_rec[0][REC_SFLIP] = flip; reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(60); \
              o[0] = treble(0, at(15)); o[1] = treble(at(15), at(34)); } while (0)
          int flip = 0;
          host_knobs[2][4] = 12000; HALVES(fw);
          host_knobs[2][4] = 20000; HALVES(rv);
          flip = 1; HALVES(fl);
          #undef HALVES
          CHECK(fw[0] > 4 * fw[1] && rv[1] > 4 * rv[0] && fl[1] > 4 * fl[0], "direction: forwards loud first (%.2g, %.2g), the sampler reversed quiet first (%.2g, %.2g), and an old saved flip doesn't turn it round", fw[0], fw[1], rv[0], rv[1]);
          host_knobs[2][4] = 12000; host_eng_rec[0][REC_SFLIP] = 1;
          host_eng_rec[0][REC_SFLIP] = 0; }
        host_sampler_type[2] = 0; host_sampler_rate[2] = 0; host_knobs[2][4] = 0; host_knobs[2][7] = 0; host_knobs[2][3] = 0;
        make_tone(2, 3000, 13230, 3000);
        /* legato: no new sample; retrig: one every note */
        make_click(2, 3000, 1500); smp_rec(SMP_SOLO, 0, 0);
        reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 50); mode(MODE_GLIDE); on(0, KEY_C4, 127); run_ms(200); size_t l0 = rec_n; on(1, KEY_C4 * 1.5, 127); run_ms(100);
        const double leg = treble(l0, l0 + at(30));
        offk(1); offk(0); run_ms(800);
        reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 50); mode(MODE_RETRIG); on(0, KEY_C4, 127); run_ms(200); size_t r0 = rec_n; on(1, KEY_C4 * 1.5, 127); run_ms(100);
        const double ret = treble(r0, r0 + at(30));
        offk(1); offk(0); run_ms(800); mode(MODE_GLIDE);
        CHECK(ret > 30 * leg, "a glided legato note plays no sample; a restarted one does (treble %.3g vs %.3g)", leg, ret);
        /* the source: AUTO finds the recording, a set slot without one plays nothing */
        smp_rec(32767, 0, 5);                                   /* slot 5: empty */
        reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(100);
        const double none = treble(0, at(30));
        smp_rec(32767, 0, 3);                                   /* slot 3: the click */
        reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(100);
        CHECK(treble(0, at(30)) > 50 * none, "SMP 5 (empty) plays no sample, SMP 3 does");
        offk(0); run_ms(800);
        /* its own tone: the note's TONE closed leaves the click bright; S.TONE closed darkens it. SAMPATT fades it in */
        {
            make_click(2, 3000, 1500);
            #define CLICK(tn, stn, att) (smp_rec(32767, 0, 0), host_eng_rec[0][REC_STONE] = (int16_t)(stn), host_eng_rec[0][REC_SATK] = (int16_t)(att), tone(tn), \
                                         reset(), set(SINE, 0, 4500, 16000, 0, 0, 127, 0), on(0, KEY_C4, 127), run_ms(60))
            CLICK(32767, 0, 0); const double open = treble(0, at(30));
            CLICK(8000, 0, 0); const double tone_closed = treble(0, at(30));
            CLICK(32767, 26000, 0); const double s_closed = treble(0, at(30));
            CLICK(32767, 0, 0); const double a0 = treble(0, at(4));
            CLICK(32767, 0, 32767); const double a100 = treble(0, at(4));
            CLICK(32767, 0, 0); const double b0 = treble(at(25), at(32));
            CLICK(32767, 0, 16384); const double a25 = treble(0, at(4)), a25b = treble(at(25), at(32));
            CHECK(tone_closed > 0.6 * open && s_closed < 0.1 * open, "TONE closed leaves the sample bright (%.0f %% of its treble), S.TONE closed darkens it (%.1f %%)", tone_closed / open * 100, s_closed / open * 100);
            CHECK(a100 < 0.05 * a0 && a25 < 0.1 * a0 && a25b > 0.5 * b0, "SAMPATT: the click fades in (first 4 ms %.1f %% of its treble at 100 ms, %.1f %% at 25 ms; at 25 ms it is through, %.0f %% after it)", a100 / a0 * 100, a25 / a0 * 100, a25b / b0 * 100);
            { /* and as much with the drive full: the fade is after the sample's drive, which can't undo it */
              #define CLKD(att) (smp_rec(SMP_SOLO, 0, 0), host_eng_rec[0][REC_SATK] = (int16_t)(att), reset(), set(SINE, 0, 4500, 16000, 32767, 0, 127, 0), on(0, KEY_C4, 127), run_ms(60))
              CLKD(0); const double d0 = treble(0, at(4));
              CLKD(16384); const double d25 = treble(0, at(4));
              CLKD(32767); const double d100 = treble(0, at(4));
              CHECK(d100 < 0.05 * d0 && d25 < 0.15 * d0, "SAMPATT at full drive: the click still fades in (first 4 ms %.1f %% at 100 ms, %.1f %% at 25 ms)", d100 / d0 * 100, d25 / d0 * 100);
              #undef CLKD
              host_eng_rec[0][REC_SATK] = 0; }
            wav("sample_stone_closed");
            /* driven and toned, it still ends: the voice comes back, silent */
            smp_rec(32767, 0, 0); host_eng_rec[0][REC_STONE] = 16000; tone(32767);
            reset(); set(SINE, 0, 4500, 0, 32767, 0, 0, 0); on(0, KEY_C4, 127); run_ms(5); offk(0); run_ms(600);
            CHECK(!active[0] && peak(at(400), rec_n) == 0, "driven, S.TONE half: the layer rings out and the voice is given back");
            #undef CLICK
            host_eng_rec[0][REC_STONE] = 0; host_eng_rec[0][REC_SATK] = 0; tone(32767);
            /* SMP HPF: the sample's low end goes before its drive; its top stays */
            {
                double e[2][2];
                for (int f = 0; f < 2; f++) {
                    make_tone(2, 3000, 13230, f ? 3000 : 150);
                    smp_rec(0, 0, 0); reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(150);
                    int16_t *base = malloc(rec_n * 2); const size_t bn = rec_n; memcpy(base, rec, bn * 2);
                    for (int h = 0; h < 2; h++) {
                        smp_rec(8192, 0, 0); host_eng_rec[0][REC_SHP] = (int16_t)(h ? 24576 : 0);
                        reset(); set(SINE, 0, 4500, 16000, 0, 0, 127, 0); on(0, KEY_C4, 127); run_ms(150);
                        e[f][h] = 0; for (size_t k = at(50); k < bn; k++) e[f][h] += ((double)rec[k] - base[k]) * ((double)rec[k] - base[k]);
                    }
                    free(base);
                }
                const double lo = sqrt(e[0][1] / e[0][0]), hi = sqrt(e[1][1] / e[1][0]);
                CHECK(lo < 0.06 && hi > 0.8, "SMP HPF at 75 %% (~950 Hz): a 150 Hz sample down to %.1f %%, a 3 kHz one kept (%.0f %%)", lo * 100, hi * 100);
                host_eng_rec[0][REC_SHP] = 0; make_click(2, 3000, 1500);
            }
            /* white: a push swaps it; the plain turn is S.TONE (right opens), Shift + turn SAMPATT */
            white_smp = 0; host_knobs[0][2] = 20000;
            CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 2, 1024) == 0, "white turns the OP-1's tone until it is swapped");
            ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 2, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 2, 0);
            host_eng_rec[0][REC_STONE] = 10000;
            const int t1 = ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 2, 2048);
            const int st_after = host_eng_rec[0][REC_STONE];
            host_shift_held = 1; ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 2, 4096); host_shift_held = 0;
            CHECK(t1 && white_smp && st_after == 10000 - 2048 && host_eng_rec[0][REC_SHP] == 1024 && host_knobs[0][2] == 20000,
                  "pushed: white turns S.TONE (opening: %d) and Shift + white SMP HPF (%d, at the Shift rate), the note's tone kept", st_after, host_eng_rec[0][REC_SHP]);
            ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 2, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 2, 0);
            CHECK(!white_smp, "pushed again: TONE and SUB");
            host_eng_rec[0][REC_STONE] = 0; host_eng_rec[0][REC_SATK] = 0; host_eng_rec[0][REC_SHP] = 0;
        }
        /* the controls: a green push swaps green; the turns go to the record */
        smp_rec(0, 0, 0); host_knobs[0][1] = 1000; green_smp = 0;
        CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 1, 1024) == 0, "green turns the OP-1's knock until it is swapped");
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 1, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 1, 0);
        int took = ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 1, 4096);
        CHECK(took && green_smp && host_eng_rec[0][REC_SLVL] == 4096 && host_knobs[0][1] == 1000, "pushed: a plain green turn is SAMPLE (4 detents: %d), knock kept", host_eng_rec[0][REC_SLVL]);
        host_shift_held = 1;
        int16_t rr[REC_N];
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 1, 4 * 1024); rec_get(0, rr);
        const int att4 = rr[REC_SATK];
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 1, 40 * 1024); rec_get(0, rr);
        const int a_top = rr[REC_SATK];
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 1, -60 * 1024); rec_get(0, rr);
        CHECK(att4 == 4096 && a_top == 32767 && rr[REC_SATK] == 0 && rr[REC_STUNE] == 0, "Shift + green: SMP ATK, 32 detents end to end (%d after 4), the tune untouched", att4);
        /* SMP TUNE, Shift + blue on blue's DRIVE page: a semitone a detent, -24 .. +24 */
        blue_pg = 1;
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, 3 * 1024); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, -512);
        rec_get(0, rr);
        const int t3 = rr[REC_STUNE];
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, 40 * 1024); rec_get(0, rr);
        const int t12 = rr[REC_STUNE];
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, -60 * 1024); rec_get(0, rr);
        blue_pg = 0;
        CHECK(t3 == 3 && t12 == 24 && rr[REC_STUNE] == -24 && host_eng_rec[0][REC_STUNE] == 1, "Shift + blue (DRIVE page): SMP TUNE a semitone a detent, -24 .. +24 (%d, %d, %d)", t3, t12, rr[REC_STUNE]);
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 1, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 1, 0);
        const int sh_back = !green_smp;
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 1, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 1, 0);
        host_shift_held = 0;
        CHECK(sh_back && green_smp, "a green push with Shift held swaps both ways too: back to knock, then to the sample again");
        ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 1, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 1, 0);
        CHECK(!green_smp && ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 1, 1024) == 0, "pushed again: green is knock again");
        int seq[10];
        for (int i = 0; i < 10; i++) { ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 1, 1); ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 1, 0); rec_get(0, rr); seq[i] = rr[REC_SRC]; }
        CHECK(seq[0] == 2 && seq[6] == 8 && seq[7] == SRC_AUTO && seq[8] == 2, "T2 green push: AUTO, 2 .. 8 (not tyr8's own slot 1), AUTO (%d %d .. %d %d %d)", seq[0], seq[1], seq[6], seq[7], seq[8]);
        CHECK(ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_TURN, 1, 1024) == 0, "T2 turns stay the OP-1's envelope");
        smp_rec(0, 0, 0); no_samples();
        free(ref);
    }

    /* a phrase to listen to */
    reset(); set(10000, 14000, 6000, 20000, 14000, 12000, 0, 60);
    const double ph_hz[8] = { 43.65, 43.65, 51.91, 43.65, 65.41, 58.27, 43.65, 38.89 };
    for (int i = 0; i < 8; i++) { on(i % 6, ph_hz[i], i & 1 ? 100 : 127); run_ms(i == 4 ? 180 : 120); if (i != 4) offk(i % 6); run_ms(i == 4 ? 0 : 260); if (i == 5) offk(4); }
    run_ms(2500);
    wav("phrase");

    /* ---- screen ---- */
    if (outdir) { char path[512]; snprintf(path, sizeof path, "%s/frames.json", outdir); fj = fopen(path, "w"); if (fj) fprintf(fj, "["); }
    SCREEN_tyr8.init(0);
    reset(); vis_lvl = 0; vis_inc = 0;
    memset(host_knobs, 0, sizeof host_knobs); sub(0); sag(0);
    host_knobs[0][6] = 0; host_knobs[0][5] = 0; host_t2[0][1] = T2_LO;
    for (int i = 0; i < 60; i++) frame("small slick, at rest");
    int lugsides = 0; for (int q = 0; q < npolys; q++) lugsides += polys[q].n == 4;
    CHECK(lugsides == 0, "drive and clip at 0: a racing slick, no tread blocks (%d)", lugsides);
    /* on a slick the far rim shows only along the tyre's own outline: none of it is drawn across the tread */
    int hidden = 0, farl = 0;
    for (int j = 0; j < nlines; j++) {
        if (lines[j].c[0] != 0xDF || lines[j].c[1] != 0xD9) continue;
        farl++;
        const double mx = (lines[j].x0 + lines[j].x1) / 2.0, my = (lines[j].y0 + lines[j].y1) / 2.0;
        double best = 1e18;
        for (int i = 0; i < sil_n; i++) {
            const double ax = sil[i].x, ay = sil[i].y, dx = sil[(i + 1) % sil_n].x - ax, dy = sil[(i + 1) % sil_n].y - ay, L2 = dx * dx + dy * dy;
            double u = L2 > 0 ? ((mx - ax) * dx + (my - ay) * dy) / L2 : 0;
            u = u < 0 ? 0 : (u > 1 ? 1 : u);
            const double d = hypot(mx - ax - u * dx, my - ay - u * dy);
            if (d < best) best = d;
        }
        if (best > 1.5 * 1024) hidden++;
    }
    CHECK(farl > 5 && hidden == 0, "the back of the tyre is hidden by the tyre: %d of %d far-rim lines run across the tread", hidden, farl);
    double low, fw, ht, ht_small;
    near_shape(&low, &fw, &ht_small);
    /* the near face's lowest point is in front of the contact, where the road is lower: compare with the road there */
    CHECK(fabs(vis_hub_y / 1024.0 + last_wheel.rout / 1024.0 - CON_Y) < 0.6, "at rest the tyre stands on the road (contact y %.2f)", vis_hub_y / 1024.0 + last_wheel.rout / 1024.0);
    int above = 1; { op1_pt nf[LUG_N / 2 * 5]; wheel_t w = last_wheel; const int n = outline(&w, -(int32_t)(ang >> 8), 0, nf);
        for (int i = 0; i < n; i++) { const double sx = (nf[i].x / 1024.0 - CON_X) / 0.894, ry = CON_Y - 0.447 * sx; if (nf[i].y / 1024.0 > ry + 0.6) above = 0; } }
    CHECK(above, "no part of the near face goes below the road");
    int blk = 0; for (int i = 0; i < 400 * 256; i++) blk += host_frame565[i] == 0;
    CHECK(blk > 4000, "the tyre's rubber is filled black in the panel's pixels (%d)", blk);
    host_t2[0][1] = T2_HI; host_knobs[0][6] = 32767;
    host_knobs[0][1] = 0; frame("knock 0: a smooth road");
    CHECK(vis_rocks == 0 && vis_cracks == 0 && vis_weeds == 0 && vis_tufts > 15, "knock 0: no rocks, cracks or weeds; the grass is there (%d tufts)", vis_tufts);
    host_knobs[0][1] = 32767; frame("knock full: rocks, cracks, weeds");
    CHECK(vis_rocks >= 3 && vis_cracks >= 6 && vis_weeds >= 2, "knock full: %d rocks, %d cracks, %d weeds", vis_rocks, vis_cracks, vis_weeds);
    host_knobs[0][1] = 20000;
    for (int i = 0; i < 60; i++) frame("monster truck, off-road lugs, at rest");
    near_shape(&low, &fw, &ht);
    CHECK(ht > ht_small * 1.5, "decay sets the size, and clip the lugs: %.1f px high at the smallest slick, %.1f at the largest with lugs", ht_small, ht);
    host_knobs[0][6] = 0; frame("big slick"); double ht_slick; near_shape(&low, &fw, &ht_slick);
    host_knobs[0][5] = 32767; frame("big, clip 0, drive full"); double ht_drv; near_shape(&low, &fw, &ht_drv);
    CHECK(fabs(ht_drv - ht) < 0.6, "drive alone raises the lugs as clip does: the higher of the two (%.1f and %.1f px)", ht_drv, ht);
    host_knobs[0][5] = 0;
    CHECK(ht - ht_slick > 2 * 9 * 0.92 * 0.8, "full lugs stand out of the slick (%.1f px higher)", ht - ht_slick);
    host_knobs[0][6] = 32767; frame("monster truck again");
    double fw0; near_shape(&low, &fw0, &ht);
    CHECK(low > -0.6 && low < 0.6, "the lowest point of the near face is on the road (%.2f px above it)", low);
    sag(32767); frame("flat: sag full"); double fw1, ht1; near_shape(&low, &fw1, &ht1);
    CHECK(fw1 > fw0 + 15 && ht1 < ht - 6 && low > -0.6 && low < 0.6, "sag flattens the bottom onto the road: %.1f px of it on the road (%.1f round), %.1f px lower", fw1, fw0, ht - ht1);
    CHECK(fabs(vis_hub_y / 1024.0 + (last_wheel.rout - last_wheel.q) / 1024.0 - CON_Y) < 0.6, "and the flat stays on the road");
    sag(0); rise(32767); frame("blown up: sag -100 %"); double fw2, ht2; near_shape(&low, &fw2, &ht2);
    CHECK(ht2 > ht + 4 && low > -0.6 && low < 0.6, "sag below 0 blows the tyre up: %.1f px taller (contact %.1f px, %.1f round), still on the road (%.2f)", ht2 - ht, fw2, fw0, low);
    { const int16_t d0 = host_t2[0][1], c6 = host_knobs[0][6], c5 = host_knobs[0][5];
      host_t2[0][1] = t2_of(0.4); host_knobs[0][6] = 0; host_knobs[0][5] = 0;
      rise(0); frame("mid size slick"); double fwm, htm, low0, ht0; near_shape(&low0, &fwm, &ht0); rise(32767);
      frame("blown up: mid size slick"); near_shape(&low, &fwm, &htm);
      CHECK(low > low0 - 0.8 && low < low0 + 0.8 && htm > ht0 + 8, "a mid-size slick blown up: %.1f px taller, as low as round (%.2f, round %.2f)", htm - ht0, low, low0);
      host_t2[0][1] = T2_HI; frame("blown up: largest slick");                  /* a still to look at */
      host_t2[0][1] = d0; host_knobs[0][6] = c6; host_knobs[0][5] = c5; }
    rise(0);
    sub(0); frame("sub 0: a plain dish");
    CHECK(vis_spokes == 0 && !vis_whitewall && !vis_spinner, "sub 0: no spokes, no whitewall, no spinner");
    sub(9830); frame("sub 30 %: spokes growing");
    CHECK(vis_spokes >= 10 && vis_spokes <= 14 && !vis_whitewall, "sub 30 %%: %d spokes, no whitewall yet", vis_spokes);
    sub(21000); frame("sub 64 %: whitewall");
    CHECK(vis_whitewall && !vis_spinner, "sub 64 %%: %d spokes and the whitewall", vis_spokes);
    sub(32767); frame("sub full: lowrider");
    int gold = 0; for (int j = 0; j < nlines; j++) if (lines[j].c[0] == 0xFF && lines[j].c[1] == 0xC8) gold++;
    CHECK(vis_spokes == 40 && vis_whitewall && vis_spinner && gold >= 10, "sub full: 40 spokes, whitewall, gold spinner (%d gold lines)", gold);
    sag(32767); frame("lowrider, flat"); sag(0);
    /* a note: bounce and spin, with everything at its largest */
    sub(32767); sag(32767);
    vis_trig++; vis_lvl = 32767; vis_inc = (uint32_t)(55 / 88200.0 * 4294967296.0);
    const int32_t rest = vis_hub_y;
    int32_t top = rest; uint32_t a0 = ang; int offscreen = 0, shadow = 0, escapes = 0;
    for (int i = 0; i < 90; i++) {
        frame(i == 6 ? "note on: bounce" : i == 30 ? "rolling" : "rolling on");
        if (vis_hub_y < top) top = vis_hub_y;
        if (i == 6) for (int j = 0; j < nlines; j++) shadow += lines[j].c[0] == 0x6F && lines[j].y0 > OP1_Q10(115);
        for (int j = 0; j < nlines; j++) if (lines[j].y1 > OP1_Q10(150) || lines[j].y0 > OP1_Q10(150) || lines[j].x0 < OP1_Q10(2) || lines[j].x1 < OP1_Q10(2) || lines[j].x0 > OP1_Q10(318) || lines[j].x1 > OP1_Q10(318) || lines[j].y0 < OP1_Q10(2) || lines[j].y1 < OP1_Q10(2)) offscreen = 1;
        for (int q = 0; q < npolys; q++) for (int j = 0; j < polys[q].n; j++) if (polys[q].p[j].y > OP1_Q10(150) || polys[q].p[j].y < OP1_Q10(2) || polys[q].p[j].x < OP1_Q10(2) || polys[q].p[j].x > OP1_Q10(318)) offscreen = 1;
        escapes += dyn_escapes();
    }
    CHECK((rest - top) / 1024.0 > 5, "a note kicks the wheel off the road (%.1f px)", (rest - top) / 1024.0);
    CHECK(shadow >= 2, "and its shadow stays on the road");
    sag(0); rise(32767); vis_trig++;
    for (int i = 0; i < 90; i++) {
        frame(i == 6 ? "blown up: bounce" : "blown up, rolling");
        for (int j = 0; j < nlines; j++) if (lines[j].y1 > OP1_Q10(150) || lines[j].y0 > OP1_Q10(150) || lines[j].x0 < OP1_Q10(2) || lines[j].x1 < OP1_Q10(2) || lines[j].x0 > OP1_Q10(318) || lines[j].x1 > OP1_Q10(318) || lines[j].y0 < OP1_Q10(2) || lines[j].y1 < OP1_Q10(2)) offscreen = 1;
        for (int q = 0; q < npolys; q++) for (int j = 0; j < polys[q].n; j++) if (polys[q].p[j].y > OP1_Q10(150) || polys[q].p[j].y < OP1_Q10(2) || polys[q].p[j].x < OP1_Q10(2) || polys[q].p[j].x > OP1_Q10(318)) offscreen = 1;
        escapes += dyn_escapes();
    }
    rise(0); sag(32767);
    CHECK(!offscreen, "nothing goes off the screen or below its lower margin, at the largest size, flat or blown up, lowrider, bouncing");
    host_knobs[0][1] = 32767; for (int i = 0; i < 40; i++) { frame("rough road, rolling"); escapes += dyn_escapes(); } host_knobs[0][1] = 20000;
    { /* the easter egg: Shift + orange push on T1 turns the sidewall's markings on and off (a plain one doesn't); they
       * stay inside the moving area over a turn, on the plain sidewall and the whitewall, flat or not */
      const int was = tyre_badge;
      host_shift_held = 1; ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 0); host_shift_held = 0;
      const int on = tyre_badge;
      ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 0);
      const int plain = tyre_badge;
      int esc = 0, marks = 0;
      for (int k = 0; k < 4; k++) {
          sub(k & 1 ? 32767 : 0); sag(k & 2 ? 32767 : 0);
          for (int i = 0; i < 30; i++) { frame(k == 0 ? "markings: GRIFTY, TYR8, the flag" : "markings, rolling"); esc += dyn_escapes(); }
          for (int j = 0; j < nlines; j++) marks += lines[j].c[0] == 0 && lines[j].c[1] == 0 && lines[j].c[2] == 0;   /* black on a whitewall */
      }
      host_shift_held = 1; ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 0); host_shift_held = 0;
      CHECK(!was && on && plain && !tyre_badge && esc == 0 && marks > 20,
            "Shift + orange push: the sidewall markings on (a plain push leaves them) and off again; over a turn they stay in the moving area (%d escapes, %d black strokes on the whitewall)", esc, marks);
      sub(32767); sag(32767); }
    CHECK(escapes == 0 && dyn_from > 0, "no moving stroke or fill reaches the still label layer (%d escapes)", escapes);
    { /* every size, slick and lugs, flat, round and blown up, knock 0 and full, kicked off the road again and again:
       * nothing reaches the label layer, and the red strain line stays on the tyre */
      const int16_t d0 = host_t2[0][1], c6 = host_knobs[0][6], k1 = host_knobs[0][1];
      int esc = 0, off = 0, worst_sz = -1;
      for (int sz = 0; sz < 3; sz++) for (int lug = 0; lug < 2; lug++) for (int sg = 0; sg < 3; sg++) for (int kn = 0; kn < 2; kn++) {
          host_t2[0][1] = t2_of(sz * 0.5); host_knobs[0][6] = lug ? 32767 : 0; host_knobs[0][5] = 0;
          sag(sg == 0 ? 32767 : 0); rise(sg == 2 ? 32767 : 0); host_knobs[0][1] = kn ? 32767 : 0;
          vis_lvl = 32767; vis_inc = (uint32_t)(55 / 88200.0 * 4294967296.0);
          for (int i = 0; i < 150; i++) {
              if (i % 30 == 0) vis_trig++;
              frame("stress: bouncing");
              const int e = dyn_escapes(); esc += e; if (e && worst_sz < 0) worst_sz = sz * 100 + lug * 10 + sg;
              if (getenv("TY_STRESS")) printf("stress top sz %d lug %d sag %d knock %d esc %d top %.1f\n", sz, lug, sg, kn, e, (vis_hub_y - last_wheel.rout - last_wheel.inf - last_wheel.bl) / 1024.0);
              if (e && getenv("TY_STRESS")) printf("stress esc sz %d lug %d sag %d knock %d frame %d: %d\n", sz, lug, sg, kn, i, e);
              for (int q = dyn_from; q < nops; q++) { if (ops[q] < 0) continue; const hline *l = &lines[ops[q]];   /* the moving layer */
                  int in0 = 1, in1 = 1;                          /* the whole stroke inside the outline: every point */
                  for (int d = 0; d < 9; d++) { const double ox = (d % 3 - 1) * l->w / 2.0, oy = (d / 3 - 1) * l->w / 2.0;   /* half its width round */
                      in0 &= in_convex(sil, sil_n, l->x0 + ox, l->y0 + oy); in1 &= in_convex(sil, sil_n, l->x1 + ox, l->y1 + oy); }
                  if (l->c[0] == 0xFF && l->c[1] == 0x3A && (!in0 || !in1)) {
                      if (getenv("TY_STRESS") && off < 8) printf("stress off sz %d lug %d sag %d kn %d: (%.1f,%.1f)-(%.1f,%.1f) w %d hub (%.1f,%.1f)\n", sz, lug, sg, kn, l->x0/1024.0, l->y0/1024.0, l->x1/1024.0, l->y1/1024.0, l->w, last_wheel.hx/1024.0, last_wheel.hy/1024.0);
                      off++; } }
          }
      }
      CHECK(esc == 0, "bouncing at every size, flat to blown up, knock 0 and full: nothing reaches the labels (%d escapes, first at size/lugs/sag %d)", esc, worst_sz);
      CHECK(off == 0, "and nothing red on the tyre (the strain line, the valve) leaves its outline, flat or blown up (%d strokes off it)", off);
      vis_lvl = 0; for (int i = 0; i < 400; i++) frame("coasting");
      host_t2[0][1] = d0; host_knobs[0][6] = c6; host_knobs[0][1] = k1; sag(32767); rise(0); }
    CHECK(ang != a0, "the wheel turns while a note sounds");
    vis_lvl = 0; for (int i = 0; i < 400; i++) frame("coasting");
    a0 = ang; frame("stopped");
    CHECK(ang == a0, "and stops after it");
    sub(0); sag(0);
    host_shift_held = 1; host_knobs[0][5] = 16384; host_knobs[0][7] = 32767; sub(32767); sag(32767); glide_rec(32767); frame("shift held: drive, knock length, sub, glide");
    /* text strokes are the 1 px lines; everything else the screen draws is 1.5 px */
#define TEXT_SCAN(cnt, bad, cor) do { cnt = bad = 0; memset(cor, 0, sizeof cor); for (int j = 0; j < nlines; j++) if (lines[j].w == 1024) { \
        const int32_t xa = lines[j].x0 < lines[j].x1 ? lines[j].x0 : lines[j].x1, xb = lines[j].x0 > lines[j].x1 ? lines[j].x0 : lines[j].x1; \
        cnt++; if (on_tyre(lines[j].x0, lines[j].y0, lines[j].x1, lines[j].y1) || xa < OP1_Q10(6) || xb > OP1_Q10(314)) bad++; \
        cor[(lines[j].y0 > OP1_Q10(80)) * 2 + (xa > OP1_Q10(160))]++; } } while (0)
#define TEXT_HASH(h) do { h = 0; for (int j = 0; j < nlines; j++) if (lines[j].w == 1024) h = h * 31 + (unsigned)(lines[j].x0 + 7 * lines[j].y0 + 13 * lines[j].x1 + 17 * lines[j].y1); } while (0)
    int txt, txt2, bad, bad2, cor[4], cor2[4]; unsigned h1, h2;
    TEXT_SCAN(txt, bad, cor); TEXT_HASH(h1);
    host_shift_held = 0; frame("labels");
    TEXT_SCAN(txt2, bad2, cor2); TEXT_HASH(h2);
    CHECK(txt > 60 && txt2 > 60 && h1 != h2, "labels are drawn, and Shift shows the second set (%d and %d strokes)", txt2, txt);
    CHECK(cor2[0] > 8 && cor2[1] > 8 && cor2[2] > 8 && cor2[3] > 8, "one label block in each corner (%d, %d, %d, %d strokes)", cor2[0], cor2[1], cor2[2], cor2[3]);
    CHECK(!bad && !bad2, "labels stay in their corners, clear of the tyre (largest, flat), the road and the screen edge");
    mode(MODE_RETRIG); frame("mode: mono retrig"); TEXT_SCAN(txt2, bad2, cor2); mode(MODE_GLIDE);
    CHECK(!bad2, "the longest mode name fits");
    host_shift_held = 1; blue_pg = 1; frame("shift held, blue on the DRIVE page: SMP TUNE"); int txt3, bad3, cor3[4]; TEXT_SCAN(txt3, bad3, cor3);
    host_shift_held = 0; frame("blue on the DRIVE page"); { int t5, b5, c5[4]; TEXT_SCAN(t5, b5, c5);
    CHECK(!bad3 && !b5 && txt3 != txt && c5[0] > 8, "blue's DRIVE page shows DRIVE, and SMP TUNE with Shift, and they fit (%d strokes)", txt3); }
    blue_pg = 0; host_shift_held = 0; sub(0); sag(0);
    host_knobs[0][0] = 0; frame("shape at -50 %"); TEXT_SCAN(txt3, bad3, cor3);
    CHECK(!bad3, "the shape value fits with its sign");
    host_knobs[0][0] = 8192;
    host_knobs[0][2] = 32767; sag(32767); host_shift_held = 1; glide_rec(32767); frame("shift held, glide 500 ms"); host_shift_held = 0; frame("tone open, sag 100 %"); TEXT_SCAN(txt3, bad3, cor3); CHECK(!bad3, "the longest tone and sag values fit"); host_shift_held = 1; frame("shift held, glide 500 ms"); TEXT_SCAN(txt3, bad3, cor3);
    CHECK(!bad3, "the longest tone and glide values fit their corners");
    host_shift_held = 0; sag(0); rise(32767); frame("sag -100 %"); TEXT_SCAN(txt3, bad3, cor3); rise(0);
    CHECK(!bad3, "sag -100 %% fits, with its sign");
    host_shift_held = 0;

    green_smp = 1; white_smp = 1; smp_rec(19661, 5, 0); host_eng_rec[0][REC_STONE] = 8192; host_eng_rec[0][REC_SATK] = 32767; host_eng_rec[0][REC_SHP] = 16384;
    frame("green and white on the sample: SAMPLE 60 %, S.TONE 75 %"); TEXT_SCAN(txt3, bad3, cor3);
    unsigned hg; TEXT_HASH(hg);
    host_shift_held = 1; frame("shift, on the sample: SMP ATK 100 MS, SMP HPF 50 %"); { int t4, b4, c4[4]; TEXT_SCAN(t4, b4, c4);
    CHECK(!bad3 && !b4 && cor3[1] > 8 && c4[1] > 8 && cor3[2] > 8 && c4[2] > 8, "green and white swapped: SAMPLE, SMP ATK, S.TONE and SMP HPF fit in their corners"); }
    host_shift_held = 0; green_smp = 0; white_smp = 0; smp_rec(0, 0, 0); host_eng_rec[0][REC_STONE] = 0; host_eng_rec[0][REC_SATK] = 0; host_eng_rec[0][REC_SHP] = 0; (void)hg;

    { /* every name beside its longest value, with a gap: KNOCK LEN once ran into 855MS */
      static const char *const pairs[][2] = { { "SHAPE", "-50%" }, { "DRIVE", "100%" }, { "CLIP", "100%" }, { "KNOCK", "100%" },
          { "KNCKLEN", "999MS" }, { "SMP HPF", "100%" }, { "TONE", "100%" }, { "SUB", "100%" }, { "SAG", "-100%" }, { "GLIDE", "500MS" },
          { "SAMPLE", "100%" }, { "SMP REV", "100%" }, { "SMP TUNE", "-24" }, { "S.TONE", "100%" }, { "SMP ATK", "100MS" },
          { "ATTACK", "999MS" }, { "DECAY", "4.0S" }, { "SUSTAIN", "100%" }, { "RELEASE", "999MS" } };
      int worst = 1 << 30; const char *wn = "";
      for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
          const int gap = OP1_Q10(LBL_W) + OP1_Q10(2) - sq_width(LBL_S, pairs[i][1]) - sq_width(LBL_S, pairs[i][0]);
          if (gap < worst) { worst = gap; wn = pairs[i][0]; }
      }
      CHECK(worst >= OP1_Q10(4), "every label's name clears its longest value by 4 px or more (closest: %s, %.1f px)", wn, worst / 1024.0); }

    /* the T2 page: the envelope curve and four labels */
#define T2_FRAME(label) frame_of(&SCREEN_tyr8_t2, label)
    sil_n = 0; road_nseg = 0;                                  /* T2 has no tyre and no road: only the screen's edges count */
    host_t2[0][0] = t2_of(0.2); host_t2[0][1] = t2_of(0.5); host_t2[0][2] = 16384; host_t2[0][3] = t2_of(0.5);
    T2_FRAME("T2: attack, decay, sustain half, release");
    int curve = 0, outside = 0; for (int j = 0; j < nlines; j++) { if (lines[j].w != 1024) curve++;
        if (lines[j].x0 < 0 || lines[j].x1 > OP1_Q10(316) || lines[j].y0 < 0 || lines[j].y1 > OP1_Q10(140)) outside++; }
    TEXT_SCAN(txt3, bad3, cor3); unsigned h3; TEXT_HASH(h3);
    CHECK(curve > 30 && !outside, "T2 draws the envelope curve on screen (%d lines)", curve);
    CHECK(cor3[0] > 8 && cor3[1] > 8 && cor3[2] > 8 && cor3[3] > 8 && !bad3, "T2: a label in each corner, clear of the middle");
    host_t2[0][3] = T2_HI; host_t2[0][0] = T2_HI; host_t2[0][1] = T2_HI; host_t2[0][2] = 32767; T2_FRAME("T2: all up, release HOLD");
    unsigned h4; TEXT_HASH(h4); TEXT_SCAN(txt3, bad3, cor3);
    CHECK(h4 != h3 && !bad3, "T2: HOLD and the longest values fit");
    host_t2[0][0] = T2_LO; host_t2[0][1] = T2_LO; host_t2[0][2] = 0; host_t2[0][3] = T2_LO; T2_FRAME("T2: all down");
    TEXT_SCAN(txt3, bad3, cor3);
    CHECK(!bad3, "T2: the shortest values fit");
    { /* the sample's source under green's block: in green with a recording, grey without; above the curve */
      host_t2[0][0] = t2_of(0.2); host_t2[0][1] = t2_of(0.5); host_t2[0][2] = 16384; host_t2[0][3] = t2_of(0.5);
      int lo_y = 1 << 30, hi_y = 0, grn = 0, gry = 0;
      make_click(2, 3000, 1500); smp_rec(32767, 0, 0); for (int i = 0; i < 20; i++) T2_FRAME("T2: SMP AUTO 3");
      for (int j = 0; j < nlines; j++) if (lines[j].w == 1024 && lines[j].y0 > OP1_Q10(20) && lines[j].y0 < OP1_Q10(33)) {
          grn += !memcmp(lines[j].c, COL_KNOB[1], 3);
          const int32_t ya = lines[j].y0 < lines[j].y1 ? lines[j].y0 : lines[j].y1, yb = lines[j].y0 > lines[j].y1 ? lines[j].y0 : lines[j].y1;
          if (ya < lo_y) lo_y = ya;
          if (yb > hi_y) hi_y = yb; }
      TEXT_SCAN(txt3, bad3, cor3);
      no_samples(); smp_rec(32767, 0, 5); T2_FRAME("T2: SMP 5 -, no recording");
      for (int j = 0; j < nlines; j++) if (lines[j].w == 1024 && lines[j].y0 > OP1_Q10(20) && lines[j].y0 < OP1_Q10(33)) gry += !memcmp(lines[j].c, COL_GREY, 3);
      CHECK(grn > 10 && gry > 10 && !bad3 && lo_y > OP1_Q10(20) && hi_y < OP1_Q10(ENV_TOP), "T2: the sample's source under green's block, green with a recording, grey without, above the curve (%d, %d strokes)", grn, gry);
      smp_rec(0, 0, 0); }
    if (fj) { fprintf(fj, "]\n"); fclose(fj); }

    printf("audio hash %08x\n", audio_hash);
    printf(fails ? "\n%d FAILED\n" : "\nall ok\n", fails);
    return fails != 0;
}
