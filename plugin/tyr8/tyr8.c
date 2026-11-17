/* TYR8: a monophonic 808 bass for the OP-1 (hundekraft kennel plugin, tyr8.knl).
 *
 *   turn                                   Shift + turn
 *   blue   shape, see render              drive: full .. fat and gritty, up to 34 dB  (word 5)
 *   green  pitch envelope amount (knock)   clipper shape, soft cubic .. hard         (word 6)
 *   white  knock length, 20 ms .. 1.5 s   amp sustain, 0 .. 100 %                   (word 7 bits 0..6)
 *   orange amp decay, 0.1 .. 4 s           glide time, 0 .. 500 ms                   (word 7 bits 8..14)
 *
 * The engine sounds two octaves below the key played (OCT_DOWN), so the OP-1's octave 0 is already bass.
 * A push of blue, green or white switches that knob to a third value, and back: sub (a clean sine under the drive),
 * tone (a low-pass after the drive), attack (a fade-in, 0 .. 100 ms) and, a second push of white, sag (the pitch
 * falls while the note lasts). Saved with the sound in the engine's record.
 * Signal: oscillator (with feedback from the output) -> amp envelope -> drive (two saturating stages, see render) ->
 * tone -> sub blended in -> soft / hard clipper, always last. The
 * oscillator's pitch is the note plus the pitch envelope (and the glide). A restarted note starts the oscillator at
 * phase 0, so every hit is the same hit. The engine shapes its own level (own_envelope): the OP-1's T2 envelope does
 * not run for its sounds. With sustain at 0 a note is a one-shot whatever the key does; with sustain up the level
 * holds while the key is down and falls at the amp decay's rate after it goes up.
 *
 * Play mode, stepped by pushing orange and saved with the sound:
 *   MONO GLIDE   one voice, the newest key. A note played while another is held and still sounding is legato: no
 *                restart, the pitch glides; the key still held underneath takes over again when the newest goes up
 *   MONO RETRIG  one voice, every note restarts on its own pitch, no glide; letting the newest key go restarts the
 *                key still held underneath
 *   POLY         the OP-1's six voices, each a whole instrument at half level; no glide
 *
 * The code runs in the audio interrupt at 88.2 kHz, 128 samples a call: integers only, no division and no 64-bit
 * arithmetic per sample (both once a block). The sound state is static (tv), not in the OP-1's voice objects.
 *
 * Threads: note_on / note_off run on the key thread and only write the small volatile fields under "keys"; render
 * (audio) owns everything else; the screen (GUI) reads the three vis_ fields.
 * Copyright (C) 2026 David Hanover. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#include "tyr8_geom.h"
#include "seq/seq_text.h"                     /* sq_text: the core's stroke font (uppercase, digits, . % : + - / #) */

#define ONE30       (1 << 30)                 /* envelope full scale */
#define SILENT      (ONE30 >> 17)             /* -102 dB: the note is over (the drive can lift a tail 30 dB) */
#define Q18_ONE     (1 << 18)                 /* the signal path's 1.0 */
#define Q18_LIM     (3 << 17)                 /* and its 1.5, where the saturations go flat */
#define AUDIBLE     (ONE30 >> 7)              /* -42 dB: below this a held note no longer makes the next one legato */
#define OCT_DOWN    2                         /* the engine plays this many octaves below the key: bass at octave 0 */
#define OUT_GAIN    32767                     /* full scale: one voice, so nothing adds on top of it */

/* ---- keys (key thread writes, audio reads) ---- */
static volatile uint8_t  held[OP1_VOICES];    /* the voice's key is down */
static volatile uint8_t  legato_req[OP1_VOICES]; /* its note-on came while another key was down */
static volatile uint32_t stamp[OP1_VOICES];   /* note-on order: the newest held voice plays */
static volatile uint32_t beat[OP1_VOICES];    /* render call count when the voice last rendered: is it still alive */
static volatile uint32_t stamp_next = 1, calls;
static volatile int8_t   owner = -1;          /* the voice that sounds */

/* ---- the engine's record, saved with the sound: values the eight words have no room for. 0 is each one's default ----
 *   [0] play mode (the orange push steps it)
 *   [1] sub: clean sine under the drive, 0 .. ALT_MAX          the third value of blue, green and white:
 *   [2] tone: how far the low-pass after the drive is closed    a push of the encoder switches its knob to it and
 *   [3] attack, 0 .. ALT_MAX = 0 .. 100 ms                      back (alt[], not saved); white has two, attack
 *   [4] sag: how far the pitch falls while the note lasts       and sag, a push each */
enum { MODE_GLIDE, MODE_RETRIG, MODE_POLY, MODE_N };
enum { REC_MODE, REC_SUB, REC_TONE, REC_ATTACK, REC_SAG, REC_N };
#define ALT_MAX 64                            /* steps of a third value, one a detent */
#define TY_NAME "tyr8"
static uint32_t rec_key;                      /* the record's owner key, worked out once */
static volatile uint8_t alt[3];               /* blue, green, white: 0 the knob's own value, 1 its third (white 2: sag) */
static void rec_get(int slot, int16_t out[REC_N])
{
    if (!rec_key) rec_key = op1_engine_rec_owner(TY_NAME);
    const int16_t *r = op1_engine_record(slot, rec_key);
    out[REC_MODE] = r && r[0] > 0 && r[0] < MODE_N ? r[0] : MODE_GLIDE;
    for (int i = 1; i < REC_N; i++) out[i] = !r || r[i] < 0 ? 0 : (r[i] > ALT_MAX ? ALT_MAX : r[i]);
}
static int mode_of(int slot) { int16_t r[REC_N]; rec_get(slot, r); return r[REC_MODE]; }

