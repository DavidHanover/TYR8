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

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- audio driver: six voices as the firmware runs them ---- */
static int sag_w;                             /* sag: word 3 (orange) since 0.9.9 */
static int16_t P[8];
static void sag(int v) { sag_w = v; P[3] = (int16_t)v; host_knobs[0][3] = (int16_t)v; }
static void glide_rec(int v) { host_eng_rec[0][REC_GLIDE] = (int16_t)v; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
static void sub(int v) { host_eng_rec[0][REC_SUB] = (int16_t)v; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
static void mode(int m) { host_eng_rec[0][0] = (int16_t)m; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
#define SINE 8192                             /* the shape knob's plain sine: a quarter up */
#define T2_LO 64                              /* R246's range of the T2 attack, decay and release words */
#define T2_HI 16320
static int tone_w = 32767, atk_w = T2_LO, rel_w = T2_HI;   /* tone open, no attack, release HOLD */
static int active[6]; static double key_hz[6];      /* the key's own frequency, as the firmware gives it */
static int16_t *rec; static size_t rec_n, rec_cap;
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
static void reset(void)
{
    memset((void *)held, 0, sizeof held); owner = -1; memset(tv, 0, sizeof tv);
    memset(active, 0, sizeof active); rec_n = 0;
}
static void on(int v, double f, int velocity)
{ key_hz[v] = f * (1 << OCT_DOWN); active[v] = 1; host_voice_vel[v] = (int16_t)(velocity << 8); ENGINE_tyr8.ext.voice.note_on(OP1_VOICE(v), P); }
static void offk(int v) { ENGINE_tyr8.ext.voice.note_off(OP1_VOICE(v)); }
static void run_ms(double ms)
{
    int blocks = (int)(ms * 88.2 / 128 + 0.5);
    for (int b = 0; b < blocks; b++) {
        int32_t sum[128] = { 0 }; int16_t o[128];
        for (int v = 0; v < 6; v++) {
            if (!active[v]) continue;
            if (ENGINE_tyr8.ext.finished(OP1_VOICE(v))) { active[v] = 0; continue; }
            for (int n = 0; n < 128; n++) host_pitch_inc[n] = (int32_t)(key_hz[v] / 88200.0 * 4294967296.0);
            ENGINE_tyr8.ext.voice.render(OP1_VOICE(v), o, P);
            for (int n = 0; n < 128; n++) sum[n] += o[n];
        }
        if (rec_n + 128 > rec_cap) { rec_cap = rec_cap ? rec_cap * 2 : 1 << 20; rec = realloc(rec, rec_cap * 2); }
        for (int n = 0; n < 128; n++) { rec[rec_n++] = op1_sat16(sum[n]); audio_hash = (audio_hash ^ (uint16_t)rec[rec_n - 1]) * 16777619u; }
    }
}
static int peak(size_t a, size_t b) { int p = 0; for (size_t i = a; i < b && i < rec_n; i++) if (abs(rec[i]) > p) p = abs(rec[i]); return p; }
static size_t at(double ms) { return (size_t)(ms * 88.2); }
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
    fprintf(fj, "%s{\"label\":\"%s\",\"ops\":[", nframes++ ? ",\n" : "", label);
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
    nlines = npolys = nops = 0;
    for (int i = 0; i < 400 * 256; i++) host_frame565[i] = 0xFFFF;
    scr->update(0, 16);
    dump(label);
}
static void frame(const char *label) { frame_of(&SCREEN_tyr8, label); }

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
    int lin = 1; for (size_t i = at(200) + 2; i < at(200) + 300; i++) { int d1 = rec[i] - rec[i - 1], d0 = rec[i - 1] - rec[i - 2]; if ((d1 > 0) == (d0 > 0) && abs(d1 - d0) > 6) lin = 0; }
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
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1) == 1 && host_eng_rec[0][0] == MODE_RETRIG, "orange push: glide -> retrig");
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 0); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1);
    CHECK(host_eng_rec[0][0] == MODE_GLIDE, "orange push: retrig -> glide (two modes, no poly)");
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 3, 1024) == 0 && ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 3, 1) == 0 && host_eng_rec[0][0] == MODE_GLIDE, "orange's turn, and other pages, are left to the OP-1");
    host_shift_held = 1; ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1); host_shift_held = 0;
    CHECK(host_eng_rec[0][0] == MODE_RETRIG, "orange push with Shift held still steps the play mode");
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
    for (int c = 0; c < 3; c++) {                               /* full drive, hard: plain, tone closed, sub full */
        sub(c == 2 ? 32767 : 0); tone(c == 1 ? 0 : 32767); reset(); set(SINE, 0, 0, 6000, 32767, 32767, 127, 0); on(0, 55, 127); run_ms(600);
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

    /* 12b. the input hook: a plain turn is the OP-1's, every Shift + turn is TYR8's */
    host_slot = 0; host_shift_held = 0; blue_clip = 0; memset(host_eng_rec[0], 0, sizeof host_eng_rec[0]); mode(MODE_GLIDE);
    memset(host_knobs, 0, sizeof host_knobs); host_knobs[0][5] = 14000; host_knobs[0][6] = 12000; host_knobs[0][7] = 4500;
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, 1024) == 0 && ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 2, 1024) == 0, "a plain turn is the OP-1's");
    CHECK(shift_turn(0, 4 * 1024) == 1 && host_knobs[0][5] == 15024 && host_knobs[0][6] == 12000, "Shift + blue, four detents: drive %d (+1024), clip untouched", host_knobs[0][5]);
    host_shift_held = 1; ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 0, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 0, 0); host_shift_held = 0;
    CHECK(blue_clip == 1 && shift_turn(0, -2 * 1024) == 1 && host_knobs[0][6] == 11488 && host_knobs[0][5] == 15024, "Shift + blue push switches to clip: clip %d, drive kept", host_knobs[0][6]);
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 0, 1);
    CHECK(blue_clip == 1, "a blue push without Shift does not switch");
    host_shift_held = 1; ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 0, 1); host_shift_held = 0;
    CHECK(blue_clip == 0, "a second Shift + blue push is back to drive");
    shift_turn(1, 300 * 1024);
    CHECK(host_knobs[0][7] == 32767, "Shift + green: knock length, stops at the top (%d)", host_knobs[0][7]);
    shift_turn(2, 10 * 1024); shift_turn(3, 3 * 1024); shift_turn(3, -1024);
    CHECK(host_eng_rec[0][REC_SUB] == 2560 && host_eng_rec[0][REC_GLIDE] == 512 && host_eng_rec[0][REC_MODE] == MODE_GLIDE, "Shift + white: sub %d, Shift + orange: glide %d, the mode kept", host_eng_rec[0][REC_SUB], host_eng_rec[0][REC_GLIDE]);
    shift_turn(2, -100 * 1024);
    CHECK(host_eng_rec[0][REC_SUB] == 0, "sub stops at 0");
    CHECK(ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_TURN, 0, 1024) == 0, "T2's turns are the OP-1's (it turns the envelope words)");
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
    sag(0);

    /* the sound this was for: full feedback saw, a long deep knock, sag, drive up, the tone half closed */
    sag(20479); tone(18430); reset(); set(31500, 22000, 20000, 26000, 28000, 26000, 0, 0);
    on(0, 43.65, 127); run_ms(150); offk(0); run_ms(1400); on(1, 38.89, 127); run_ms(150); offk(1); run_ms(5000);
    CHECK(peak(0, rec_n) <= 32768 && !active[0] && !active[1] && peak(rec_n - at(100), rec_n) == 0, "the doom patch stays in range and ends");
    wav("doom");
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
    double low, fw, ht, ht_small;
    near_shape(&low, &fw, &ht_small);
    /* the near face's lowest point is in front of the contact, where the road is lower: compare with the road there */
    CHECK(fabs(vis_hub_y / 1024.0 + last_wheel.rout / 1024.0 - CON_Y) < 0.6, "at rest the tyre stands on the road (contact y %.2f)", vis_hub_y / 1024.0 + last_wheel.rout / 1024.0);
    int above = 1; { op1_pt nf[LUG_N / 2 * 5]; wheel_t w = last_wheel; const int n = outline(&w, -(int32_t)(ang >> 8), 0, nf);
        for (int i = 0; i < n; i++) { const double sx = (nf[i].x / 1024.0 - CON_X) / 0.894, ry = CON_Y - 0.447 * sx; if (nf[i].y / 1024.0 > ry + 0.6) above = 0; } }
    CHECK(above, "no part of the near face goes below the road");
    int blk = 0; for (int i = 0; i < 400 * 256; i++) blk += host_frame565[i] == 0;
    CHECK(blk > 4000, "the tyre's rubber is filled black in the panel's pixels (%d)", blk);
    host_t2[0][1] = T2_HI; host_knobs[0][6] = 32767; host_knobs[0][1] = 20000;
    for (int i = 0; i < 60; i++) frame("monster truck, off-road lugs, at rest");
    near_shape(&low, &fw, &ht);
    CHECK(ht > ht_small * 1.6 * 1.1, "decay sets the size, and clip the lugs: %.1f px high at the smallest slick, %.1f at the largest with lugs", ht_small, ht);
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
    sag(0);
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
    int32_t top = rest; uint32_t a0 = ang; int offscreen = 0, shadow = 0;
    for (int i = 0; i < 90; i++) {
        frame(i == 6 ? "note on: bounce" : i == 30 ? "rolling" : "rolling on");
        if (vis_hub_y < top) top = vis_hub_y;
        if (i == 6) for (int j = 0; j < nlines; j++) shadow += lines[j].c[0] == 0x6F && lines[j].y0 > OP1_Q10(115);
        for (int j = 0; j < nlines; j++) if (lines[j].y1 > OP1_Q10(150) || lines[j].y0 > OP1_Q10(150) || lines[j].x0 < OP1_Q10(2) || lines[j].x1 < OP1_Q10(2) || lines[j].x0 > OP1_Q10(318) || lines[j].x1 > OP1_Q10(318) || lines[j].y0 < OP1_Q10(2) || lines[j].y1 < OP1_Q10(2)) offscreen = 1;
        for (int q = 0; q < npolys; q++) for (int j = 0; j < polys[q].n; j++) if (polys[q].p[j].y > OP1_Q10(150) || polys[q].p[j].y < OP1_Q10(2) || polys[q].p[j].x < OP1_Q10(2) || polys[q].p[j].x > OP1_Q10(318)) offscreen = 1;
    }
    CHECK((rest - top) / 1024.0 > 5, "a note kicks the wheel off the road (%.1f px)", (rest - top) / 1024.0);
    CHECK(shadow >= 2, "and its shadow stays on the road");
    CHECK(!offscreen, "nothing goes off the screen or below its lower margin, at the largest size, flat, lowrider, bouncing");
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
    host_shift_held = 1; blue_clip = 1; frame("shift held, blue on clip"); int txt3, bad3, cor3[4]; TEXT_SCAN(txt3, bad3, cor3);
    CHECK(!bad3 && txt3 != txt, "Shift with blue switched shows clip, and fits (%d strokes)", txt3);
    blue_clip = 0; host_shift_held = 0; sub(0); sag(0);
    host_knobs[0][0] = 0; frame("shape at -50 %"); TEXT_SCAN(txt3, bad3, cor3);
    CHECK(!bad3, "the shape value fits with its sign");
    host_knobs[0][0] = 8192;
    host_knobs[0][2] = 32767; sag(32767); host_shift_held = 1; glide_rec(32767); frame("shift held, glide 500 ms"); host_shift_held = 0; frame("tone open, sag 100 %"); TEXT_SCAN(txt3, bad3, cor3); CHECK(!bad3, "the longest tone and sag values fit"); host_shift_held = 1; frame("shift held, glide 500 ms"); TEXT_SCAN(txt3, bad3, cor3);
    CHECK(!bad3, "the longest tone and glide values fit their corners");
    host_shift_held = 0;

    /* the T2 page: the envelope curve and four labels */
#define T2_FRAME(label) frame_of(&SCREEN_tyr8_t2, label)
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
    if (fj) { fprintf(fj, "]\n"); fclose(fj); }

    printf("audio hash %08x\n", audio_hash);
    printf(fails ? "\n%d FAILED\n" : "\nall ok\n", fails);
    return fails != 0;
}
