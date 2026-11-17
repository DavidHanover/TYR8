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
int16_t host_voice_env[128]; int16_t host_knobs[8][8]; int host_slot;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- audio driver: six voices as the firmware runs them ---- */
static void sag(int v) { host_eng_rec[0][4] = (int16_t)v; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
static void third(int sub, int tone, int atk) { host_eng_rec[0][1] = (int16_t)sub; host_eng_rec[0][2] = (int16_t)tone; host_eng_rec[0][3] = (int16_t)atk; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
static void mode(int m) { host_eng_rec[0][0] = (int16_t)m; host_eng_rec_owner[0] = op1_engine_rec_owner("tyr8"); }
#define SINE 8192                             /* the shape knob's plain sine: a quarter up */
static int16_t P[8];                         /* the sound's words */
static int active[6]; static double key_hz[6];      /* the key's own frequency, as the firmware gives it */
static int16_t *rec; static size_t rec_n, rec_cap;

static void set(int osc, int amt, int pdec, int adec, int drive, int clip, int sus, int glide)
{ P[0] = osc; P[1] = amt; P[2] = pdec; P[3] = adec; P[4] = 0x5300; P[5] = drive; P[6] = clip; P[7] = (int16_t)(sus | glide << 8); }
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
        for (int n = 0; n < 128; n++) rec[rec_n++] = op1_sat16(sum[n]);
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

/* ---- screen mocks ---- */
#define NODES 20
static const char *node_id[NODES] = { "wheel-rig", "wheel-spin", "tread0", "tread1", "tread2", "tread3", "tread4", "tread5",
    "tread6", "tread7", "tread8", "tread9", "tread10", "tread11", "tread12", "tread13" };
static int32_t node_m[NODES][9]; static int node_set[NODES];
typedef struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } hline;
static hline lines[512]; static int nlines; static int drew;
void *host_find_node(void *scr, const char *id)
{ (void)scr; for (int i = 0; i < NODES && node_id[i]; i++) if (!strcmp(node_id[i], id)) return (void *)(intptr_t)(i + 1); return 0; }
void host_node_set_transform(void *node, const int32_t *m) { int i = (int)(intptr_t)node - 1; memcpy(node_m[i], m, 36); node_set[i] = 1; }
void host_screen_draw(void *scr, int dt) { (void)scr; (void)dt; drew++; }
void *host_raster(void) { return (void *)1; }
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{
    (void)r; const op1_pt *a = p0, *b = p1;
    if (nlines < 512) { hline *l = &lines[nlines++]; l->x0 = a->x; l->y0 = a->y; l->x1 = b->x; l->y1 = b->y; l->w = w; memcpy(l->c, c, 4); }
}
static FILE *fj; static int nframes;
static void frame(const char *label)
{
    nlines = 0;
    SCREEN_tyr8.update(0, 16);
    if (!fj) return;
    fprintf(fj, "%s{\"label\":\"%s\",\"nodes\":{", nframes++ ? ",\n" : "", label);
    for (int i = 0, n = 0; i < NODES && node_id[i]; i++) if (node_set[i]) {
        fprintf(fj, "%s\"%s\":[", n++ ? "," : "", node_id[i]);
        for (int j = 0; j < 9; j++) fprintf(fj, "%s%d", j ? "," : "", node_m[i][j]);
        fprintf(fj, "]");
    }
    fprintf(fj, "},\"lines\":[");
    for (int i = 0; i < nlines; i++)
        fprintf(fj, "%s[%d,%d,%d,%d,%d,%d,%d,%d]", i ? "," : "", lines[i].x0, lines[i].y0, lines[i].x1, lines[i].y1, lines[i].w,
                lines[i].c[0], lines[i].c[1], lines[i].c[2]);
    fprintf(fj, "]}");
}
/* where the matrix of node i puts drawing point (x, y), in px */
static void map(int i, double x, double y, double *ox, double *oy)
{ const int32_t *m = node_m[i]; *ox = (x * m[0] + y * m[3]) / 1024 + m[6] / 1024.0; *oy = (x * m[1] + y * m[4]) / 1024 + m[7] / 1024.0; }

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
    mode(MODE_POLY); reset(); set(SINE, 0, 0, 6000, 0, 0, 127, 100);
    on(0, 55, 127); run_ms(300); int one = peak(at(100), rec_n); on(1, 82.5, 127); run_ms(300);
    CHECK(one > 16000 && one <= 16384, "poly: one note is at half level (peak %d)", one);
    CHECK(peak(at(400), rec_n) > one * 3 / 2 && active[0] && active[1], "poly: two keys sound together (peak %d)", peak(at(400), rec_n));
    offk(0); offk(1); run_ms(1500);
    CHECK(!active[0] && !active[1] && peak(rec_n - at(100), rec_n) == 0, "poly: both voices end");
    wav("poly_fifth");
    mode(MODE_GLIDE); host_slot = 0;
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1) == 1 && host_eng_rec[0][0] == MODE_RETRIG, "orange push: glide -> retrig");
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 0); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1);
    CHECK(host_eng_rec[0][0] == MODE_POLY, "orange push: retrig -> poly");
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 3, 1);
    CHECK(host_eng_rec[0][0] == MODE_GLIDE, "orange push: poly -> glide");
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 3, 1024) == 0 && ENGINE_tyr8.input(OP1_ENG_T2, OP1_ENG_PUSH, 3, 1) == 0 && host_eng_rec[0][0] == MODE_GLIDE, "orange's turn, and other pages, are left to the OP-1");

    /* 12. the third values: attack, tone, sub */
    for (int c = 0; c < 2; c++) {                               /* no drive, then full drive and hard clip */
        mode(MODE_GLIDE); third(0, 0, ALT_MAX); reset(); set(SINE, 0, 0, 20000, c ? 32767 : 0, c ? 32767 : 0, 127, 0); on(0, 55, 127); run_ms(300);
        CHECK(peak(0, at(5)) < 2000 && peak(at(40), at(60)) > 11000 && peak(at(40), at(60)) < 21000 && peak(at(150), rec_n) > 32000,
              "attack 100 ms, %s: a slow fade-in (peaks %d at 5 ms, %d at 50 ms, %d after)", c ? "full drive" : "no drive", peak(0, at(5)), peak(at(40), at(60)), peak(at(150), rec_n));
        if (c) wav("attack_100ms");
    }
    third(0, 0, ALT_MAX); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 0, 0); on(0, 55, 127); run_ms(104); offk(0); run_ms(10); size_t ta = rec_n; on(1, 55, 127); run_ms(100);
    int stp = 0; for (size_t i = ta - 50; i < ta + 400; i++) if (abs(rec[i] - rec[i - 1]) > stp) stp = abs(rec[i] - rec[i - 1]);
    CHECK(stp < 1500, "attack: a restart over a sounding note does not click (largest sample step %d)", stp);
    double hfm[3], rmsm[3];
    for (int c = 0; c < 3; c++) {                               /* full drive, hard: plain, tone closed, sub full */
        third(c == 2 ? ALT_MAX : 0, c == 1 ? ALT_MAX : 0, 0); reset(); set(SINE, 0, 0, 6000, 32767, 32767, 127, 0); on(0, 55, 127); run_ms(600);
        double e = 0, de = 0;
        for (size_t i = at(300); i < rec_n; i++) { e += (double)rec[i] * rec[i]; de += (double)(rec[i] - rec[i - 1]) * (rec[i] - rec[i - 1]); }
        hfm[c] = de / e; rmsm[c] = sqrt(e / (rec_n - at(300)));
        CHECK(peak(0, rec_n) <= 32768, "%s: inside full scale (peak %d)", c == 0 ? "plain" : c == 1 ? "tone closed" : "sub full", peak(0, rec_n));
        if (c) wav(c == 1 ? "tone_closed" : "sub_full");
    }
    CHECK(hfm[1] < hfm[0] / 20, "tone closed: 1/%.0f of the treble", hfm[0] / hfm[1]);
    CHECK(hfm[2] < hfm[0] * 0.2 && rmsm[2] > 16000, "sub full: mostly the clean sine, still loud (treble 1/%.0f, rms %.0f)", hfm[0] / hfm[2], rmsm[2]);
    third(0, 0, 0);
    host_slot = 0; host_shift_held = 0; memset((void *)alt, 0, sizeof alt);
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, 1024) == 0, "an unpushed encoder's turn is the OP-1's");
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 0, 1) == 1 && alt[0] && ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, 3 * 1024) == 1 && host_eng_rec[0][REC_SUB] == 3,
          "blue push, then three detents: sub is %d", host_eng_rec[0][REC_SUB]);
    host_shift_held = 1;
    CHECK(ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, 1024) == 0 && host_eng_rec[0][REC_SUB] == 3, "with Shift the turn still goes to drive");
    host_shift_held = 0;
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 0, -100 * 1024);
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 1, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 1, -2 * 1024);
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 2, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 2, 200 * 1024);
    CHECK(host_eng_rec[0][REC_SUB] == 0 && host_eng_rec[0][REC_TONE] == 2 && host_eng_rec[0][REC_ATTACK] == ALT_MAX && host_eng_rec[0][REC_MODE] == MODE_GLIDE,
          "values stop at their ends; turning tone down closes it (sub %d, tone %d, attack %d)", host_eng_rec[0][REC_SUB], host_eng_rec[0][REC_TONE], host_eng_rec[0][REC_ATTACK]);
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 0, 1);
    CHECK(!alt[0] && alt[1] && alt[2], "a second push switches the knob back");
    third(0, 0, 0); memset((void *)alt, 0, sizeof alt);

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
    third(0, ALT_MAX, 0); reset(); set(SINE, 0, 0, 6000, 32767, 32767, 127, 0); on(0, 55, 127); run_ms(500);
    CHECK(peak(at(200), rec_n) > 32000, "tone closed at full drive: a dark wall, still full scale (peak %d)", peak(at(200), rec_n));
    third(0, 0, 0);

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

    /* 17. sag: the pitch falls while the note lasts; white's second push */
    sag(ALT_MAX); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(60); offk(0); run_ms(1200);
    double fs0 = freq(0, at(60)), fs1 = freq(at(500), at(700));
    CHECK(fs0 < 55 && fs0 > 50 && fs1 < fs0 * 0.75 && fs1 > 12, "sag at maximum: %.1f Hz at the start, %.1f Hz half a second on", fs0, fs1);
    wav("sag");
    sag(ALT_MAX / 2); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(60); offk(0); run_ms(1200);
    CHECK(freq(at(500), at(700)) > fs1 * 1.1 && freq(at(500), at(700)) < 54, "sag at half falls less (%.1f Hz)", freq(at(500), at(700)));
    sag(0); reset(); set(SINE, 0, 0, 20000, 14000, 12000, 127, 0); on(0, 55, 127); run_ms(60); offk(0); run_ms(1200);
    CHECK(fabs(freq(at(500), at(700)) - 55) < 0.3, "sag at 0: none (%.2f Hz)", freq(at(500), at(700)));
    memset((void *)alt, 0, sizeof alt); host_shift_held = 0;
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 2, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 2, 0);
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 2, 1); ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_TURN, 2, 5 * 1024);
    CHECK(alt[2] == 2 && host_eng_rec[0][REC_SAG] == 5 && host_eng_rec[0][REC_ATTACK] == 0, "white pushed twice turns sag (%d), not attack", host_eng_rec[0][REC_SAG]);
    ENGINE_tyr8.input(OP1_ENG_T1, OP1_ENG_PUSH, 2, 1);
    CHECK(alt[2] == 0, "a third push of white is back to knock length");
    sag(0); third(0, 0, 0);

    /* the sound this was for: full feedback saw, a long deep knock, sag, drive up, the tone half closed */
    sag(40); third(0, 28, 0); reset(); set(31500, 22000, 20000, 26000, 28000, 26000, 0, 0);
    on(0, 43.65, 127); run_ms(150); offk(0); run_ms(1400); on(1, 38.89, 127); run_ms(150); offk(1); run_ms(5000);
    CHECK(peak(0, rec_n) <= 32768 && !active[0] && !active[1] && peak(rec_n - at(100), rec_n) == 0, "the doom patch stays in range and ends");
    wav("doom");
    sag(0); third(0, 0, 0);

    /* 18. the tail stays a clean tone as it dies away: 882 Hz / 16 = 55.125 Hz is exactly 1600 samples a cycle */
    for (int c = 0; c < 3; c++) {
        const int drv[3] = { 0, 14000, 32767 };
        reset(); set(SINE, 0, 0, 16000, drv[c], 12000, 0, 0); on(0, 55.125, 127); run_ms(40); offk(0); run_ms(2500);
        /* by 0.9 s the note has fallen 61 dB; at full drive it is still at -27 dBFS, with all that gain on its rounding */
        const double q7 = tail_snr(at(700), at(800), 55.125), q9 = tail_snr(at(900), at(1000), 55.125);
        CHECK(q7 > 42 && q9 > (c ? 36 : 28), "drive %d %%: the tail is a clean tone, %.0f dB over its noise at 0.7 s (peak %d), %.0f dB at 0.9 s (peak %d)",
              drv[c] * 100 / 32767, q7, peak(at(700), at(800)), q9, peak(at(900), at(1000)));
        if (c == 2) wav("tail_full_drive");
    }

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

    /* 20. the sub is a sine at the note, whatever the shape: full sub, no drive, soft clip, tone closed to leave the sub */
    for (int c = 0; c < 4; c++) {
        const int shp[4] = { 8192, 24576, 0, 32767 };
        third(ALT_MAX, ALT_MAX, 0); reset(); set(shp[c], 0, 0, 6000, 0, 0, 127, 0); on(0, 55, 127); run_ms(700);
        CHECK(sine_part(at(300), at(700), 55) > 0.985, "sub with shape %d %%: %.1f %% of the output is a 55 Hz sine", shp[c] * 200 / 32767 - 50, sine_part(at(300), at(700), 55) * 100);
    }
    third(0, 0, 0);

    /* a phrase to listen to */
    reset(); set(10000, 14000, 6000, 20000, 14000, 12000, 0, 60);
    const double ph_hz[8] = { 43.65, 43.65, 51.91, 43.65, 65.41, 58.27, 43.65, 38.89 };
    for (int i = 0; i < 8; i++) { on(i % 6, ph_hz[i], i & 1 ? 100 : 127); run_ms(i == 4 ? 180 : 120); if (i != 4) offk(i % 6); run_ms(i == 4 ? 0 : 260); if (i == 5) offk(4); }
    run_ms(2500);
    wav("phrase");

    /* ---- screen ---- */
    if (outdir) { char path[512]; snprintf(path, sizeof path, "%s/frames.json", outdir); fj = fopen(path, "w"); if (fj) fprintf(fj, "["); }
    SCREEN_tyr8.init(0);
    CHECK(n_rig && n_spin && n_tread[13], "the screen finds its SVG nodes");
    reset(); vis_lvl = 0; vis_inc = 0;
    memset(host_knobs, 0, sizeof host_knobs);
    host_knobs[0][3] = 0; host_knobs[0][6] = 0;
    for (int i = 0; i < 60; i++) frame("small slick, at rest");
    double x, y, x2, y2;
    map(0, TY_HUB_X, TY_HUB_Y + TY_R_TYRE, &x, &y);
    CHECK(fabs(y - (TY_GROUND_Y - 0.75)) < 0.6, "at rest the tyre stands on the ground (bottom y %.2f)", y);
    map(0, TY_HUB_X, TY_HUB_Y - TY_R_TYRE, &x2, &y2);
    CHECK(fabs((y - y2) - 2 * 0.43 * TY_R_TYRE) < 1, "amp decay 0: a small tyre (diameter %.1f px)", y - y2);
    host_knobs[0][3] = 32767; host_knobs[0][6] = 32767; host_knobs[0][1] = 20000;
    for (int i = 0; i < 60; i++) frame("monster truck, off-road lugs, at rest");
    map(0, TY_HUB_X, TY_HUB_Y - TY_R_TYRE, &x2, &y2); map(0, TY_HUB_X, TY_HUB_Y + TY_R_TYRE, &x, &y);
    CHECK(fabs((y - y2) - 2 * 0.94 * TY_R_TYRE) < 1, "amp decay max: full size (diameter %.1f px)", y - y2);
    /* tread 0 sits at angle 0 (+x); with the wheel unturned its outer edge must be R + H out, on the hub's height */
    double ang0 = (ang >> 8) * 2 * M_PI / 8192, hubx, huby;
    map(0, TY_HUB_X, TY_HUB_Y, &hubx, &huby);
    map(2, TY_HUB_X + TY_R_TYRE + TY_TREAD_H, TY_HUB_Y, &x, &y);