/* ---- a voice's sound (audio thread). The mono modes use tv[0] whichever key plays; poly one per OP-1 voice ---- */
typedef struct {
    uint32_t seen_stamp;                      /* the note-on stamp render has started */
    uint32_t ph;                              /* oscillator phase, 2^32 = one cycle */
    int32_t  lvl;                             /* amp envelope, 0 .. ONE30 */
    int32_t  pe;                              /* pitch envelope, 0 .. ONE30 */
    int32_t  off;                             /* glide: phase increment still to slide, old note minus new */
    int32_t  last_inc;                        /* phase increment at the end of the last block, glide included */
    int32_t  last_y;                          /* last sample before the velocity gain, Q18 */
    int32_t  tail;                            /* declick: the step a restart would make, << 8, fading to 0 */
    int32_t  vel;                             /* the note's velocity gain, Q15 */
    int32_t  d_s;                             /* smoothed drive knob, Q15 << 8 */
    int32_t  hp;                              /* the fattener's low-pass state between its stages, Q18 << 8 */
    int32_t  dcs;                             /* and its DC tracker after stage 1, Q18 << 8 */
    int32_t  atk;                             /* attack ramp, 0 .. ONE30 */
    int32_t  z1, z2;                          /* tone: the two low-pass states, Q18 */
    int32_t  f1, f2;                          /* feedback: the last two output samples */
    int32_t  sag;                             /* pitch sag: the note's frequency factor, ONE30 falling */
    uint8_t  restart;                         /* this block starts a note: 1 from silence, 2 over a sounding one */
    uint8_t  live;                            /* the envelope has not run out (0 at boot: nothing sounds) */
} voice_t;
static voice_t tv[OP1_VOICES];

/* ---- for the screen ---- */
static volatile uint32_t vis_inc;             /* sounding phase increment */
static volatile int32_t  vis_lvl;             /* amp envelope, Q15 */
static volatile uint32_t vis_trig;            /* counts restarted notes */

/* 2^-(x / 4096) in Q16, x >= 0: the knob laws (once a block) */
static uint32_t exp2neg(uint32_t x)
{
    static const uint16_t t[17] = { 65535, 62757, 60097, 57549, 55109, 52773, 50535, 48393, 46341, 44376, 42495,
                                    40693, 38968, 37316, 35734, 34219, 32768 };
    const uint32_t sh = x >> 12, i = (x >> 8) & 15, fr = x & 255;
    if (sh > 30) return 0;
    return (t[i] - (((uint32_t)(t[i] - t[i + 1]) * fr) >> 8)) >> sh;
}

static inline int32_t q15(int32_t x) { return x < 0 ? 0 : x; }      /* a knob word, whatever the sound holds */
/* the soft clip's cubic part, 4/27 x^3, for a Q18 x within +-1.5: x - cube18(x) is the soft clip. Worked out at 15
 * bits: it is tiny when x is, so x itself keeps all of its 18 */
static inline int32_t cube18(int32_t x)
{
    const int32_t xs = x >> 4;                                 /* Q14 */
    const int32_t x3 = (((xs * xs) >> 14) * xs) >> 14;
    return ((x3 * 4855) >> 15) * 16;
}
/* Where stage 1's output will average once a note at envelope level g (Q30) is running: one cycle of a sine through
 * the bias bk, the gain and the saturation. A restart sets the AC coupling's tracker there, so the note starts as it
 * will go on. Left at the resting level, the tracker would cancel the whole first half-wave of a heavily driven note
 * (the bias holds that half against the ceiling), and the hit would come in late. Once a note: 32 points. */
static int32_t warm(int32_t bk, int32_t g1, int32_t mix, int32_t g)      /* g Q30, g1 Q8, mix Q12 -> Q18 */
{
    const int16_t *tab = op1_sin_tab();
    const int32_t gh = g >> 15, b = (bk * gh) >> 12;
    int32_t sum = 0;
    for (int k = 0; k < 32; k++) {
        int32_t x = ((((op1_sin_q(tab, k * 256) * gh) >> 12) + b) * g1) >> 8;
        x = x > Q18_LIM ? Q18_LIM : (x < -Q18_LIM ? -Q18_LIM : x);
        sum += (x - ((cube18(x) * mix) >> 12)) >> 5;
    }
    return sum;
}
static inline int alive(int v) { return calls - beat[v] < 64; }

static void ty_note_on(void *self, const int16_t *params)
{
    const int v = op1_voice_index(self); (void)params;
    if (v < 0 || v >= OP1_VOICES) return;
    int other = 0;
    for (int i = 0; i < OP1_VOICES; i++) if (i != v && held[i] && alive(i)) other = 1;
    legato_req[v] = (uint8_t)other;
    held[v] = 1;
    beat[v] = calls;
    stamp[v] = stamp_next++;
    owner = (int8_t)v;
}

static void ty_note_off(void *self)
{
    const int v = op1_voice_index(self);
    if (v < 0 || v >= OP1_VOICES) return;
    held[v] = 0;
    if (owner != v || mode_of(op1_current_slot()) == MODE_POLY) return;
    int best = -1;                              /* mono: the newest key still down takes over (glide mode: legato) */
    for (int i = 0; i < OP1_VOICES; i++)
        if (i != v && held[i] && alive(i) && (best < 0 || (int32_t)(stamp[i] - stamp[best]) > 0)) best = i;
    if (best >= 0) {
        legato_req[best] = 1;
        stamp[best] = stamp_next++;
        owner = (int8_t)best;
    }
}

/* asked once a block before render: a voice that is not sounding stays only while its key is down */
static int ty_finished(void *self)
{
    const int v = op1_voice_index(self);
    if (v < 0 || v >= OP1_VOICES) return 1;
    if (mode_of(op1_current_slot()) == MODE_POLY) return stamp[v] == tv[v].seen_stamp && !tv[v].live;
    if (v != owner) return !held[v];
    return stamp[v] == tv[0].seen_stamp && !tv[0].live;
}