#define FACE_R (hypot((x - hubx) * 1024.0 / TY_KX, y - huby) / 0.94)   /* radius in the wheel's face, full-size px */
    CHECK(fabs(FACE_R - (TY_R_TYRE + TY_TREAD_H)) < 0.8 && fabs(atan2(y - huby, (x - hubx) * 1024.0 / TY_KX) - ang0) < 0.02,
          "full lugs stand %d px out of the tyre (r %.1f)", TY_TREAD_H, FACE_R);
    map(0, TY_HUB_X + TY_R_TYRE, TY_HUB_Y, &x2, &y2);
    CHECK(fabs((x2 - hubx) - 0.94 * TY_R_TYRE * TY_KX / 1024.0) < 0.6, "three-quarter view: the face is %.0f %% as wide as it is high", (x2 - hubx) / (0.94 * TY_R_TYRE) * 100);
    int far = 0; for (int j = 0; j < nlines; j++) if (lines[j].w != 1024 && lines[j].y0 < OP1_Q10(TY_GROUND_Y - 6) && lines[j].x0 > OP1_Q10(96) && lines[j].x0 < OP1_Q10(224)) far++;
    CHECK(far >= 20 && far <= 45, "the far sides of the lugs are drawn on the side that shows (%d lines)", far);
    double lowest = 0; for (int i = 0; i < TY_TREAD_N; i++) { double a = 2 * M_PI * i / TY_TREAD_N; map(2 + i, TY_HUB_X + (TY_R_TYRE + TY_TREAD_H) * cos(a), TY_HUB_Y + (TY_R_TYRE + TY_TREAD_H) * sin(a), &x, &y); if (y > lowest) lowest = y; }
    CHECK(lowest < TY_GROUND_Y && lowest > TY_GROUND_Y - 4, "the lugs reach the ground, not through it (lowest %.2f)", lowest);
    host_knobs[0][6] = 0; frame("slick");
    map(2, TY_HUB_X + TY_R_TYRE + TY_TREAD_H, TY_HUB_Y, &x, &y); map(0, TY_HUB_X, TY_HUB_Y, &hubx, &huby);
    CHECK(fabs(FACE_R - TY_R_TYRE) < 0.8, "clip shape 0: the lugs lie flat on the tyre (r %.1f)", FACE_R);
    host_knobs[0][6] = 32767;
    /* a note: bounce and spin */
    vis_trig++; vis_lvl = 32767; vis_inc = (uint32_t)(55 / 88200.0 * 4294967296.0);
    map(0, TY_HUB_X, TY_HUB_Y + TY_R_TYRE, &x, &y); const double rest = y;
    double top = 1e9; uint32_t a0 = ang; int under = 0, offscreen = 0;
    for (int i = 0; i < 90; i++) {
        frame(i == 6 ? "note on: bounce" : i == 30 ? "rolling" : "rolling on");
        map(0, TY_HUB_X, TY_HUB_Y + TY_R_TYRE, &x, &y); if (y < top) top = y; if (y > TY_GROUND_Y) under = 1;
        map(0, TY_HUB_X, TY_HUB_Y - TY_R_TYRE - TY_TREAD_H, &x, &y); if (y + 0.94 * TY_WY < 2) offscreen = 1;
        for (int j = 0; j < nlines; j++) if (lines[j].y1 > OP1_Q10(148) || lines[j].y0 > OP1_Q10(148) || lines[j].x0 < 0 || lines[j].x1 > OP1_Q10(320) || lines[j].y0 < OP1_Q10(2) || lines[j].y1 < OP1_Q10(2)) offscreen = 1;
    }
    CHECK(rest - top > 5, "a note kicks the wheel off the ground (%.1f px)", rest - top);
    CHECK(!under && !offscreen, "nothing goes through the ground or off the screen");
    CHECK(ang != a0, "the wheel turns while a note sounds");
    vis_lvl = 0; for (int i = 0; i < 400; i++) frame("coasting");
    a0 = ang; frame("stopped");
    CHECK(ang == a0, "and stops after it");
    host_shift_held = 1; host_knobs[0][5] = 16384; host_knobs[0][7] = 64 | 127 << 8; frame("shift held: gauges show drive, clip, sustain, glide");
    /* text strokes are the 1 px lines; everything else the screen draws is 1.5 px */