static void ty_render(void *self, int16_t *out, const int16_t *params)
{
    const int v = op1_voice_index(self);
    calls++;
    int16_t rc[REC_N];
    rec_get(op1_current_slot(), rc);
    const int mode = rc[REC_MODE];
    if (v < 0 || v >= OP1_VOICES || (mode != MODE_POLY && v != owner)) {   /* mono: a key under the sounding one is silent */
        if (v >= 0 && v < OP1_VOICES) beat[v] = calls;
        for (int n = 0; n < OP1_BLOCK; n++) out[n] = 0;
        return;
    }
    beat[v] = calls;
    voice_t *const t = mode == MODE_POLY ? &tv[v] : &tv[0];
    const int32_t *inc = OP1_PITCH_INC;
    const int32_t sus = (params[7] & 127) * (ONE30 / 127);
    const int32_t glide = mode == MODE_GLIDE ? (params[7] >> 8) & 127 : 0;   /* only glide mode glides */
    const int gate = held[v];

    /* ---- once a block: knob laws ---- */
    /* shape, four quarters of the knob (shown as -50 .. 150 %, so that the plain waves are 0, 50 and 100): sine with feedback (most at the bottom) .. sine | .. triangle | .. saw |
     * saw with feedback (most at the top). Feedback: the finished output bends the oscillator's phase, by up to half a
     * cycle; a little thickens the wave, a lot breaks it up into something with no pitch to speak of */
    const int32_t shape = q15(params[0]);
    const int32_t sh_tri = shape < 8192 ? 0 : (shape < 16384 ? (shape - 8192) * 4 : 32767);
    const int32_t sh_saw = shape < 16384 ? 0 : (shape < 24576 ? (shape - 16384) * 4 : 32767);
    int32_t fb = shape < 8192 ? (8192 - shape) * 4 : (shape > 24576 ? (shape - 24576) * 4 : 0);
    fb = fb > 32767 ? 32767 : fb;
    fb = (fb * fb) >> 15;                                      /* squared: fine control of the tame end */
    int32_t amt = (q15(params[1]) * q15(params[1])) >> 15;     /* squared: fine control of small knocks */
    amt <<= 2;                                                 /* Q13: up to 16x the note's frequency on top */
    const uint32_t kp = 19017u * exp2neg((uint32_t)(q15(params[2]) * 25514) >> 15) >> 16;   /* 20 ms .. 1.5 s */
    const uint32_t ka = 4755u * exp2neg((uint32_t)(q15(params[3]) * 21799) >> 15) >> 16;    /* 0.1 .. 4 s   */
    const int32_t dk = q15(params[5]);
    /* clip: soft .. hard. It sets the knee of stage 2 and of the last clipper, and how hard the last one is driven:
     * 1.76x at soft (a plain sine just reaches the ceiling, rounded) up to 4x at hard (the same sine nearly square) */
    const int32_t clip = (q15(params[6]) + 7) >> 3;            /* Q12 */
    const int32_t cg = 1802 + ((2294 * clip) >> 12);           /* Q10 */

    /* a new note for this voice */
    if (stamp[v] != t->seen_stamp) {
        t->seen_stamp = stamp[v];
        const int32_t slide = glide && t->last_inc ? t->last_inc - (inc[0] >> OCT_DOWN) : 0;   /* from the last pitch */
        if (mode == MODE_GLIDE && legato_req[v] && t->live && t->lvl > AUDIBLE) {
            t->off = slide;                                    /* legato: no restart, the pitch slides */
        } else {                                               /* restart: phase 0, both envelopes from the top */
            t->restart = t->live ? 2 : 1;
            if (!t->live) { t->d_s = dk << 8; t->hp = 0; t->last_y = 0; t->z1 = t->z2 = 0; t->f1 = t->f2 = 0; }   /* from silence: every hit the same */
            t->sag = ONE30;
            t->atk = rc[REC_ATTACK] ? 0 : ONE30;               /* attack: the sound fades in from nothing */
            t->ph = 0;
            t->lvl = ONE30;
            t->pe = ONE30;
            t->off = 0;
            t->vel = op1_voice_vel_gain(self);
            if (v == owner) vis_trig++;
        }
        t->live = 1;
    }

    /* ---- once a block: envelope steps ---- */
    const int32_t m0 = (int32_t)(((int64_t)amt * (t->pe >> 15)) >> 15) << 8;      /* pitch mod now, Q13 << 8 */
    t->pe -= (int32_t)(((int64_t)t->pe * kp) >> 16);
    const int32_t m1 = (int32_t)(((int64_t)amt * (t->pe >> 15)) >> 15) << 8;      /* and at the end of the block */
    const int32_t dm = (m1 - m0) >> 7;

    const int32_t target = gate ? sus : 0;
    const int32_t g0 = t->lvl;                                 /* Q30: the per-sample ramp needs every bit, or a quiet
                                                                * tail moves in steps, one a block, and buzzes at the block rate */
    t->lvl += (int32_t)(((int64_t)(target - t->lvl) * ka) >> 16);
    if (target == 0 && t->lvl < SILENT) t->lvl = 0;
    /* attack: a linear fade-in of the finished sound, 0 .. 100 ms. After the clipper, not before the drive, where the
     * gain would turn any ramp into a few milliseconds */
    const int32_t a0 = t->atk >> 8;                            /* Q22 */
    if (t->atk < ONE30) {                                      /* one block is 1.451 ms */
        const uint32_t st = 997110000u / (uint32_t)(rc[REC_ATTACK] ? rc[REC_ATTACK] : 1);   /* ONE30 * 1.451 * 64 / 100 / steps */
        t->atk = st >= (uint32_t)(ONE30 - t->atk) ? ONE30 : t->atk + (int32_t)st;
    }
    const int32_t da = ((t->atk >> 8) - a0) >> 7;
    const int32_t dg = (t->lvl - g0) >> 7;

    /* sag: the pitch falls steadily while the note lasts, by up to two octaves over the amp decay's length (it stops
     * three octaves down). The decay runs at ka a block with a time constant a fifth of its length, so over that length
     * a factor (1 - ks) a block comes to 2^-(2 amount) with ks = 2 ln 2 / 5 * amount * ka. */
    const uint32_t dq = (uint32_t)(ONE30 - t->sag) >> 14;      /* how far it has fallen, Q16 */
    if (rc[REC_SAG]) {
        const uint32_t ks = (ka * (uint32_t)rc[REC_SAG] * 18170u / ALT_MAX) >> 12;   /* Q20 */
        t->sag -= (int32_t)(((int64_t)t->sag * ks) >> 20);
        if (t->sag < ONE30 >> 3) t->sag = ONE30 >> 3;
    }

    if (glide) {                                               /* 0 .. 500 ms: the slide's time constant is a quarter */
        const uint32_t kg = 96319u / (uint32_t)glide;          /* 65536 * 1.451 ms * 4 / (glide * 500 ms / 127) */
        t->off -= (int32_t)(((int64_t)t->off * (kg > 65536u ? 65536u : kg)) >> 16);
    } else t->off = 0;

    /* drive, a "fattener": two gain stages, AC-coupled, with a treble lift between them. All of it grows with the knob:
     *   bias     0 .. 0.3 of the envelope's level added before stage 1: the wave is clipped off-centre, and after the
     *            AC coupling it is lopsided, which no later clipper can undo: even harmonics, growl. It follows the
     *            envelope so that the tail keeps its shape instead of sinking under the bias
     *   stage 1  gain 1 .. 8x, soft saturation (blended in over the first quarter of the knob)
     *   coupling the level it averages taken off again (a 7 Hz high-pass, faded in over the first eighth of the knob)
     *   lift     the part above ~220 Hz again, 0 .. 3x: stage 2 then distorts stage 1's harmonics too, grit
     *   stage 2  gain 1 .. 4x into a soft saturation that holds it inside +-1                (up to 32x = 30 dB)
     * Then tone and sub, and last of all, always, the clipper. */
    t->d_s += ((dk << 8) - t->d_s) >> 2;                             /* a turned knob ramps over a few blocks */
    const int32_t d = t->d_s >> 8;
    const int32_t g1 = (int32_t)(exp2neg(12288u - ((uint32_t)(d * 12288) >> 15)) >> 5);   /* Q8, 1 .. 8x */
    const int32_t g2 = (int32_t)(exp2neg(8192u - ((uint32_t)(d * 8192) >> 15)) >> 6);     /* Q8, 1 .. 4x */
    const int32_t bk = (((d * d) >> 15) * 9830) >> 15;
    const int32_t mix = d > 8191 ? 4096 : d >> 1;              /* Q12 */
    const int32_t lift = (d * 768) >> 15;                      /* Q8, 0 .. 3x */
    const int32_t cpl = d > 4095 ? 4096 : d;                   /* Q12; no coupling at no drive: the clean sub stays whole */
    if (t->restart) t->dcs = warm(bk, g1, mix, g0) * 256;      /* the AC coupling starts where it will settle (Q18 << 8) */
    const int32_t og = (t->vel * (mode == MODE_POLY ? OUT_GAIN / 2 : OUT_GAIN)) >> 15;   /* poly: two notes fill the bus */
    /* tone, two one-pole low-passes in a row (12 dB / octave) from 12 kHz down to 190 Hz, exactly out of the way when
     * open; sub, a plain sine at the envelope's level faded in under the driven signal (up to three quarters), past
     * the drive and the tone; then the clipper, driven at least 1.76x so that a plain sine with no drive just reaches
     * its ceiling: whatever the settings, the output is at full scale, and a closed tone is a dark wall, not a quiet one */
    int32_t tc = 2048;                                         /* Q11 */
    if (rc[REC_TONE]) {
        const uint32_t wq = (56000u * exp2neg((uint32_t)(rc[REC_TONE] * 6 * 4096) / ALT_MAX)) >> 16;   /* 2 pi f / rate, Q16 */
        tc = (int32_t)((wq << 11) / (65536u + wq));
        if (tc < 1) tc = 1;
    }
    const int32_t sub = (rc[REC_SUB] * 1536) / ALT_MAX;        /* Q11, 0 .. 0.75 */
    const int16_t *tab = op1_sin_tab();

    /* ---- per sample ---- */
    uint32_t p = t->ph;
    int32_t m = m0, g = g0, av = a0, lp = t->hp, dc = t->dcs, tl = t->tail, y = 0, z1 = t->z1, z2 = t->z2;
    int32_t f1 = t->f1, f2 = t->f2;
    int32_t gg = (int32_t)(((int64_t)g0 * g1) >> 11);          /* envelope x stage 1's gain, Q27 (up to 8.0) */
    const int32_t dgg = ((int32_t)(((int64_t)t->lvl * g1) >> 11) - gg) >> 7;
    const int32_t o = t->off;
    for (int n = 0; n < OP1_BLOCK; n++) {
        int32_t i = (inc[n] >> OCT_DOWN) + o;
        if (i < 0) i = 0;
        uint32_t ic = (uint32_t)i >> 12;                       /* sag: take off the part the pitch has fallen */
        if (ic > 32767) ic = 32767;
        i -= (int32_t)((ic * dq) >> 4);
        uint32_t ib = (uint32_t)i >> 13;
        if (ib > 16383) ib = 16383;
        p += (uint32_t)i + ib * (uint32_t)(m >> 8);           /* note + glide + pitch envelope */
        m += dm;

        const uint32_t q = p + ((uint32_t)(((f1 + f2) >> 1) * fb) << 1);   /* feedback: the phase the waves are read at */
        const int32_t k = (int32_t)(q >> 19), fr = (int32_t)((q >> 4) & 0x7FFF);
        const int32_t s0 = op1_sin_q(tab, k), s1 = op1_sin_q(tab, k + 1);
        const int32_t sn = s0 + (((s1 - s0) * fr) >> 15);      /* sine, interpolated */
        int32_t tr = (int32_t)q >> 15;                         /* triangle, in phase with the sine */
        if (tr > 32768) tr = 65536 - tr; else if (tr < -32768) tr = -65536 - tr;
        int32_t x = sn + (((tr - sn) * sh_tri) >> 15);
        x += ((((int32_t)q >> 16) - x) * sh_saw) >> 15;        /* saw: rises through 0 at phase 0, like the others */

        /* From here the signal is Q18 (1.0 = 262144), eight times finer than the 16 bits it leaves in: the drive
         * amplifies a dying tail by up to 30 dB, and what the tail is rounded to here is the noise heard on it.
         * A product must stay inside 32 bits, so the gains give up the bits the signal takes (Q8, Q11, Q12). */
        /* the sub: a sine at the note's own phase (p, not q: feedback does not bend it) at the envelope's level */
        int32_t sc = sn;
        if (fb) {
            const int32_t kc = (int32_t)(p >> 19), c0 = op1_sin_q(tab, kc);
            sc = c0 + (((op1_sin_q(tab, kc + 1) - c0) * (int32_t)((p >> 4) & 0x7FFF)) >> 15);
        }
        const int32_t clean = ((sc * (g >> 15)) >> 12) + ((sc * ((g >> 8) & 127)) >> 19);
        g += dg;
        /* amp envelope and stage 1's gain in one product (gg, Q27), so that the rounding comes after the gain; its
         * top 15 bits and the 7 below them. Then the bias, which follows the envelope */
        const int32_t gh = gg >> 15, gl = (gg >> 8) & 127;
        x += bk;                                               /* (bk is Q15, as the oscillator) */
        x = ((x * gh) >> 9) + ((x * gl) >> 16);
        gg += dgg;
        x = x > Q18_LIM ? Q18_LIM : (x < -Q18_LIM ? -Q18_LIM : x);
        x -= (cube18(x) * mix) >> 12;
        dc += (x * 256 - dc) >> 11;                            /* AC coupling */
        x -= ((dc >> 8) * cpl) >> 12;
        lp += (x * 256 - lp) >> 6;                             /* lift what is above ~220 Hz */
        x += ((x - (lp >> 8)) * lift) >> 8;
        x = x > 655360 ? 655360 : (x < -655360 ? -655360 : x);
        x = (x * g2) >> 8;                                     /* stage 2 */
        x = x > Q18_LIM ? Q18_LIM : (x < -Q18_LIM ? -Q18_LIM : x);
        const int32_t h2 = x > Q18_ONE ? Q18_ONE : (x < -Q18_ONE ? -Q18_ONE : x);
        x -= cube18(x);
        x += ((h2 - x) * clip) >> 12;                          /* soft .. hard, as the last clipper */

        z1 += ((x - z1) * tc) >> 11;                           /* tone */
        z2 += ((z1 - z2) * tc) >> 11;
        x = z2;
        x += ((clean - x) * sub) >> 11;                        /* sub */

        if (fb) {                                              /* feedback's tap: here, through a fixed soft clip, so */
            int32_t ft = (x * 7209) >> 12;                     /* that the clip knob does not also set how wild it is */
            ft = ft > Q18_LIM ? Q18_LIM : (ft < -Q18_LIM ? -Q18_LIM : ft);
            f2 = f1;
            f1 = (ft - cube18(ft)) >> 3;
        }
        x = (x * cg) >> 10;                                    /* the clipper, last */
        const int32_t xc = x > Q18_LIM ? Q18_LIM : (x < -Q18_LIM ? -Q18_LIM : x);
        const int32_t ys = xc - cube18(xc);
        const int32_t yh = x > Q18_ONE - 8 ? Q18_ONE - 8 : (x < 8 - Q18_ONE ? 8 - Q18_ONE : x);   /* hard: brick wall at +-1 */
        y = ys + (((yh - ys) * clip) >> 12);
        y = (y * (av >> 10)) >> 12;                            /* attack: a fade of the level, not of the tone */
        av += da;

        if (t->restart) { tl = (t->last_y - y) * 256; t->restart = 0; } /* a new note carries on from the last sample: no click */
        y += tl >> 8;                                          /* and that offset fades in 1.5 ms */
        tl -= tl >> 7;
        out[n] = op1_sat16((((y + 4) >> 3) * og) >> 15);       /* back to 16 bits */
    }
    t->ph = p;
    t->hp = lp;
    t->z1 = z1;
    t->f1 = f1;
    t->f2 = f2;
    t->z2 = z2;
    t->dcs = dc;
    t->tail = tl;
    t->last_y = y;
    t->last_inc = (inc[OP1_BLOCK - 1] >> OCT_DOWN) + o;
    if (t->lvl == 0 && (tl >> 11) == 0) t->live = 0;

    if (v == owner) {                                          /* the screen follows the newest note */
        vis_inc = (uint32_t)t->last_inc;
        vis_lvl = t->lvl >> 15;
    }
}

/* the engine's page (KeyHandler thread): the orange push steps the play mode; a push of blue, green or white switches
 * that knob to its third value and back, and while it is switched a turn (without Shift) steps that value */
static int ty_input(int page, int ev, int a, int b)
{
    static int32_t acc[3];
    if (page != OP1_ENG_T1 || a < 0 || a > 3) return 0;
    const int slot = op1_current_slot();
    int16_t r[REC_N];
    if (ev == OP1_ENG_PUSH) {
        if (b && a == 3) {
            rec_get(slot, r);
            r[REC_MODE] = (int16_t)((r[REC_MODE] + 1) % MODE_N);
            op1_engine_record_store(slot, TY_NAME, r, REC_N);
        } else if (b) {
            alt[a] = (uint8_t)((alt[a] + 1) % (a == 2 ? 3 : 2));
            acc[a] = 0;
        }
        return 1;
    }
    if (ev != OP1_ENG_TURN || a == 3 || !alt[a] || op1_shift_held()) return 0;
    int st = op1_eng_detents(&acc[a], b);
    if (st) {
        rec_get(slot, r);
        if (a == 1) st = -st;                                  /* tone is kept as "closed by": clockwise opens it */
        const int at = a == 2 && alt[2] == 2 ? REC_SAG : 1 + a;
        int v = r[at] + st;
        v = v < 0 ? 0 : (v > ALT_MAX ? ALT_MAX : v);
        if (v != r[at]) { r[at] = (int16_t)v; op1_engine_record_store(slot, TY_NAME, r, REC_N); }
    }
    return 1;
}