#define TEXT_SCAN(cnt, bad, cor) do { cnt = bad = 0; memset(cor, 0, sizeof cor); for (int j = 0; j < nlines; j++) if (lines[j].w == 1024) { \
        const int32_t xa = lines[j].x0 < lines[j].x1 ? lines[j].x0 : lines[j].x1, xb = lines[j].x0 > lines[j].x1 ? lines[j].x0 : lines[j].x1; \
        cnt++; if ((xb > OP1_Q10(95) && xa < OP1_Q10(225)) || xa < OP1_Q10(6) || xb > OP1_Q10(314)) bad++; \
        cor[(lines[j].y0 > OP1_Q10(80)) * 2 + (xa > OP1_Q10(160))]++; } } while (0)
#define TEXT_HASH(h) do { h = 0; for (int j = 0; j < nlines; j++) if (lines[j].w == 1024) h = h * 31 + (unsigned)(lines[j].x0 + 7 * lines[j].y0 + 13 * lines[j].x1 + 17 * lines[j].y1); } while (0)
    int txt, txt2, bad, bad2, cor[4], cor2[4]; unsigned h1, h2;
    TEXT_SCAN(txt, bad, cor); TEXT_HASH(h1);
    host_shift_held = 0; frame("labels");
    TEXT_SCAN(txt2, bad2, cor2); TEXT_HASH(h2);
    CHECK(txt > 60 && txt2 > 60 && h1 != h2, "labels are drawn, and Shift shows the second set (%d and %d strokes)", txt2, txt);
    CHECK(cor2[0] > 8 && cor2[1] > 8 && cor2[2] > 8 && cor2[3] > 8, "one label block in each corner (%d, %d, %d, %d strokes)", cor2[0], cor2[1], cor2[2], cor2[3]);
    CHECK(!bad && !bad2, "labels stay in their corners, clear of the wheel and the screen edge");
    mode(MODE_RETRIG); frame("mode: mono retrig"); TEXT_SCAN(txt2, bad2, cor2); mode(MODE_GLIDE);
    CHECK(!bad2, "the longest mode name fits");
    alt[0] = alt[1] = alt[2] = 1; third(32, 16, ALT_MAX); frame("pushed: sub, tone, attack"); int txt3, bad3, cor3[4]; TEXT_SCAN(txt3, bad3, cor3);
    CHECK(!bad3 && txt3 != txt2, "pushed encoders show their third value, and the longest fits");
    alt[2] = 2; sag(32); frame("pushed twice: sag"); memset((void *)alt, 0, sizeof alt); third(0, 0, 0); sag(0);
    host_knobs[0][0] = 0; frame("shape at -50 %"); TEXT_SCAN(txt3, bad3, cor3);
    CHECK(!bad3, "the shape value fits with its sign");
    host_knobs[0][0] = 8192;
    host_knobs[0][2] = 32767; frame("knock length in seconds"); TEXT_SCAN(txt3, bad3, cor3);
    CHECK(!bad3, "the longest knock length fits its corner");
    host_shift_held = 0;
    if (fj) { fprintf(fj, "]\n"); fclose(fj); }

    printf(fails ? "\n%d FAILED\n" : "\nall ok\n", fails);
    return fails != 0;
}