/* own envelope (finished) and the input hook: the 40-byte op1_engine_ext2_t (plugin.json "own_envelope",
 * "engine_input"); the fields of the hooks it does not use stay 0 */
const op1_engine_ext2_t ENGINE_tyr8 = {
    { { ty_note_on, ty_render, ty_note_off, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } },
      ty_finished, 0, 0 },
    ty_input, 0, 0,
};

/* ======================= screen (GUI thread) =======================
 * tyr8.svg (gen_svg.py, geometry in tyr8_geom.h): a tyre rolling on a ground line.
 * Three-quarter view as an oblique projection: the wheel's face is squashed sideways (TY_KX) and the tyre's width is
 * a fixed offset to the far side (TY_WX, TY_WY), so every part is still one affine matrix.
 *   size     amp decay (orange): compact car tyre .. monster truck wheel
 *   treads   clipper shape (Shift + green): slick .. off-road lugs (their near faces are SVG, the rest lines here)
 *   spin     the sounding pitch, while a note sounds; it coasts down after
 *   bounce   every restarted note kicks the wheel off the ground, harder with the pitch envelope amount (green);
 *            the ground grows bumps with it too, and the rolling wheel hops over them
 *   labels   one block a corner, each encoder in its colour (blue and green on top, white and orange below): name,
 *            value and a gauge; while Shift is held the Shift set. The play mode is under orange's block
 * The native renderer is not known to combine nested transforms, so the rig, the spin group and every tread block are
 * siblings and each gets its whole matrix here (row vectors, Q10: x' = x a + y c + tx, y' = x b + y d + ty). */
#define LBL_W     84                          /* width of a corner's label block, px */
#define TICK_PX   20                          /* ground bump spacing */
static void *n_rig, *n_spin, *n_tread[TY_TREAD_N];
static uint32_t ang;                          /* wheel angle, 8192 << 8 a turn */
static uint32_t scroll;                       /* ground travelled, Q10 px */
static int32_t  run;                          /* 0 .. 1024: how much of the pitch's speed the wheel has */
static int32_t  by, bv;                       /* bounce: height (Q10 px, up is negative) and speed (Q10 px / s) */
static uint32_t seen_trig, seen_tick;
static const uint8_t COL_KNOB[4][4] = { { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF },
                                        { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
static const uint8_t COL_GREY[4] = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t COL_BUMP[4] = { 0xDF, 0xD9, 0xFF, 0xFF };

static inline int32_t tsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t tcos(int32_t a) { return tsin(a + 2048); }

static void ty_screen_init(void *scr)
{
    static const char *const ids[TY_TREAD_N] = { "tread0", "tread1", "tread2", "tread3", "tread4", "tread5", "tread6",
                                                 "tread7", "tread8", "tread9", "tread10", "tread11", "tread12", "tread13" };
    n_rig = op1_find_node(scr, "wheel-rig");
    n_spin = op1_find_node(scr, "wheel-spin");
    for (int i = 0; i < TY_TREAD_N; i++) n_tread[i] = op1_find_node(scr, ids[i]);
    for (int k = 0; k < 4; k++) op1_engine_shift(&ENGINE_tyr8.ext.voice, k, 1);   /* Shift + turn: words 5, 6, 7 */
}

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, 0x600, c);
}

/* bump height (px) of ground tick i at pitch envelope amount amt: a fixed pattern per tick, so it scrolls */
static int32_t bump(uint32_t i, int32_t amt)
{
    const uint32_t h = (i * 2654435761u) >> 30;                /* 0 .. 3 */
    return (int32_t)((h * (uint32_t)amt * 2u) >> 15);          /* up to 6 px */
}

static void ty_screen_update(void *scr, int dt)
{
    const int slot = OP1_SCREEN_SLOT & 7;
    int16_t k[4];
    op1_synth_knobs_live(slot, k);
    const int16_t *w = op1_synth_knobs(slot);
    const int32_t clip = w[6] < 0 ? 0 : w[6];
    if (dt < 0) dt = 0;
    if (dt > 50) dt = 50;

    /* size and tread height */
    const int32_t s = 440 + ((523 * k[3]) >> 15);                            /* 0.43 .. 0.94, Q10 */
    const int32_t kt = (clip * 1024) >> 15;                                  /* tread height 0 .. 1, Q10 */
    const int32_t rout = (s * (OP1_Q10(TY_R_TYRE) + kt * TY_TREAD_H)) >> 10; /* outer radius on screen, Q10 px */

    /* spin: the sounding pitch (12 .. 200 Hz counted), 32 Hz = one turn a second */
    const int32_t lv = vis_lvl;
    const int32_t want = lv > 1024 ? 1024 : lv;
    run = want > run ? run + ((want - run + 1) >> 1) : run - (run >> 5) - 1;
    if (run < want) run = want;
    int32_t hz = (int32_t)(((vis_inc >> 16) * 88200u) >> 16);
    hz = hz < 12 ? 12 : (hz > 200 ? 200 : hz);
    const uint32_t da = (uint32_t)(((hz * 66 * run) >> 10) * dt);            /* table units << 8 */
    ang += da;
    const uint32_t ds = (uint32_t)(((int64_t)da * rout >> 10) * 6434 >> 21); /* arc = angle x radius, Q10 px */
    scroll += ds;

    /* bounce: a kick on every restarted note and on every bump that passes under the wheel */
    const int32_t amt = k[1];
    if (vis_trig != seen_trig) {
        seen_trig = vis_trig;
        bv = -OP1_Q10(40) - ((OP1_Q10(110) * (amt >> 5)) >> 10);
    }
    const uint32_t tick = (scroll + OP1_Q10(TY_HUB_X)) / OP1_Q10(TICK_PX);
    if (tick != seen_tick) {
        seen_tick = tick;
        const int32_t h = bump(tick, amt);
        if (h > 0 && by == 0 && bv == 0 && run > 64) bv = -OP1_Q10(18) * h;   /* not over a note's kick */
    }
    for (int i = 0; i < dt; i++) {                                           /* one step a millisecond */
        bv += 922;                                                           /* 900 px / s^2 */
        by += bv / 1000;
        if (by >= 0) {
            by = 0;
            bv = -(bv * 45) / 100;
            if (bv > -OP1_Q10(15)) bv = 0;
        }
    }

    /* matrices */
    const int32_t cx = OP1_Q10(TY_HUB_X), cy = OP1_Q10(TY_GROUND_Y) - 768 - rout + by;   /* hub on screen */
    const int32_t hx = TY_HUB_X, hy = TY_HUB_Y;                                          /* hub in the drawing */
    const int32_t sk = (s * TY_KX) >> 10;                                    /* the face's sideways scale */
    if (n_rig) {
        int32_t m[9] = { sk, 0, 0, 0, s, 0, cx - sk * hx, cy - s * hy, 1024 };
        op1_node_set_transform(n_rig, m);
    }
    const int32_t a = (int32_t)(ang >> 8);
    const int32_t c = (tcos(a) * s) >> 15, sn = (tsin(a) * s) >> 15;         /* s Rot(a), Q10 */
    if (n_spin) {
        const int32_t l00 = (c * TY_KX) >> 10, l01 = (-sn * TY_KX) >> 10;    /* squash after the spin */
        int32_t m[9] = { l00, sn, 0, l01, c, 0, cx - (l00 * hx + l01 * hy), cy - (sn * hx + c * hy), 1024 };
        op1_node_set_transform(n_spin, m);
    }
    for (int i = 0; i < TY_TREAD_N; i++) {
        if (!n_tread[i]) continue;
        /* the block sits at angle f after the spin; u points out of the wheel there. A point q (from the hub, already
         * spun) goes to  A s [ (R + kt (q.u - R)) u + (q.t) t ]: the radial part squashed towards the carcass radius R,
         * then the face's sideways squash A. Linear part L = A s (kt u u' + t t') Rot(a), constant part A s (1 - kt) R u. */
        const int32_t f = a + (8192 * i) / TY_TREAD_N;
        const int32_t ux = tcos(f) >> 1, uy = tsin(f) >> 1;                  /* Q14 */
        const int32_t n00 = (kt * ((ux * ux) >> 14) + 1024 * ((uy * uy) >> 14)) >> 14;   /* Q10 */
        const int32_t n01 = ((kt - 1024) * ((ux * uy) >> 14)) >> 14;
        const int32_t n11 = (kt * ((uy * uy) >> 14) + 1024 * ((ux * ux) >> 14)) >> 14;
        const int32_t l00 = (((n00 * c + n01 * sn) >> 10) * TY_KX) >> 10, l01 = (((-n00 * sn + n01 * c) >> 10) * TY_KX) >> 10;
        const int32_t l10 = (n01 * c + n11 * sn) >> 10, l11 = (-n01 * sn + n11 * c) >> 10;
        const int32_t kr = (s * (1024 - kt) >> 10) * TY_R_TYRE;              /* s (1 - kt) R, Q10 px */
        int32_t m[9] = { l00, l10, 0, l01, l11, 0,
                         cx + ((((kr * ux) >> 14) * TY_KX) >> 10) - (l00 * hx + l01 * hy),
                         cy + ((kr * uy) >> 14) - (l10 * hx + l11 * hy), 1024 };
        op1_node_set_transform(n_tread[i], m);
    }
    op1_screen_draw(scr, dt);

    /* the far faces of the tread blocks and the edges across the tread, where the tread shows: the side of the wheel
     * whose outward normal looks along the tyre's width (the SVG's black near face hides the rest of the far edge) */
    void *r = op1_raster();
    if (kt > 40) {
        const int32_t wx = s * TY_WX, wy = s * TY_WY;                        /* near face to far face, Q10 px */
        const int32_t rb = OP1_Q10(TY_R_TYRE) >> 2, rt = (OP1_Q10(TY_R_TYRE) + kt * TY_TREAD_H) >> 2;   /* Q8 px */
        for (int i = 0; i < TY_TREAD_N; i++) {
            const int32_t f = a + (8192 * i) / TY_TREAD_N;
            const int32_t ux = tcos(f) >> 1, uy = tsin(f) >> 1;              /* Q14 */
            if ((ux * TY_WX * 1024) / TY_KX + uy * TY_WY < 16384 * 4) continue;
            /* corners: carcass -w, top -wt, top +wt, carcass +w; (radius, along the tangent) -> screen */
            static const int32_t tw[4] = { -TY_TREAD_W >> 2, -TY_TREAD_WT >> 2, TY_TREAD_WT >> 2, TY_TREAD_W >> 2 };
            int32_t px[4], py[4];
            for (int j = 0; j < 4; j++) {
                const int32_t rr = j == 0 || j == 3 ? rb : rt;
                const int32_t qx = (rr * ux - tw[j] * uy) >> 12, qy = (rr * uy + tw[j] * ux) >> 12;   /* Q10 px */
                px[j] = cx + (((qx * s) >> 10) * TY_KX >> 10);
                py[j] = cy + ((qy * s) >> 10);
            }
            for (int j = 0; j < 3; j++) ln(r, px[j] + wx, py[j] + wy, px[j + 1] + wx, py[j + 1] + wy, COL_BUMP);
            ln(r, px[1], py[1], px[1] + wx, py[1] + wy, COL_BUMP);
            ln(r, px[2], py[2], px[2] + wx, py[2] + wy, COL_BUMP);
        }
    }

    /* ground: a tick every TICK_PX, scrolling left; bumps over them with the pitch envelope amount */
    const int32_t gy = OP1_Q10(TY_GROUND_Y);
    const uint32_t first = scroll / OP1_Q10(TICK_PX);
    const int32_t frac = (int32_t)(scroll % OP1_Q10(TICK_PX));
    for (int j = 0; j <= 320 / TICK_PX; j++) {
        const int32_t x = j * OP1_Q10(TICK_PX) - frac;
        if (x < OP1_Q10(8) || x > OP1_Q10(312)) continue;
        ln(r, x, gy + 1024, x, gy + OP1_Q10(4), COL_GREY);
        const int32_t h = bump(first + (uint32_t)j, amt);
        if (h > 0) {
            ln(r, x - OP1_Q10(3), gy, x, gy - OP1_Q10(1) * h, COL_BUMP);
            ln(r, x, gy - OP1_Q10(1) * h, x + OP1_Q10(3), gy, COL_BUMP);
        }
    }

    /* the four encoders, each in its colour: name, gauge, value. While Shift is held, the four Shift values */
    static const char *const names[2][4] = { { "SHAPE", "KNOCK", "KNOCK LEN", "DECAY" }, { "DRIVE", "CLIP", "SUSTAIN", "GLIDE" } };
    const int shift = op1_shift_held() != 0;
    int32_t val[4] = { k[0], k[1], k[2], k[3] };
    int32_t num[4];                                                          /* the value as shown */
    char unit[4] = { '%', '%', 'M', 'S' };                                   /* M: ms, S: seconds with one decimal */
    const char *name[4];
    for (int i = 0; i < 4; i++) name[i] = names[shift][i];
    if (shift) {
        val[0] = w[5] < 0 ? 0 : w[5];
        val[1] = clip;
        val[2] = (w[7] & 127) * 258;
        val[3] = ((w[7] >> 8) & 127) * 258;
        num[0] = (val[0] * 100 + 16384) >> 15;
        num[1] = (val[1] * 100 + 16384) >> 15;
        num[2] = ((w[7] & 127) * 100 + 63) / 127;
        num[3] = (((w[7] >> 8) & 127) * 500 + 63) / 127;
        unit[2] = '%'; unit[3] = 'M';
    } else {
        num[0] = ((val[0] * 200 + 16384) >> 15) - 50;                        /* -50 feedback sine, 0 sine, 50 triangle, 100 saw, 150 feedback saw */
        num[1] = (val[1] * 100 + 16384) >> 15;
        num[2] = (int32_t)((1500u * exp2neg((uint32_t)((32767 - val[2]) * 25514) >> 15) + 32768u) >> 16);  /* ms */
        if (num[2] >= 1000) { num[2] = (num[2] + 50) / 100; unit[2] = 'S'; }
        num[3] = (int32_t)((40u * exp2neg((uint32_t)((32767 - val[3]) * 21799) >> 15) + 32768u) >> 16);    /* s / 10 */
        /* a pushed encoder shows and turns its third value */
        static const char *const third[4] = { "SUB", "TONE", "ATTACK", "SAG" };
        int16_t rc[REC_N];
        rec_get(slot, rc);
        for (int i = 0; i < 3; i++) {
            if (!alt[i]) continue;
            const int sag = i == 2 && alt[2] == 2;
            const int32_t a = i == 1 ? ALT_MAX - rc[REC_TONE] : (sag ? rc[REC_SAG] : rc[1 + i]);   /* tone: how open it is */
            name[i] = third[sag ? 3 : i];
            val[i] = (a * 32767) / ALT_MAX;
            num[i] = (a * 100 + ALT_MAX / 2) / ALT_MAX;                       /* attack: 100 ms at the top */
            unit[i] = i == 2 && !sag ? 'M' : '%';
        }
    }
    for (int i = 0; i < 4; i++) {
        const int32_t x0 = OP1_Q10(i & 1 ? 312 - LBL_W : 8), y = OP1_Q10(i < 2 ? 8 : 106);
        const int32_t len = (OP1_Q10(LBL_W) >> 5) * (val[i] >> 5) >> 5;
        char t[8]; int n = 0;
        int32_t v = num[i];
        if (v < 0) { t[n++] = '-'; v = -v; }
        if (unit[i] == 'S') { t[n++] = (char)('0' + v / 10); t[n++] = '.'; t[n++] = (char)('0' + v % 10); t[n++] = 'S'; }
        else {
            if (v >= 100) t[n++] = (char)('0' + v / 100);
            if (v >= 10) t[n++] = (char)('0' + v / 10 % 10);
            t[n++] = (char)('0' + v % 10);
            if (unit[i] == 'M') { t[n++] = 'M'; t[n++] = 'S'; } else t[n++] = '%';
        }
        t[n] = 0;
        sq_text(r, x0, y, 1024, name[i], 1024, COL_KNOB[i]);
        sq_text(r, x0 + OP1_Q10(LBL_W) - sq_width(1024, t) + OP1_Q10(2), y, 1024, t, 1024, COL_KNOB[i]);
        ln(r, x0, y + OP1_Q10(10), x0 + OP1_Q10(LBL_W), y + OP1_Q10(10), COL_GREY);
        if (len > 512) ln(r, x0, y + OP1_Q10(10), x0 + len, y + OP1_Q10(10), COL_KNOB[i]);
    }
    /* the play mode, under orange's block and in its colour: its push steps it */
    static const char *const modes[MODE_N] = { "MONO GLIDE", "MONO RETRIG", "POLY" };
    const char *const mt = modes[mode_of(slot)];
    sq_text(r, OP1_Q10(312) - sq_width(1024, mt) + OP1_Q10(2), OP1_Q10(122), 1024, mt, 1024, COL_KNOB[3]);
}

const op1_screen_t SCREEN_tyr8 = { ty_screen_init, ty_screen_update };
