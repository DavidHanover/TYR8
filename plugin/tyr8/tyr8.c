/* TYR8: an 808 bass for the OP-1 (hundekraft kennel plugin, tyr8.knl).
 *
 *   T1     turn (words 0..3, the OP-1's own)   Shift + turn (taken by ty_input)
 *   blue   two pages, a blue push (with or without Shift) switching them:
 *            SHAPE page   shape, see render (word 0)           clip, soft .. hard (word 6)
 *            DRIVE page   drive, up to 30 dB (word 5)          SMP TUNE: the sample's tune (record [9])
 *   green  pitch envelope amount (knock)       knock length, 20 ms .. 1.5 s                     (word 7)
 *   white  tone: low-pass after the drive      sub: a clean sine an octave down (at the note's own   (record)
 *                                               pitch where that would be under ~40 Hz), up to all of it
 *   orange sag, -100 .. 100 %: the pitch falls (up)    glide time, 0 .. 500 ms (record)
 *          or rises (down) while the note lasts; the turn is taken here (ty_input): the fall is word 3, the rise the
 *          record's [7], so a sound saved before 0.9.13 keeps its fall
 *   orange pushes on T1 do nothing; the play mode is a T2 orange push, shown under RELEASE (record)
 *   a white push swaps white over to the sample's own tone (S.TONE, record [12]) and, with Shift, its high-pass
 *          (SMP HPF, record [14]); not saved
 *   a green push swaps green over to the sample layer (not saved): turn its level, Shift + turn its attack fade
 *          (SMP ATK, record [13]); it plays the way the sampler does (record [8]). SMP TUNE, -24 .. +24 semitones on
 *          top of the sampler's own, following the key, is Shift + blue on blue's DRIVE page. Every push swaps with or
 *          without Shift
 *
 *   T2     the OP-1's own envelope words (t2_page), drawn by the engine's T2 page:
 *          blue attack 0 .. 1 s, green decay 0.1 .. 4 s, white sustain, orange release 10 ms .. 4 s, its top 5 % HOLD;
 *          a green push steps the sample's source: AUTO (the first other slot with a sampler recording), slot 1 .. 8;
 *          an orange push steps the play mode, shown under RELEASE
 *
 * The engine sounds two octaves below the key played (OCT_DOWN), so the OP-1's octave 0 is already bass.
 * Signal: oscillator (with feedback from the output) -> amp envelope -> drive (two saturating stages, see render) ->
 * tone -> sub blended in -> soft / hard clipper, always last -> attack fade. The
 * oscillator's pitch is the note plus the pitch envelope (and the glide). A restarted note starts the oscillator at
 * phase 0, so every hit is the same hit. The engine shapes its own level (own_envelope) and reads the T2 words as its
 * envelope. The level falls from full to the sustain level at the decay rate while the key is down; after the key is
 * up it falls to 0 at the release rate, or with release at HOLD on at the decay rate, so sustain 0 with HOLD is the
 * 808 one-shot that rings its full decay whatever the key does.
 *
 * Play mode, stepped by pushing orange and saved with the sound:
 *   MONO GLIDE   one voice, the newest key. A note played while another is held and still sounding is legato: no
 *                restart, the pitch glides; the key still held underneath takes over again when the newest goes up
 *   MONO RETRIG  one voice, every note restarts on its own pitch, no glide; letting the newest key go restarts the
 *                key still held underneath
 * (0.9.9 dropped POLY: six full chains were too much for the OP-1, and it sounded bad. A sound saved in it plays in
 * MONO GLIDE.)
 *
 * The code runs in the audio interrupt at 88.2 kHz, 128 samples a call: integers only, no division and no 64-bit
 * arithmetic per sample (both once a block). The sound state is static (tv), not in the OP-1's voice objects.
 *
 * Threads: note_on / note_off run on the key thread and only write the small volatile fields under "keys"; render
 * (audio) owns everything else; the screen (GUI) reads the three vis_ fields.
 * Copyright (C) 2026 David Hanover. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "op1_plugin.h"
#include "seq/seq_text.h"                     /* sq_text: the core's stroke font (uppercase, digits, . % : + - / #) */

#define ONE30       (1 << 30)                 /* envelope full scale */
#define ONE29       (1 << 29)                 /* the sag factor's 1.0 */
#define SILENT      (ONE30 >> 17)             /* -102 dB: the note is over (the drive can lift a tail 30 dB) */
#define Q18_ONE     (1 << 18)                 /* the signal path's 1.0 */
#define Q18_LIM     (3 << 17)                 /* and its 1.5, where the saturations go flat */
#define AUDIBLE     (ONE30 >> 7)              /* -42 dB: below this a held note no longer makes the next one legato */
#define OCT_DOWN    2                         /* the engine plays this many octaves below the key: bass at octave 0 */
#define OUT_GAIN    20675                     /* -4 dB: the synth bus's 35 Hz high-pass after the voices tilts a full-scale
                                               * square-ish note and throws its edges and onset up to 6 dB over (measured
                                               * on the driven patches, 2nd order); at full scale the OP-1 clipped every
                                               * note (red, a click). Up to 0.9.16 it was 32767; 0.9.17 .. 0.9.19 -6.5 dB,
                                               * which the owner found too quiet. An ordinary note stays inside; the
                                               * heaviest patches go up to ~2 dB over */
#define FADE_LSB    96                        /* the tail's last fade starts below this output peak (-51 dBFS) */

/* ---- keys (key thread writes, audio reads) ---- */
static volatile uint8_t  held[OP1_VOICES];    /* the voice's key is down */
static volatile uint8_t  legato_req[OP1_VOICES]; /* its note-on came while another key was down */
static volatile uint32_t stamp[OP1_VOICES];   /* note-on order: the newest held voice plays */
static volatile uint32_t beat[OP1_VOICES];    /* render call count when the voice last rendered: is it still alive */
static volatile uint32_t on_at[OP1_VOICES];   /* render call count at its note-on: voices on at the same count are one press */
static volatile uint32_t stamp_next = 1, calls;
static volatile int8_t   owner = -1;          /* the newest key's voice: its pitch, velocity and notes */
/* The sound plays on one OP-1 voice for as long as it rings (rv), whichever key owns it: up to 0.9.23 it moved to
 * every new key's voice, and the OP-1's handling of each voice's output made that move a click on every press (poly,
 * legato too; the OP-1's mono, one voice, never clicked). The owner hands over what only its own render can read: its
 * pitch increments, its velocity, and which note it is. The renderer takes a new note once the owner has rendered it,
 * so a renderer that goes first in the OP-1's voice order plays it a block (1.45 ms) later */
static int8_t   rv = -1;                      /* the voice the sound plays on */
static int32_t  own_inc[OP1_BLOCK];           /* the owner's increments, its last render */
static int32_t  own_vel = 32767;
static uint32_t own_stamp;                    /* its note then */
static int8_t   own_v = -1;                   /* and which voice it was */
static uint32_t rv_seen;                      /* the renderer's beat at the owner's last render */
static uint8_t  rv_stale;                     /* owner renders in a row with no render of rv between: rv is gone */

/* ---- the engine's record, saved with the sound: values the eight words have no room for. 0 is each one's default ----
 *   [0] play mode (a T2 orange push steps it)
 *   [5] sub: the clean sine's share, 0 .. 32767 (Shift + white)
 *   [6] glide time, 0 .. 32767 (Shift + orange)
 *   [7] sag's rise: how far the pitch rises while the note lasts, 0 .. 32767 (orange turned below 0; the fall is word 3)
 *   [8] the sample layer's level, 0 (off) .. 32767 (green, swapped)
 *   [9] its tune on top of the sampler's own: 25 + semitones (1 .. 49), 0 for none (Shift + blue, DRIVE page); rec_get
 *       gives the semitones, -24 .. 24
 *   [10] its source: 0 AUTO, 1 .. 8 slot 1 .. 8 (a T2 green push)
 *   [11] not used (0.9.27 .. 0.9.33: a flip of the sampler's direction, Shift + green push; it plays the sampler's way)
 *   [12] its own tone, how far closed: 0 open .. 32767 (white, swapped; shown as S.TONE 100 .. 0 %)
 *   [13] its attack fade, 0 (none) .. 32767 (100 ms) (Shift + green, swapped: SMP ATK)
 *   [14] its high-pass before its drive, 0 (off) .. 32767: 30 Hz .. 3 kHz (Shift + white, swapped)
 * The sample layer runs through its own copy of the drive (the same settings, its own states), then its own tone
 * (S.TONE), and joins the note after the note's tone, before the clipper: TONE does not darken it.
 * [1] .. [4] held the push pages' values up to 0.9.3 and are not read. */
enum { MODE_GLIDE, MODE_RETRIG, MODE_N };
enum { REC_MODE = 0, REC_SUB = 5, REC_GLIDE = 6, REC_RISE = 7, REC_SLVL = 8, REC_STUNE = 9, REC_SRC = 10, REC_SFLIP = 11, REC_STONE = 12, REC_SATK = 13, REC_SHP = 14, REC_N };
#define SRC_AUTO 0
#define TY_NAME "tyr8"
static uint32_t rec_key;                      /* the record's owner key, worked out once */
static volatile uint8_t blue_pg;              /* blue's page: 0 SHAPE (Shift: CLIP), 1 DRIVE (Shift: SMP TUNE); a push switches.
                                               * The screen's state, not saved */
static volatile uint8_t white_smp;            /* a white push swaps white to S.TONE / SMP HPF (1) or TONE / SUB: not saved */
static volatile uint8_t green_smp;            /* a green push swaps green to the sample layer (1) or knock (0): not saved */
#ifndef TYRE_BADGE_ON
#define TYRE_BADGE_ON 0
#endif
static volatile uint8_t tyre_badge = TYRE_BADGE_ON;   /* Shift + orange push on T1: the tyre's sidewall markings, an easter egg (not saved) */
static void rec_get(int slot, int16_t out[REC_N])
{
    if (!rec_key) rec_key = op1_engine_rec_owner(TY_NAME);
    const int16_t *r = op1_engine_record(slot, rec_key);
    out[REC_MODE] = r && r[0] > 0 && r[0] < MODE_N ? r[0] : MODE_GLIDE;
    for (int i = 1; i < REC_N; i++) out[i] = !r || r[i] < 0 ? 0 : r[i];
    if (!r) out[REC_GLIDE] = 16384;           /* a new sound (no record yet): glide in the middle, 250 ms */
    out[REC_STUNE] = out[REC_STUNE] >= 1 && out[REC_STUNE] <= 49 ? out[REC_STUNE] - 25 : 0;   /* semitones */
    out[REC_SFLIP] = 0;                       /* 0.9.27 .. 0.9.33 kept a flip of the sampler's direction here: not read */
    if (out[REC_SRC] > 8) out[REC_SRC] = SRC_AUTO;
}
/* back to what the record holds */
static void rec_put(int slot, int16_t r[REC_N])
{
    int16_t w[REC_N];
    for (int i = 0; i < REC_N; i++) w[i] = r[i];
    w[REC_STUNE] = r[REC_STUNE] ? (int16_t)(r[REC_STUNE] + 25) : 0;
    op1_engine_record_store(slot, TY_NAME, w, REC_N);
}
static int mode_of(int slot) { int16_t r[REC_N]; rec_get(slot, r); return r[REC_MODE]; }

/* ---- the sample layer's source: another slot's sampler recording, found as UZ-1 finds its sample ---- */
#define SCAN_THR  256                         /* -42 dBFS: where the recording starts and ends */
#define RESCAN    64                          /* render calls between scans at note-on (about 90 ms) */
/* the recording's part to play and how the sampler plays it: its rate (the read step a key gives: step, Q12 samples a
 * voice sample, = key's phase increment x rate >> 38, as R246's sampler voice: sampler+'s sp_voice.c), its gain (Q15)
 * and its direction */
typedef struct { const int16_t *d; int32_t on, end, rate, gain; int8_t slot; uint8_t ok, rev; } src_t;
/* the sampler's gain, as its voice (sampler+'s sp_tap, bit-exact): g = w7^2 x 2 >> 16, and the sample comes out x g x 2
 * x 16 >> 16, saturated at full scale: x g / 2048, so a quarter of the knob (8192) is 0 dB, the top +24 dB, hard-clipped.
 * Kept as that factor, Q11 */
#define SMP_UNITY 2048
#define RATE_C4 44187388                      /* a recording based at middle C (261.6 Hz): 88200 / (f 16384), Q31 */
static src_t srcb[2];                         /* double buffered: the key thread fills one, render reads the other */
static volatile uint8_t src_i, src_force = 1;
static volatile uint32_t src_at;
static int sb_open(int slot, const int16_t **d, const int16_t **pk, int32_t *cap)
{
    const uint8_t *sb = op1_sample_buffer(slot);
    if (!sb) return 0;
#ifndef OP1_HOST
    if ((uintptr_t)sb >= 0x04000000u || ((uintptr_t)sb & 3)) return 0;
    const uintptr_t dp = *(const uintptr_t *)sb;
    if (!dp || dp >= 0x04000000u || (dp & 1)) return 0;
#endif
    int32_t c = *(const int32_t *)(sb + 0x1038) - OP1_SAMPLE_GUARD;
    if (c > OP1_SAMPLE_MAX) c = OP1_SAMPLE_MAX;
    if (c < 512) return 0;
    *d = op1_sample_data(sb);
    *pk = op1_sample_peaks(sb);
    *cap = c;
    return 1;
}
/* the slot's engine is the synth sampler (R246's engine 6; the table kennel's kn_sampler.c reads) */
/* and its rate: the slot's sampler object (0x01DF6F78[slot], op1_sample_buffer's) +4, set from the sound's base
 * frequency (sampler_set_base_freq, kennel's kn_sampler.c) */
#ifndef OP1_HOST
static inline int slot_is_sampler(int s) { return *(volatile int *)(0xFF8016E4 + (s & 7) * 4) == 6; }
static inline int32_t slot_rate(int s) { const int32_t *o = ((const int32_t *const *)0x01DF6F78)[s & 7]; return o ? o[1] : 0; }
#else
static int host_sampler_type[8];
static int32_t host_sampler_rate[8];
static inline int slot_is_sampler(int s) { return host_sampler_type[s & 7] == 6; }
static inline int32_t slot_rate(int s) { return host_sampler_rate[s & 7]; }
#endif
#define SMP_FULL (OP1_SAMPLE_MAX - OP1_SAMPLE_GUARD)      /* the sampler's markers are fractions of this, 32768 = 1 */
/* where the recording starts and ends: between the sampler's own START and END markers (its knob words 0 and 3, as
 * the user trimmed it on the sampler's page; the whole buffer for a slot that is not a sampler any more), from the
 * buffer's peaks (one a 256 samples) the loud part inside them, then its first loud sample */
static int scan(int slot, src_t *o)
{
    const int16_t *d, *pk;
    int32_t cap;
    o->ok = 0; o->slot = (int8_t)slot;
    if (!sb_open(slot, &d, &pk, &cap)) return 0;
    int32_t m0 = 0, m1 = cap;
    o->rate = RATE_C4; o->gain = SMP_UNITY; o->rev = 0;      /* not a sampler any more: as recorded, at middle C */
    if (slot_is_sampler(slot)) {
        const int16_t *w = op1_synth_knobs(slot);
        /* the sampler's own settings: its gain (word 7, squared, as its voice), its direction (word 4 bit 14), and
         * its tune (the rate; something wild is taken as not set) */
        const int32_t gw = w[7] < 0 ? 0 : w[7], g = (gw * gw * 2 + 0x8000) >> 16;
        o->gain = g > 32767 ? 32767 : g;                      /* Q11: the voice plays it x g / 2048, up to 16x (+24 dB) */
        o->rev = w[4] >= 0x4000;
        const int32_t rt = slot_rate(slot);
        if (rt > RATE_C4 / 64 && rt < RATE_C4 * 32) o->rate = rt;
        const int32_t a = w[0] < 0 ? 0 : w[0], b = w[3] < 0 ? 0 : w[3];
        const int32_t s0 = (int32_t)(((int64_t)a * SMP_FULL) >> 15), s1 = (int32_t)(((int64_t)b * SMP_FULL) >> 15);
        if (s1 > s0) { m0 = s0; m1 = s1 < cap ? s1 : cap; }
    }
    const int nb = cap / 256 < OP1_PEAK_BUCKETS ? cap / 256 : OP1_PEAK_BUCKETS;
    int i0 = -1, i1 = -1;
    for (int i = m0 / 256; i < nb && i * 256 < m1; i++) {
        const int p = pk[i] < 0 ? -pk[i] : pk[i];
        if (p >= SCAN_THR) { if (i0 < 0) i0 = i; i1 = i; }
    }
    if (i0 < 0) return 0;
    int32_t on = i0 * 256 > m0 ? i0 * 256 : m0, e = (i1 + 1) * 256;
    if (e > m1) e = m1;
    for (int32_t i = on; i < on + 256 && i < e; i++) { const int a = d[i] < 0 ? -d[i] : d[i]; if (a >= SCAN_THR / 2) { on = i; break; } }
    on = on - 16 > m0 ? on - 16 : m0;                         /* a little before the transient, not before START */
    if (e > cap - 2) e = cap - 2;
    if (e - on < 64) return 0;
    o->d = d; o->on = on; o->end = e; o->ok = 1;
    return 1;
}
/* setting: SRC_AUTO, the first other slot with a recording, or 1 .. 8 */
static void find_source(int own, int setting, src_t *o)
{
    if (setting != SRC_AUTO) { const int s = setting - 1; if (s == own || !scan(s, o)) { o->ok = 0; o->slot = (int8_t)s; } return; }
    for (int s = 0; s < 8; s++) if (s != own && scan(s, o)) return;
    o->ok = 0; o->slot = -1;
}

/* ---- the T2 words (R246's envelope knobs, saved with the sound as its "adsr") as 0 .. 32767 each ----
 * R246 turns attack, decay and release over 64 .. 16320 and sustain over 0 .. 32767. A slot with no words at all (never
 * on the OP-1) reads as the 808 defaults: no attack, a 0.6 s decay, sustain 0, release HOLD. */
enum { T2_ATTACK, T2_DECAY, T2_SUSTAIN, T2_RELEASE };
#define REL_HOLD 31130                        /* release from 95 %: HOLD, the note finishes its decay after the key */
static void t2_get(int slot, int32_t u[4])
{
    const volatile int16_t *t = op1_t2_words(slot);
    const int32_t w[4] = { t[0], t[1], t[2], t[3] };
    if (!(w[0] | w[1] | w[2] | w[3])) { u[0] = 0; u[1] = 16000; u[2] = 0; u[3] = 32767; return; }
    for (int k = 0; k < 4; k++) {
        int32_t v = k == T2_SUSTAIN ? w[k] : ((w[k] - 64) * 32767) / 16256;
        u[k] = v < 0 ? 0 : (v > 32767 ? 32767 : v);
    }
}
/* the attack in ms, 0 .. 999: a square law, so the short end is fine (10 % is 10 ms) */
static inline int32_t attack_ms(int32_t u) { return (((u * u) >> 15) * 1000) >> 15; }
/* the decay's per-block step, Q16: 0.1 .. 4 s */
static uint32_t exp2neg(uint32_t x);
static inline uint32_t decay_k(int32_t u) { return 4755u * exp2neg((uint32_t)(u * 21799) >> 15) >> 16; }

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
    int32_t  fade;                            /* the tail's last fade, Q15: 32768 none (see render) */
    int32_t  pk, pkacc;                       /* output level: the largest block-end sample of the last 16 blocks, Q18 */
    uint8_t  pkn;                             /* blocks into that window */
    int32_t  z1, z2;                          /* tone: the two low-pass states, Q18 */
    int32_t  f1, f2;                          /* feedback: the last two output samples */
    int32_t  sag;                             /* pitch sag: the note's frequency factor, Q29 (ONE29 = 1): 1/8 .. 2 */
    uint8_t  restart;                         /* this block starts a note: 1 from silence, 2 over a sounding one */
    uint8_t  ph_h;                            /* the phase's 33rd bit: the octave-down sub's phase is (ph_h ph) / 2 */
    int32_t  ow;                              /* the sub's octave-down share, Q12: 0 at the note's own pitch */
    uint8_t  live;                            /* the envelope has not run out (0 at boot: nothing sounds) */
    const int16_t *sd;                        /* the sample layer playing: its data (0: none), */
    int32_t  sp, sst;                         /* where it is and its step a voice sample (< 0: backwards), Q12 samples, */
    int32_t  srate, stm;                      /* the sampler's rate and SMP TUNE's factor (Q16, x 1/4 .. 4): the step follows the key's pitch every block, */
    uint32_t sstamp;                          /* while the note that started it is the one sounding */
    int32_t  sws;                             /* the top of SAMPLE: how far the body is faded out, Q15 (32768 all), smoothed */
    uint8_t  shad;                            /* this note started a sample: the top of SAMPLE fades the body out */
    int32_t  sde;                             /* the sample's level, followed (Q18): the sub steps aside while it plays */
    int32_t  slo, shi;                        /* the part it plays, [slo, shi) */
    int32_t  sg;                              /* the sampler's gain, Q11 (2048 = 0 dB, up to 16x), clipped at full scale as it does */
    int32_t  sdc, slp, sz1, sz2;              /* its drive's coupling and lift states (Q18 << 8), its tone's (Q18) */
    int32_t  sra, sri;                        /* its attack fade, Q15 << 8, and the fade's step a voice sample */
    int32_t  sfo;                             /* voice samples since the recording ended: its chain rings, then fades */
    int32_t  sh1, sh2;                        /* its high-pass's two low-pass states, Q15 << 8 */
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
/* a tone knob's filter coefficient (Q11, 2048 open) for how far it is closed, 0 .. 32767: 12 kHz down 6.4 octaves to
 * 140 Hz, by closed (2 - closed): most of the turn is spent below 2 kHz, where a driven bass changes. Linear in octaves,
 * the top half of the knob did next to nothing (the clipper after the tone puts its own treble back). Once a block */
static int32_t tone_tc(int32_t closed)
{
    if (closed <= 0) return 2048;
    const int32_t oct = (int32_t)(((uint32_t)closed * (uint32_t)(65534 - closed)) >> 15);   /* Q15 */
    const uint32_t wq = (56000u * exp2neg((uint32_t)(oct * 26214) >> 15)) >> 16;           /* 2 pi f / rate, Q16 */
    const int32_t tc = (int32_t)((wq << 11) / (65536u + wq));
    return tc < 1 ? 1 : tc;
}
/* SMP HPF's coefficient, Q15, for the knob 1 .. 32767: 30 Hz .. 3 kHz, even in octaves (6.64); 0 is off */
static int32_t hpf_c(int32_t u)
{
    if (u <= 0) return 0;
    const uint32_t wq = (14006u * exp2neg((uint32_t)((32767 - u) * 27197) >> 15)) >> 16;   /* 2 pi f / rate, Q16 */
    const int32_t c = (int32_t)((wq << 15) / (65536u + wq));
    return c < 1 ? 1 : c;
}

static void ty_note_on(void *self, const int16_t *params)
{
    const int v = op1_voice_index(self); (void)params;
    if (v < 0 || v >= OP1_VOICES) return;
    /* legato over another key still down, pressed before this one: not over a voice the OP-1 started for the same
     * press (unison starts several before the next render). Up to 0.9.15 the second voice of a press was legato over
     * the first, so with unison no note restarted (no knock) until the last tail had died away */
    const uint32_t now = calls;
    int other = 0;
    for (int i = 0; i < OP1_VOICES; i++) if (i != v && held[i] && alive(i) && on_at[i] != now) other = 1;
    legato_req[v] = (uint8_t)other;
    on_at[v] = now;
    held[v] = 1;
    beat[v] = calls;
    stamp[v] = stamp_next++;
    owner = (int8_t)v;
    /* the sample layer's source, now and then (a scan of the peaks): only with the layer on */
    const int slot = op1_current_slot();
    int16_t r[REC_N];
    rec_get(slot, r);
    if (r[REC_SLVL] && (src_force || calls - src_at >= RESCAN)) {
        src_force = 0; src_at = calls;
        const uint8_t n = src_i ^ 1;
        find_source(slot, r[REC_SRC], &srcb[n]);
        src_i = n;
    }
}

static void ty_note_off(void *self)
{
    const int v = op1_voice_index(self);
    if (v < 0 || v >= OP1_VOICES) return;
    held[v] = 0;
    if (owner != v) return;
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
    if (v == rv) {                                             /* the sound's voice: until it has died away, and */
        if (!tv[0].live && own_stamp == tv[0].seen_stamp && (owner < 0 || stamp[owner] == own_stamp)) { rv = -1; return 1; }
        return 0;                                              /* no new note is waiting */
    }
    if (v == owner) return !held[v] && stamp[v] == own_stamp && own_stamp == tv[0].seen_stamp;   /* its note taken in */
    return !held[v];
}

/* ---- the per-sample loop ----
 * What a block needs that does not change in it (blk_t), and one copy of the loop for each combination of the parts
 * that are often not in use: the tone filter (open), the sub (0), feedback (shape between sine and saw) and the pitch
 * and level moves (glide, sag, an attack still fading in). A part that is not in use is left out of its copy, not
 * skipped by a test each sample, and each copy gives exactly the samples the whole loop would: the tone filter at its
 * open coefficient passes x through unchanged, and so on. Up to 0.9.4 one loop did everything, about 380
 * instructions a sample. */
enum { F_TONE = 1, F_SUB = 2, F_FB = 4, F_MOD = 8 };
typedef struct {
    const int32_t *inc;
    const int16_t *tab;
    int32_t dq, csh;
    int32_t dm, dg, dgg, da, o, sh_tri, sh_saw, fb, bk, mix, cpl, lift, g2, clip, cg, tc, sub, og;
    int32_t m, g, gg, av;                     /* where the ramps start */
    int32_t ow, dow;                          /* the sub's octave share, Q12 << 7, and its ramp a sample */
    const int32_t *sb;                        /* the sample layer this block, Q18 after stage 1's gain (zeros: none) */
    int32_t bw;                               /* the body's share before the clipper, Q11 (2048 all): the top of SAMPLE */
    const int32_t *dk;                        /* while a sample plays: the sub's share a sample, Q11 (0: bw all block) */
} blk_t;

static inline __attribute__((always_inline)) void run_block(voice_t *t, int16_t *out, const blk_t *k, const int F)
{
    const int32_t *const inc = k->inc;
    const int16_t *const tab = k->tab;
    const int32_t *const sb = k->sb;
    const int32_t bw = k->bw;
    const int32_t *const dk = k->dk;
    const int32_t dq = k->dq, csh = k->csh;
    const int32_t dm = k->dm, dg = k->dg, dgg = k->dgg, da = k->da, o = k->o, sh_tri = k->sh_tri, sh_saw = k->sh_saw;
    const int32_t fb = k->fb, bk = k->bk, mix = k->mix, cpl = k->cpl, lift = k->lift, g2 = k->g2, clip = k->clip;
    const int32_t cg = k->cg, tc = k->tc, sub = k->sub, og = k->og;
    uint32_t p = t->ph, ph = t->ph_h;
    int32_t ow = k->ow;
    const int32_t dow = k->dow;
    int32_t m = k->m, g = k->g, gg = k->gg, av = k->av, lp = t->hp, dc = t->dcs, tl = t->tail, y = 0;
    int32_t z1 = t->z1, z2 = t->z2, f1 = t->f1, f2 = t->f2, pre = 0;
    int rs = t->restart;
    for (int n = 0; n < OP1_BLOCK; n++) {
        int32_t i = inc[n] >> OCT_DOWN;
        if (F & F_MOD) i += o;
        if (i < 0) i = 0;
        if (F & F_MOD) {
            int32_t ic = i >> 12;                              /* sag: take off the part the pitch has fallen */
            if (ic > 32767) ic = 32767;                        /* (dq < 0: add the part it has risen) */
            i -= (ic * dq) >> 3;
        }
        uint32_t ib = (uint32_t)i >> 13;
        if (ib > 16383) ib = 16383;
        const uint32_t pp = p;
        p += (uint32_t)i + ib * (uint32_t)(m >> 8);           /* note + glide + pitch envelope */
        if (F & F_SUB) ph ^= p < pp;                           /* a cycle wrapped: the octave-down phase's top bit */
        m += dm;

        const uint32_t q = F & F_FB ? p + ((uint32_t)(((f1 + f2) >> 1) * fb) << 1) : p;   /* feedback: the phase the waves are read at */
        const int32_t kk = (int32_t)(q >> 19), fr = (int32_t)((q >> 4) & 0x7FFF);
        const int32_t s0 = op1_sin_q(tab, kk), s1 = op1_sin_q(tab, kk + 1);
        const int32_t sn = s0 + (((s1 - s0) * fr) >> 15);      /* sine, interpolated */
        int32_t tr = (int32_t)q >> 15;                         /* triangle, in phase with the sine */
        if (tr > 32768) tr = 65536 - tr; else if (tr < -32768) tr = -65536 - tr;
        int32_t x = sn + (((tr - sn) * sh_tri) >> 15);
        x += ((((int32_t)q >> 16) - x) * sh_saw) >> 15;        /* saw: rises through 0 at phase 0, like the others */

        /* From here the signal is Q18 (1.0 = 262144), eight times finer than the 16 bits it leaves in: the drive
         * amplifies a dying tail by up to 30 dB, and what the tail is rounded to here is the noise heard on it.
         * A product must stay inside 32 bits, so the gains give up the bits the signal takes (Q8, Q11, Q12). */
        /* the sub: a sine an octave down (half the phase, from p, not q: feedback does not bend it), crossfaded to the
         * note's own phase by ow, at the envelope's level */
        int32_t clean = 0;
        if (F & F_SUB) {
            int32_t sc = sn;
            if (F & F_FB) {
                const int32_t kc = (int32_t)(p >> 19), c0 = op1_sin_q(tab, kc);
                sc = c0 + (((op1_sin_q(tab, kc + 1) - c0) * (int32_t)((p >> 4) & 0x7FFF)) >> 15);
            }
            if (ow) {                                          /* (an exact 0 only when it stays 0: dow then too) */
                const uint32_t po = (ph << 31) | (p >> 1);
                const int32_t ko = (int32_t)(po >> 19), o0 = op1_sin_q(tab, ko);
                const int32_t so = o0 + (((op1_sin_q(tab, ko + 1) - o0) * (int32_t)((po >> 4) & 0x7FFF)) >> 15);
                sc += ((so - sc) * (ow >> 7)) >> 12;
            }
            ow += dow;
            clean = ((sc * (g >> 15)) >> 12) + ((sc * ((g >> 8) & 127)) >> 19);
            g += dg;
        }
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

        if (F & F_TONE) {                                      /* tone (open: both stages follow x exactly) */
            z1 += ((x - z1) * tc) >> 11;
            z2 += ((z1 - z2) * tc) >> 11;
            x = z2;
        } else pre = x;
        x = (x * bw) >> 11;                                    /* the top of SAMPLE fades the body out here, after its drive
                                                                * and tone, before the clipper (2048: exactly x) */
        x += sb[n];                                            /* the sample layer, driven and toned on its own */

        if (F & F_FB) {                                        /* feedback's tap: here, through a fixed soft clip, so */
            int32_t ft = (x * 7209) >> 12;                     /* that the clip knob does not also set how wild it is */
            ft = ft > Q18_LIM ? Q18_LIM : (ft < -Q18_LIM ? -Q18_LIM : ft);
            f2 = f1;
            f1 = (ft - cube18(ft)) >> 3;
        }
        x = (x * cg) >> csh;                                   /* the clipper, last */
        const int32_t xc = x > Q18_LIM ? Q18_LIM : (x < -Q18_LIM ? -Q18_LIM : x);
        const int32_t ys = xc - cube18(xc);
        const int32_t yh = x > Q18_ONE - 8 ? Q18_ONE - 8 : (x < 8 - Q18_ONE ? 8 - Q18_ONE : x);   /* hard: brick wall at +-1 */
        y = ys + (((yh - ys) * clip) >> 12);
        if (F & F_SUB) {                                       /* sub: after the clipper, so it stays a clean sine. It fades
                                                                * with the body (the top of SAMPLE) and steps aside while a
                                                                * sample plays, or SUB at full would leave none of it */
            const int32_t sw = (sub * (dk ? dk[n] : bw)) >> 11;
            y += ((clean - y) * sw) >> 11;
        }
        if (F & F_MOD) {
            y = (y * (av >> 10)) >> 12;                        /* attack: a fade of the level, not of the tone */
            av += da;
        }

        if (rs) { tl = (t->last_y - y) * 256; rs = 0; }        /* a new note carries on from the last sample: no click */
        y += tl >> 8;                                          /* and that offset fades in 1.5 ms */
        tl -= tl >> 7;
        y = y > Q18_ONE ? Q18_ONE : (y < -Q18_ONE ? -Q18_ONE : y);   /* over a loud tail the two add up past full scale:
                                                                * the product below would overflow and wrap (a click) */
        out[n] = op1_sat16((((y + 4) >> 3) * og) >> 15);       /* back to 16 bits */
    }
    if (!(F & F_TONE)) z1 = z2 = pre;                          /* where the open filter's stages are */
    t->restart = 0;
    t->ph = p;
    t->ph_h = (uint8_t)ph;
    t->hp = lp;
    t->z1 = z1;
    t->z2 = z2;
    t->f1 = f1;
    t->f2 = f2;
    t->dcs = dc;
    t->tail = tl;
    t->last_y = y;
}

/* the top of SAMPLE: up to SMP_SOLO the sample is laid on the body, through the drive's copy and the clipper; from there
 * to the end the body (and its sub) fades out between the drive and the clipper, the sample going on through its drive
 * and the clipper as before: at 100 % the sample alone, driven and clipped */
#define SMP_SOLO 24576                        /* 75 % */
static inline int32_t smp_solo(int32_t lv) { return lv <= SMP_SOLO ? 0 : (lv >= 32767 ? 32768 : ((lv - SMP_SOLO) * 32768) / (32767 - SMP_SOLO)); }
/* the sample layer's step for a phase increment (the key's pitch at the moment, as the OP-1 gives it), Q12 samples a
 * voice sample: what the sampler would take (inc x rate >> 38), x SMP TUNE's factor; up to 8 samples a voice sample */
static int32_t smp_step(int32_t inc, int32_t rate, int32_t stm)
{
    const int64_t base = ((int64_t)(uint32_t)inc * rate) >> 38;
    int64_t st = (base * stm) >> 16;
    return (int32_t)(st < 1 ? 1 : (st > (8 << 12) ? (8 << 12) : st));
}
static void ty_render(void *self, int16_t *out, const int16_t *params)
{
    const int v = op1_voice_index(self);
    calls++;
    int16_t rc[REC_N];
    rec_get(op1_current_slot(), rc);
    const int mode = rc[REC_MODE];
    if (v < 0 || v >= OP1_VOICES) { for (int n = 0; n < OP1_BLOCK; n++) out[n] = 0; return; }
    beat[v] = calls;
    if (v == owner) {                                          /* the owner hands over what only it can read */
        const int32_t *pi = OP1_PITCH_INC;
        for (int n = 0; n < OP1_BLOCK; n++) own_inc[n] = pi[n];
        own_vel = op1_voice_vel_gain(self); own_stamp = stamp[v]; own_v = (int8_t)v;
        if (rv >= 0 && rv != v) {                              /* the renderer still rendering? */
            if (beat[rv] == rv_seen) { if (++rv_stale >= 2) rv = -1; }
            else { rv_seen = beat[rv]; rv_stale = 0; }
        }
        if (rv < 0) { rv = (int8_t)v; rv_seen = beat[v]; rv_stale = 0; }
    }
    if (v != rv || own_v < 0) {                                /* every voice but the sound's is silent */
        for (int n = 0; n < OP1_BLOCK; n++) out[n] = 0;
        return;
    }
    voice_t *const t = &tv[0];
    const int32_t *inc = own_inc;
    int32_t env[4];
    t2_get(op1_current_slot(), env);
    const int32_t sus = env[T2_SUSTAIN] * 32768;               /* Q30 */
    const int32_t glide = mode == MODE_GLIDE ? rc[REC_GLIDE] : 0;   /* only glide mode glides */
    /* sag, signed: the fall (word 3) or, with none, minus the rise (the record) */
    const int32_t sag = q15(params[3]) ? q15(params[3]) : -rc[REC_RISE];
    const int32_t atk_ms = attack_ms(env[T2_ATTACK]);
    const int gate = held[own_v];                              /* the owner's key */

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
    const uint32_t kp = 19017u * exp2neg((uint32_t)(q15(params[7]) * 25514) >> 15) >> 16;   /* 20 ms .. 1.5 s */
    const uint32_t ka = decay_k(env[T2_DECAY]);                                             /* 0.1 .. 4 s   */
    /* release, after the key is up: 10 ms .. 4 s on the same law as the decay (400 : 1, 8.64 octaves); HOLD keeps the
     * decay's rate */
    const uint32_t kr = env[T2_RELEASE] >= REL_HOLD ? ka
                      : 47550u * exp2neg((uint32_t)((env[T2_RELEASE] * 32767 / REL_HOLD) * 35407) >> 15) >> 16;
    const int32_t dk = q15(params[5]);
    /* clip: soft .. hard. It sets the knee of stage 2 and of the last clipper, and how hard the last one is driven:
     * 1.76x at soft (a plain sine just reaches the ceiling, rounded) up to 4x at three quarters (the same sine nearly
     * square), then the last quarter slams it, up to 16x (+12 dB more) at 100 %: a tail flattened to a square to the end */
    const int32_t clip = (q15(params[6]) + 7) >> 3;            /* Q12 */
    const int32_t slam = clip > 3072 ? clip - 3072 : 0;        /* the last quarter, 0 .. 1024 */
    const int32_t cg = 1802 + ((2294 * clip) >> 12) + ((slam * slam * 12) >> 10);   /* Q10, 1.76 .. 16x */

    /* a new note for this voice */
    if (own_stamp != t->seen_stamp) {
        t->seen_stamp = own_stamp;
        const int32_t slide = glide && t->last_inc ? t->last_inc - (inc[0] >> OCT_DOWN) : 0;   /* from the last pitch */
        if (mode == MODE_GLIDE && legato_req[own_v] && t->live && t->lvl > AUDIBLE) {
            t->off = slide;                                    /* legato: no restart, the pitch slides */
        } else {                                               /* restart: phase 0, both envelopes from the top */
            t->restart = t->live ? 2 : 1;
            if (!t->live) { t->d_s = dk << 8; t->hp = 0; t->last_y = 0; t->z1 = t->z2 = 0; t->f1 = t->f2 = 0; }   /* from silence: every hit the same */
            t->sag = ONE29;
            t->atk = atk_ms ? 0 : ONE30;                       /* attack: the sound fades in from nothing */
            t->fade = 32768; t->pk = Q18_ONE; t->pkacc = 0; t->pkn = 0;
            t->ph = 0; t->ph_h = 0;
            t->lvl = ONE30;
            t->pe = ONE30;
            t->off = 0;
            t->vel = own_vel;
            t->sd = 0;                                         /* the sample layer, from its start */
            t->shad = 0;
            const src_t *sr = &srcb[src_i];
            if (rc[REC_SLVL] && sr->ok) {
                /* the step the sampler would take for this key (key follow: the key's own pitch, not TYR8's two
                 * octaves down), then SMP TUNE: x 2^(tune / 12) = 4 x 2^-((24 - tune) / 12) */
                const int32_t tu = rc[REC_STUNE];
                t->srate = sr->rate;
                t->stm = (int32_t)(exp2neg((uint32_t)((24 - tu) * 4096) / 12) << 2);
                const int32_t st = smp_step(own_inc[OP1_BLOCK / 2], t->srate, t->stm);
                t->sstamp = own_stamp;
                const int back = sr->rev;                      /* the sampler's own direction */
                t->sd = sr->d; t->slo = sr->on; t->shi = sr->end; t->sg = sr->gain;
                t->sp = back ? (sr->end - 1) << 12 : sr->on << 12;
                t->sdc = t->slp = t->sz1 = t->sz2 = 0; t->sfo = 0; t->sh1 = t->sh2 = 0;
                const int32_t au = rc[REC_SATK], ams = (((au * au) >> 15) * 100) >> 15;   /* 0 .. 100 ms, a square law */
                t->sra = ams ? 0 : 32767 << 8;
                t->sri = ams ? (32767 << 8) / (ams * 88 + 1) : 0;
                t->sst = back ? -(int32_t)st : (int32_t)st;
                t->shad = 1; t->sws = smp_solo(rc[REC_SLVL]); t->sde = 0;
            }
            vis_trig++;
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
    t->lvl += (int32_t)(((int64_t)(target - t->lvl) * (gate ? ka : kr)) >> 16);
    if (target == 0 && t->lvl < SILENT) t->lvl = 0;
    /* attack: a linear fade-in of the finished sound, 0 .. 1 s. After the clipper, not before the drive, where the
     * gain would turn any ramp into a few milliseconds */
    const int32_t a0 = t->atk >> 8;                            /* Q22 */
    if (t->atk < ONE30) {                                      /* one block is 1.451 ms */
        const uint32_t st = 1557984375u / (uint32_t)(atk_ms ? atk_ms : 1);   /* ONE30 * 1.451 / ms */
        t->atk = st >= (uint32_t)(ONE30 - t->atk) ? ONE30 : t->atk + (int32_t)st;
    }
    /* the tail's last fade. Below about -51 dBFS the 16-bit output has only a few levels left for the wave, and a dying
     * tail is a coarse staircase there: harmless at a normal level, grit when something after the OP-1's synth (a
     * crusher, 40 dB of gain) brings it up. So once the note is on its way out (key up, or sustain 0) and its output
     * has fallen below FADE_LSB, it fades to exact silence with a 15 ms time constant, instead of spending half a
     * second in that staircase. A held note at its sustain level is never faded */
    const int32_t f0 = t->fade;
    {
        const int32_t ay = t->last_y < 0 ? -t->last_y : t->last_y;
        if (ay > t->pkacc) t->pkacc = ay;
        if (++t->pkn >= 16) { t->pk = t->pkacc; t->pkacc = 0; t->pkn = 0; }   /* 23 ms: half a cycle of the lowest note */
        const int32_t og_ = (t->vel * OUT_GAIN) >> 15;
        const int32_t thr = og_ > 0 ? (int32_t)((int64_t)FADE_LSB * 8 * 32768 / og_) : Q18_ONE;   /* in y's units */
        if (t->fade < 32768 || (target == 0 && t->pk < thr && t->live)) {
            t->fade -= (int32_t)(((int64_t)t->fade * 6340) >> 16);   /* 1.451 ms / 15 ms a block */
            if (t->fade < 64) { t->fade = 0; t->lvl = 0; }
        }
    }
    const int32_t a0f = (int32_t)(((int64_t)a0 * f0) >> 15), a1f = (int32_t)(((int64_t)(t->atk >> 8) * t->fade) >> 15);
    const int32_t da = (a1f - a0f) >> 7;
    const int32_t dg = (t->lvl - g0) >> 7;

    /* sag: the pitch falls (or, below 0, rises) steadily while the note lasts, over the amp decay's length by
     * 0.75 (u + u^2) octaves, u = 0.7 sag falling, 0.5 |sag| rising: 25, 50, 75, 100 % fall about 1.9, 4.2, 7.2 and
     * 10.7 semitones, -100 % rises 6.75 (0.9.12 had two octaves, linear: most of it was over by a third of the turn;
     * 0.9.13 .. 0.9.17 the same law without the 0.7 and 0.5, of which the owner used -50 .. 70 %). It stops three octaves down, or one
     * octave up. The decay runs at ka a block with a time constant a fifth of its length, so over that length a factor
     * (1 -+ ks) a block comes to 2^-+(2 e) with ks = 2 ln 2 / 5 * e * ka, e = 0.375 (u + u^2). */
    const int32_t dq = (ONE29 - t->sag) >> 14;                 /* how far it has fallen, Q15 (risen: < 0) */
    if (sag) {
        /* the knob's ends are 0.9.15's 70 % fall and 50 % rise, the ranges the owner found useful */
        const uint32_t u = sag > 0 ? (uint32_t)(sag * 7 / 10) : (uint32_t)(-sag) / 2;
        const uint32_t e = (u * 12288u + ((u * u) >> 15) * 12288u) >> 15;
        const uint32_t ks = (uint32_t)(((uint64_t)ka * 18170u * e) >> 27);   /* Q20 */
        const int32_t step = (int32_t)(((int64_t)t->sag * ks) >> 20);
        if (sag > 0) { t->sag -= step; if (t->sag < ONE29 >> 3) t->sag = ONE29 >> 3; }
        else         { t->sag += step; if (t->sag > ONE30) t->sag = ONE30; }
    }

    if (glide) {                                               /* 0 .. 500 ms: the slide's time constant is a quarter */
        const uint32_t kg = 24850302u / (uint32_t)glide;       /* 65536 * 1.451 ms * 4 / (glide * 500 ms / 32767) */
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
    /* the sample layer, this block: the recording at the sampler's pitch for the key and SMP TUNE, its way round,
     * linearly interpolated, faded out over its last 512 samples (no click where it is cut), x the
     * sampler's gain and clipped as the sampler does, then x SAMPLE (a square law); then the drive's two stages as the note has them (no bias: that
     * follows the note's envelope), with states of its own, its own tone, S.TONE, and SMP ATK's fade-in. Not through the amp envelope:
     * it plays whole, whatever the decay. run_block adds it after the note's tone */
    static const int32_t no_smp[OP1_BLOCK];
    static int32_t smp[OP1_BLOCK], sdk[OP1_BLOCK];
    const int32_t *dkb = 0;
    const int32_t *sbuf = no_smp;
    /* the top of SAMPLE (smp_solo), ramped a little a block so a turn doesn't step the body's level */
    const int32_t ws_want = t->shad ? smp_solo(rc[REC_SLVL]) : 0;
    t->sws += (ws_want - t->sws) >> 2;
    if ((ws_want - t->sws) >> 2 == 0) t->sws = ws_want;
    const int32_t ws = t->sws;
    if (t->sd && rc[REC_SLVL]) {
        const int32_t lv = rc[REC_SLVL] < SMP_SOLO ? rc[REC_SLVL] : SMP_SOLO;
        const int32_t lg = (lv * lv) >> 14;                    /* Q15, 1.125 at the top of the layering */
        const int32_t sgn = t->sg, bwb = (32768 - ws) >> 4;
        int32_t de = t->sde;
        const int32_t sg = (lg * g1) >> 15;                    /* Q8, up to 2 x 8 */
        const int16_t *const sdat = t->sd;
        int32_t pos = t->sp, sdc = t->sdc, slp = t->slp, z1 = t->sz1, z2 = t->sz2, ra = t->sra, fo = t->sfo;
        int32_t h1 = t->sh1, h2s = t->sh2;
        const int32_t hc = hpf_c(rc[REC_SHP]);
        /* the step follows the key's pitch, block by block, as the sampler's voice does: a new note's pitch can still
         * be on its way from the last one in its first blocks (portamento, the OP-1's smoothing). Not a legato note's: the
         * sample of the note before plays on at its pitch */
        if (own_stamp == t->sstamp) { const int32_t s = smp_step(inc[OP1_BLOCK / 2], t->srate, t->stm); t->sst = t->sst < 0 ? -s : s; }
        const int32_t st = t->sst, lo = t->slo << 12, hi = t->shi << 12, ri = t->sri;
        const int32_t stc = tone_tc(rc[REC_STONE]);
        for (int n = 0; n < OP1_BLOCK; n++) {
            int32_t x = 0;
            if (pos >= lo && pos < hi) {
                const int32_t i = pos >> 12, a = sdat[i], bb = sdat[i + 1];
                int32_t sv = a + (((bb - a) * (pos & 0xFFF)) >> 12);
                const int32_t rem = st > 0 ? t->shi - i : i - t->slo + 1;   /* to where it stops */
                if (rem < 512) sv = (sv * rem) >> 9;
                sv = (sv * sgn) >> 11;                         /* the sampler's gain, and its clip: what it plays */
                sv = sv > 32767 ? 32767 : (sv < -32767 ? -32767 : sv);
                if (hc) {                                      /* SMP HPF: two one-pole high-passes, before the drive */
                    h1 += (((sv * 256 - h1) >> 8) * hc) >> 7;
                    const int32_t s1 = sv - (h1 >> 8);
                    h2s += (((s1 * 256 - h2s) >> 8) * hc) >> 7;
                    sv = s1 - (h2s >> 8);
                }
                x = (sv * sg) >> 5;                            /* Q15 x Q8 -> Q18, stage 1's gain in */
                pos += st;
            } else if (fo < 2048) fo++;
            x = x > Q18_LIM ? Q18_LIM : (x < -Q18_LIM ? -Q18_LIM : x);     /* stage 1 .. stage 2, as run_block */
            x -= (cube18(x) * mix) >> 12;
            sdc += (x * 256 - sdc) >> 11;
            x -= ((sdc >> 8) * cpl) >> 12;
            slp += (x * 256 - slp) >> 6;
            x += ((x - (slp >> 8)) * lift) >> 8;
            x = x > 655360 ? 655360 : (x < -655360 ? -655360 : x);
            x = (x * g2) >> 8;
            x = x > Q18_LIM ? Q18_LIM : (x < -Q18_LIM ? -Q18_LIM : x);
            const int32_t h2 = x > Q18_ONE ? Q18_ONE : (x < -Q18_ONE ? -Q18_ONE : x);
            x -= cube18(x);
            x += ((h2 - x) * clip) >> 12;
            z1 += ((x - z1) * stc) >> 11;                      /* S.TONE (open: exactly x) */
            z2 += ((z1 - z2) * stc) >> 11;
            int32_t y = z2;
            if (ra < (32767 << 8)) {                           /* SMP ATK: after the drive, so the drive can't push the faded
                                                                * start back up; Q12 steps, no 64-bit product */
                y = ((y >> 2) * (ra >> 11)) >> 10;
                ra += ri; if (ra > (32767 << 8)) ra = 32767 << 8;
            }
            if (fo > 1024) y = (y * (2048 - fo)) >> 10;        /* after the end: 1024 samples to ring out, 1024 to fade */
            smp[n] = y;
            const int32_t ay = y < 0 ? -y : y;                 /* the sample's level, followed: at once up, 5.8 ms down */
            if (ay > de) de = ay; else de -= de >> 9;
            const int32_t duck = de >= (Q18_ONE >> 3) ? 2048 : de >> 4;   /* all of the sub out from an eighth of full scale */
            sdk[n] = (bwb * (2048 - duck)) >> 11;
        }
        t->sp = pos; t->sdc = sdc; t->slp = slp; t->sz1 = z1; t->sz2 = z2; t->sra = ra; t->sfo = fo; t->sh1 = h1; t->sh2 = h2s;
        /* over once the recording has played and its drive and tone have rung out and faded (the coupling's rounding
         * can hold a few LSBs for ever: they fade with it, no step) */
        if (fo >= 2048) t->sd = 0;
        sbuf = smp; t->sde = de; dkb = sdk;
    } else t->sd = 0;
    const int32_t og = (t->vel * OUT_GAIN) >> 15;
    /* tone, two one-pole low-passes in a row (12 dB / octave) from 12 kHz down to 190 Hz, exactly out of the way when
     * open; sub, a plain sine at the envelope's level faded in under the driven signal (up to three quarters), past
     * the drive and the tone; then the clipper, driven at least 1.76x so that a plain sine with no drive just reaches
     * its ceiling: whatever the settings, the output is at full scale, and a closed tone is a dark wall, not a quiet one */
    const int32_t tc = tone_tc(32767 - q15(params[2]));        /* Q11; tone: the knob is how open it is */
    int32_t ow0;
#ifndef SUB_MAX
#define SUB_MAX 2048                          /* all of it at full: only the clean sine (0.9.7 .. 0.9.13: 60 %) */
#endif
    /* the sub an octave down while that is above the synth bus's 35 Hz high-pass: from the note's pitch now (glide and
     * sag, not the knock), the octave in fully at 45 Hz and up, the note's own pitch at 40 Hz and under, a crossfade
     * between. 45 Hz an octave down is a phase increment of 2 x 45 x 2^32 / 88200 */
    {
        int64_t ni = (int64_t)(inc[0] >> OCT_DOWN) + t->off;
        ni = (ni * t->sag) >> 29;
        const int32_t lo = 3895640, hi = 4382595;              /* 40 and 45 Hz, octave down */
        const int32_t want = ni <= lo ? 0 : (ni >= hi ? 4096 : (int32_t)(((ni - lo) * 4096) / (hi - lo)));
        ow0 = t->ow;
        if (t->restart) ow0 = t->ow = want;
        else t->ow += (want - t->ow) >> 2;                     /* a glide or a sag through it: ramped, no step */
    }
    const int32_t sub = (rc[REC_SUB] * SUB_MAX) >> 15;         /* Q11: the clean sine's share after the clipper */

    /* ---- per sample: the loop built for what this block uses (see run_block) ---- */
    blk_t k;
    k.inc = inc; k.tab = op1_sin_tab(); k.sb = sbuf; k.bw = (32768 - ws) >> 4; k.dk = dkb;
    k.dm = dm; k.dg = dg; k.da = da; k.o = t->off; k.dq = dq;
    k.sh_tri = sh_tri; k.sh_saw = sh_saw; k.fb = fb; k.bk = bk; k.mix = mix; k.cpl = cpl; k.lift = lift; k.g2 = g2;
    k.clip = clip; k.cg = cg > 4096 ? cg >> 2 : cg; k.csh = cg > 4096 ? 8 : 10;   /* x cg stays in 32 bits */
    k.tc = tc; k.sub = sub; k.og = og; k.ow = ow0 << 7; k.dow = t->ow - ow0;   /* from last block's share to this one's over the block */
    k.gg = (int32_t)(((int64_t)g0 * g1) >> 11);               /* envelope x stage 1's gain, Q27 (up to 8.0) */
    k.dgg = ((int32_t)(((int64_t)t->lvl * g1) >> 11) - k.gg) >> 7;
    k.m = m0; k.g = g0; k.av = a0f;
    const int f = (tc != 2048 ? F_TONE : 0) | (sub ? F_SUB : 0) | (fb ? F_FB : 0)
                | (k.o || dq || a0f != (ONE30 >> 8) || da ? F_MOD : 0);
    switch (f) {
    case 0:  run_block(t, out, &k, 0);  break;
    case 1:  run_block(t, out, &k, 1);  break;
    case 2:  run_block(t, out, &k, 2);  break;
    case 3:  run_block(t, out, &k, 3);  break;
    case 4:  run_block(t, out, &k, 4);  break;
    case 5:  run_block(t, out, &k, 5);  break;
    case 6:  run_block(t, out, &k, 6);  break;
    case 7:  run_block(t, out, &k, 7);  break;
    case 8:  run_block(t, out, &k, 8);  break;
    case 9:  run_block(t, out, &k, 9);  break;
    case 10: run_block(t, out, &k, 10); break;
    case 11: run_block(t, out, &k, 11); break;
    case 12: run_block(t, out, &k, 12); break;
    case 13: run_block(t, out, &k, 13); break;
    case 14: run_block(t, out, &k, 14); break;
    default: run_block(t, out, &k, 15); break;
    }
    const int32_t o = k.o;
    t->last_inc = (inc[OP1_BLOCK - 1] >> OCT_DOWN) + o;
    if (!t->sd && ((ws >= 32768 && (t->tail >> 11) == 0) || (t->lvl == 0 && (t->tail >> 11) == 0)))
        t->live = 0;                                           /* (at SAMPLE 100 % the body is silent: done with the sample) */

    vis_inc = (uint32_t)t->last_inc;                           /* the screen follows the sound */
    vis_lvl = t->lvl >> 15;
}

/* the engine's T1 page (KeyHandler thread). A plain turn is the OP-1's (words 0..3). Every Shift + turn is taken
 * here, at the core's fine Shift rate (a quarter of R246's delta: 256 a detent, faster when turned fast): blue drive
 * or clip, green knock length (words, kept with op1_engine_word_set), white sub and orange glide (the record). Pushes:
 * orange steps the play mode, Shift + blue switches drive / clip, the others do nothing. And the plain orange turn,
 * sag, which goes below 0 */
extern const op1_engine_ext2_t ENGINE_tyr8;
static int32_t tune_acc;                      /* the sample tune's part detent */
static int ty_input(int page, int ev, int a, int b)
{
    if (a < 0 || a > 3) return 0;
    const int slot = op1_current_slot();
    int16_t r[REC_N];
    if (page == OP1_ENG_T2) {                                  /* T2: the turns are R246's (the envelope words); */
        if (ev != OP1_ENG_PUSH || (a != 1 && a != 3)) return 0;   /* a green push steps the sample's source, */
        if (b && a == 3) {                                     /* an orange push the play mode (shown under RELEASE) */
            rec_get(slot, r);
            r[REC_MODE] = (int16_t)((r[REC_MODE] + 1) % MODE_N);
            rec_put(slot, r);
        } else if (b) {
            rec_get(slot, r);
            int sv = r[REC_SRC] + 1;                           /* AUTO, 1 .. 8 (not this slot), AUTO */
            if (sv == slot + 1) sv++;
            if (sv > 8) sv = SRC_AUTO;
            r[REC_SRC] = (int16_t)sv;
            rec_put(slot, r);
            src_force = 1;
        }
        return 1;
    }
    if (page != OP1_ENG_T1) return 0;
    if (ev == OP1_ENG_PUSH) {
        if (b && a == 3 && op1_shift_held()) tyre_badge ^= 1;   /* Shift + orange: the sidewall's markings (an easter egg) */
        else if (b && a == 0) blue_pg ^= 1;                  /* blue: SHAPE + CLIP <-> DRIVE + SMP TUNE */
        else if (b && a == 1) { green_smp ^= 1; src_force = 1; }   /* green (with or without Shift): knock <-> the sample layer */
        else if (b && a == 2) white_smp ^= 1;                  /* white: tone and sub <-> the sample's tone and fade */
        return 1;
    }
    if (ev != OP1_ENG_TURN) return 0;
    if (a == 0 && blue_pg) {                                   /* blue's DRIVE page: drive, Shift: SMP TUNE */
        if (op1_shift_held()) {
            rec_get(slot, r);
            int32_t v = r[REC_STUNE] + op1_eng_detents(&tune_acc, b);   /* a semitone a detent */
            v = v < -24 ? -24 : (v > 24 ? 24 : v);
            if (v != r[REC_STUNE]) { r[REC_STUNE] = (int16_t)v; rec_put(slot, r); }
        } else {
            const int32_t was = q15(op1_synth_knobs(slot)[5]);
            int32_t v = was + b;                               /* a plain turn's rate, as the OP-1 turns a word */
            v = v < 0 ? 0 : (v > 32767 ? 32767 : v);
            if (v != was) op1_engine_word_set(&ENGINE_tyr8.ext.voice, 5, v);
        }
        return 1;
    }
    if (a == 2 && white_smp) {                                 /* white on the sample: S.TONE, Shift: SMP HPF */
        rec_get(slot, r);
        const int at = op1_shift_held() ? REC_SHP : REC_STONE;
        int32_t v = at == REC_SHP ? r[at] + b / 4 : r[at] - b; /* S.TONE as TONE turns: right opens it; SMP HPF at the
                                                                * Shift rate */
        v = v < 0 ? 0 : (v > 32767 ? 32767 : v);
        if (v != r[at]) { r[at] = (int16_t)v; rec_put(slot, r); }
        return 1;
    }
    if (a == 1 && green_smp) {                                 /* green on the sample layer: level, Shift: SMP ATK */
        rec_get(slot, r);
        if (op1_shift_held()) {
            int32_t v = r[REC_SATK] + b;                       /* full speed, 32 detents (a quarter speed took 10 to pass 1 ms) */
            v = v < 0 ? 0 : (v > 32767 ? 32767 : v);
            if (v == r[REC_SATK]) return 1;
            r[REC_SATK] = (int16_t)v;
        } else {
            int32_t v = r[REC_SLVL] + b;                       /* as the OP-1 turns a word: 32 detents */
            v = v < 0 ? 0 : (v > 32767 ? 32767 : v);
            if (v == r[REC_SLVL]) return 1;
            if (!r[REC_SLVL]) src_force = 1;                   /* just on: look for the source at the next note */
            r[REC_SLVL] = (int16_t)v;
        }
        rec_put(slot, r);
        return 1;
    }
    if (!op1_shift_held()) {
        if (a != 3) return 0;
        /* orange, sag: -100 .. 100 %, a detent 1024 as the OP-1 turns a word. Up from 0 is the fall (word 3), down
         * the rise (the record); one of them is 0 */
        rec_get(slot, r);
        const int32_t fall = q15(op1_synth_knobs(slot)[3]), was = fall ? fall : -r[REC_RISE];
        int32_t v = was + b;
        v = v < -32767 ? -32767 : (v > 32767 ? 32767 : v);
        if (v == was) return 1;
        const int32_t nf = v > 0 ? v : 0, nr = v < 0 ? -v : 0;
        if (nf != fall) op1_engine_word_set(&ENGINE_tyr8.ext.voice, 3, nf);
        if (nr != r[REC_RISE]) { r[REC_RISE] = (int16_t)nr; rec_put(slot, r); }
        return 1;
    }
    const int st = b / 4;
    if (a < 2) {
        const int k = a ? 7 : 6;                               /* Shift + blue on the SHAPE page: clip */
        const int32_t was = q15(op1_synth_knobs(slot)[k]);
        int32_t v = was + st;
        v = v < 0 ? 0 : (v > 32767 ? 32767 : v);
        if (v != was) op1_engine_word_set(&ENGINE_tyr8.ext.voice, k, v);
    } else {
        rec_get(slot, r);
        const int at = a == 2 ? REC_SUB : REC_GLIDE;
        int32_t v = r[at] + st;
        v = v < 0 ? 0 : (v > 32767 ? 32767 : v);
        if (v != r[at]) { r[at] = (int16_t)v; rec_put(slot, r); }
    }
    return 1;
}

/* own envelope (finished), the T2 page and the input hook: the 40-byte op1_engine_ext2_t (plugin.json "own_envelope",
 * "t2_page", "engine_input"); the fields of the hooks it does not use stay 0 */
extern const op1_screen_t SCREEN_tyr8_t2;
const op1_engine_ext2_t ENGINE_tyr8 = {
    { { ty_note_on, ty_render, ty_note_off, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } },
      ty_finished, 0, &SCREEN_tyr8_t2 },
    ty_input, 0, 0,
};

/* ======================= screen (GUI thread) =======================
 * A tyre rolling along a road, all drawn here (tyr8.svg is empty): lines with op1_raster_line, the black rubber as
 * filled polygons written into the raster's pixels (fed to the frame-skip hash). Painter's order: road, the tyre's
 * whole silhouette in black, the tread band and its lugs (back to front), the near face in black over them, then the
 * near face's lines, the rim, the labels.
 *
 * One projection for everything, dimetric (2 : 1), in screen px:
 *   travel (along the road, the wheel rolls up and to the right)   TRV = ( 0.894, -0.447)
 *   up                                                              ( 0,     -1    )
 *   depth (from the wheel's near face back across the road)         BCK = (-0.67,  -0.335)
 * so the road runs diagonally, and the wheel's face, in the travel / up plane, is a tilted ellipse. A face point at
 * (u along travel, v up) and depth p sits at hub + u TRV + (0, -v) + p BCK. Seen from (-0.749, 0.67, 1) (travel, up,
 * toward the viewer): the tread is visible where its outward normal (cos t, sin t) gives -0.749 cos t + 0.67 sin t > 0,
 * the upper left of the band.
 *
 *   size     amp decay (T2 green): small car tyre .. monster truck wheel
 *   lugs     drive or clip (words 5, 6), whichever is higher: at 0 a plain racing slick, then the staggered blocks
 *            rise from a grooved grid to tall off-road lugs: the even ones on the near half of the tread, the odd ones
 *            on the far half
 *   flat     sag (orange, word 3): the bottom flattens onto the road and the sidewall bulges; the rim stays round
 *   spokes   sub (Shift + white): a plain dish with five nuts .. laced lowrider wire spokes (up to 40), then a
 *            whitewall with an orange pinstripe (from half way), then a gold knock-off spinner (from 80 %)
 *   spin     the sounding pitch, while a note sounds; it coasts down after, and the road scrolls under it
 *   street   a road along the whole diagonal (a dashed centre line), a curb, a sidewalk and grass behind it, all
 *            scrolling, their lines cut around the label blocks
 *   bounce   every restarted note kicks the wheel off the road, harder with the pitch envelope amount (green), and
 *            its shadow stays on the road. The amount also makes the road rough: rocks in the wheel's track, which it
 *            hops over, cracks in the pavement, and from 40 % weeds in the cracks
 *   labels   one block a corner, each encoder in its colour (blue and green on top, white and orange below): name,
 *            value and a gauge; while Shift is held the Shift set, blue's drive or clip as switched. The play mode is
 *            under orange's block */
#define LBL_W     94                          /* width of a corner's label block, px: "KNCKLEN 999MS" and a gap */
#define LBL_S     1229                        /* label text scale, Q10: 1.2, 7.2 px a character */
#define LBL_YB_T1 130                         /* the lower blocks' text, T1: in the corners, 8 px off the bottom as the top's */
#define LBL_YB_T2 106                         /* and T2: up a line, for the play mode under orange's block */
#define TRV_X     916                         /* the projection, Q10 */
#define TRV_Y     (-458)
#define BCK_X     (-686)
#define BCK_Y     (-343)
#define CON_X     176                         /* where the near face meets the road, px */
#define CON_Y     126
#define R_C       46                          /* full size: carcass radius, px */
#define LUG_H     9                           /* full lug height */
#define TYRE_W    48                          /* tread width (depth units) */
#define R_RIM     27
#define R_DISH    21
#define R_HUB     6
#define LUG_N     16                          /* blocks around; the near face shows every other one */
#define ROAD_LEN  300                         /* the road along travel, each way from the contact point, px (clipped) */
#define ROAD_P0   (-22)                       /* and across, in depth: in front of the wheel .. behind it */
#define ROAD_P1   90                          /* the gutter: the curb starts here */
#define KERB_H    3                           /* the curb and the sidewalk, px up */
#define WALK_W    24                          /* the sidewalk, deep */
#define SLAB_PX   18                          /* its joints */
#define GRASS_S   10                          /* the grass grid: along travel, across */
#define GRASS_P   9
#define GRASS_ROWS 8
#define GRASS_COLS 22
#define CRACK_PX  15                          /* where cracks can be */
#define TICK_PX   12                          /* where ridges can be */
#define DASH_PX   8                           /* the centre line's dashes */
#define RING_N    28                          /* segments of a drawn ring */
static uint32_t ang;                          /* wheel angle, 8192 << 8 a turn */
static uint32_t scroll;                       /* road travelled, Q10 px */
static int32_t  run;                          /* 0 .. 1024: how much of the pitch's speed the wheel has */
static int32_t  by, bv;                       /* bounce: height (Q10 px, up is negative) and speed (Q10 px / s) */
static uint32_t seen_trig, seen_tick;
static const uint8_t COL_KNOB[4][4] = { { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF },
                                        { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
static const uint8_t COL_GREY[4]  = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t COL_DIM[4]   = { 0x45, 0x47, 0x4A, 0xFF };
static const uint8_t COL_BUMP[4]  = { 0xDF, 0xD9, 0xFF, 0xFF };
static const uint8_t COL_WHITE[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t COL_PIN[4]   = { 0xFF, 0x9A, 0x1F, 0xFF };   /* the whitewall's pinstripe */
static const uint8_t COL_GOLD[4]  = { 0xFF, 0xC8, 0x3C, 0xFF };

/* for the tests: what the last frame drew */
static op1_pt   sil[2 * (2 * LUG_N * 2 + 8)];   /* the tyre's silhouette (a convex hull), Q10 px */
static int      sil_n;
static int32_t  vis_hub_y;                    /* hub height, Q10 px */
static int      vis_spokes, vis_whitewall, vis_spinner;

static inline int32_t tsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t tcos(int32_t a) { return tsin(a + 2048); }

static void ty_screen_init(void *scr) { (void)scr; }

static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, 0x600, c);
}
static inline void lnp(void *r, op1_pt a, op1_pt b, const uint8_t *c) { ln(r, a.x, a.y, b.x, b.y, c); }

/* ---- filled polygons: panel pixels, even-odd, one span list a pixel row (as dx_gfx.c writes them) ---- */
static uint16_t *fb;
static int fb_rows;
static void *fb_scr;
#ifdef OP1_HOST
extern uint16_t host_frame565[400 * 256];
void host_fill_poly(const op1_pt *p, int n, const uint8_t *rgba);
void host_mark_dynamic(void);
#endif
static void fill_begin(void *scr, void *r)
{
#ifndef OP1_HOST
    fb = (uint16_t *)(uintptr_t)((const uint32_t *)r)[0];
    fb_rows = op1_panel_rows(r);
#else
    (void)r;
    fb = host_frame565;
    fb_rows = op1_panel_rows_of(host_screen_scale, 400);
#endif
    fb_scr = scr;
}
static void fill_poly(const op1_pt *p, int n, const uint8_t *rgba)
{
#ifdef OP1_HOST
    host_fill_poly(p, n, rgba);
#endif
    if (!fb || n < 3) return;
    const int rows = fb_rows, cols = rows / 2;
    const uint16_t v = (uint16_t)(((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) | (rgba[2] >> 3));
    int32_t y0 = p[0].y, y1 = p[0].y;
    uint32_t h = 0x9e3779b9u;
    for (int i = 0; i < n; i++) {
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
        h = (h ^ (uint32_t)p[i].x) * 16777619u ^ (uint32_t)p[i].y;
    }
    op1_screen_hash(fb_scr, h ^ v);
    int py0 = (int)(((int64_t)y0 * rows / 320 - 512) >> 10), py1 = (int)(((int64_t)y1 * rows / 320) >> 10) + 1;
    if (py0 < 0) py0 = 0;
    if (py1 > cols - 1) py1 = cols - 1;
    for (int py = py0; py <= py1; py++) {
        const int32_t ys = (int32_t)(((int64_t)(py * 1024 + 512) * 320) / rows);   /* the row's centre, screen Q10 */
        int32_t xs[48];
        int m = 0;
        for (int i = 0, j = n - 1; i < n && m < 48; j = i++) {
            const op1_pt a = p[j], b = p[i];
            if ((a.y <= ys) == (b.y <= ys)) continue;
            xs[m++] = a.x + (int32_t)(((int64_t)(ys - a.y) * (b.x - a.x)) / (b.y - a.y));
        }
        for (int i = 1; i < m; i++) { const int32_t t = xs[i]; int j = i; while (j > 0 && xs[j - 1] > t) { xs[j] = xs[j - 1]; j--; } xs[j] = t; }
        for (int i = 0; i + 1 < m; i += 2) {
            int px0 = (int)(((int64_t)xs[i] * rows / 320 + 511) >> 10), px1 = (int)(((int64_t)xs[i + 1] * rows / 320 - 512) >> 10);
            if (px0 < 0) px0 = 0;
            if (px1 > rows - 1) px1 = rows - 1;
            for (int px = px0; px <= px1; px++) fb[(rows - 1 - px) * 256 + py] = v;
        }
    }
}

/* ---- the wheel's geometry for this frame ---- */
typedef struct {
    int32_t hx, hy;                           /* hub, Q10 px */
    int32_t s;                                /* size, Q10 */
    int32_t rc, rt, rout;                     /* carcass, lug top, outer radius (the larger), Q10 px */
    int32_t wd;                               /* tread width, depth Q10 px */
    int32_t q;                                /* flattening: how far the bottom is pushed in, Q10 px */
    int32_t inf;                              /* over-inflation: how far the tread is blown out, Q10 px */
    int32_t bl, ba;                           /* and a blister on it, about to give: how far out (Q10 px), at angle ba */
    uint8_t strain;                           /* a red strain line round the sidewall */
    int32_t rrim;                             /* the rim: the flattening stops here */
} wheel_t;
static wheel_t last_wheel;                    /* for the tests */

/* a point of the wheel: angle a (8192 a turn, counterclockwise as seen from the near face), radius r and depth p
 * (Q10 px) -> screen. Tyre points (r beyond the rim) are flattened: whatever lies below the floor is pushed up onto
 * it, and the low sidewall bulges along the road, more the further out */
static op1_pt wp(const wheel_t *w, int32_t a, int32_t r, int32_t p)
{
    int32_t u = (int32_t)(((int64_t)r * tcos(a)) >> 15), v = (int32_t)(((int64_t)r * tsin(a)) >> 15);
    if (w->q > 0 && r > w->rrim) {
        const int32_t floor = -(w->rout - w->q);
        int32_t lo = (int32_t)(((int64_t)-v * 1024) / w->rout) - 102;           /* how low, Q10: 0 at 10 % down */
        lo = lo < 0 ? 0 : (lo > 922 ? 1024 : (lo * 1024) / 922);
        lo = (lo * lo) >> 10;
        int32_t out = (int32_t)(((int64_t)(r - w->rrim) * 1024) / (w->rout - w->rrim));   /* 0 at the rim .. 1 */
        if (out > 1024) out = 1024;
        const int32_t bulge = (int32_t)(((int64_t)w->q * 2458 / w->rout * lo >> 10) * out >> 10);   /* Q10, 2.4 q / R */
        u += (int32_t)(((int64_t)u * bulge) >> 10);
        if (v < floor) v = floor;
    }
    if (w->inf > 0 && r > w->rrim) {                           /* blown up: the tread further out, the sidewalls */
        int32_t out = (int32_t)(((int64_t)(r - w->rrim) * 1024) / (w->rout - w->rrim));   /* ballooning sideways */
        if (out > 1024) out = 1024;
        const int32_t f = (int32_t)(((int64_t)w->inf * out) / r);                         /* Q10: r grows by inf out */
        u += (int32_t)(((int64_t)u * f) >> 10);
        v += (int32_t)(((int64_t)v * f) >> 10);
        int32_t f2 = f;
        if (w->bl && v > -(r >> 2)) {                          /* (not where it would push into the road) */                                           /* the blister: a bump of the tread and wall at ba */
            int32_t d = (a - w->ba) & 8191;
            if (d > 4096) d -= 8192;
            if (d < 0) d = -d;
            if (d < 1100) {
                const int32_t kb = ((1100 - d) * 1024) / 1100, kk = (kb * kb) >> 10;
                const int32_t e = (int32_t)(((int64_t)w->bl * kk >> 10) * out / r);
                u += (int32_t)(((int64_t)u * e) >> 10);
                v += (int32_t)(((int64_t)v * e) >> 10);
                f2 += e;
            }
        }
        int32_t pb = (int32_t)(((int64_t)(w->inf + (int32_t)(((int64_t)(f2 - f) * r) >> 10)) * 7 / 2 * out >> 10)
                               * (1024 - out) >> 8);                                             /* most mid-wall */
        if (v < 0) pb = (int32_t)((int64_t)pb * (1024 + (int32_t)(((int64_t)v * 870) / r)) >> 10);   /* less low down, */
                                                                     /* where it would push the wall into the road */
        p += (int32_t)(((int64_t)(2 * p - w->wd) * pb) / w->wd);                        /* near face out, far face back */
    }
    op1_pt o;
    o.x = w->hx + ((u * TRV_X) >> 10) + ((p * BCK_X) >> 10);
    o.y = w->hy + ((u * TRV_Y) >> 10) - v + ((p * BCK_Y) >> 10);
    return o;
}

/* the outline of one face at depth p: the carcass, and the lugs that reach this face (even ones on the near face,
 * odd ones on the far) */
static int outline(const wheel_t *w, int32_t a0, int far, op1_pt *o)
{
    const int32_t pitch = 8192 / LUG_N, hw = pitch * 9 / 20, hwt = pitch * 7 / 20;   /* a block's half width: base, top */
    const int32_t p = far ? w->wd : 0;
    int n = 0;
    for (int i = far; i < LUG_N; i += 2) {
        const int32_t c = a0 + i * pitch;
        o[n++] = wp(w, c - hw, w->rc, p);
        o[n++] = wp(w, c - hwt, w->rt, p);
        o[n++] = wp(w, c + hwt, w->rt, p);
        o[n++] = wp(w, c + hw, w->rc, p);
        o[n++] = wp(w, c + pitch, w->rc, p);                   /* the carcass between, through the other row's block */
    }
    return n;
}

/* the far face's rim, only where it shows: where the tread next to it faces the viewer (the upper left). The rest of
 * it is behind the tread and the near face */
static void far_edge(void *r, const wheel_t *w, int32_t a0)
{
    const int32_t pitch = 8192 / LUG_N, hw = pitch * 9 / 20, hwt = pitch * 7 / 20;
    for (int i = 1; i < LUG_N; i += 2) {
        const int32_t c = a0 + i * pitch;
        const int32_t an[6] = { c - hw, c - hwt, c + hwt, c + hw, c + pitch, c + pitch + (pitch - hw) };
        const int32_t rr[6] = { w->rc, w->rt, w->rt, w->rc, w->rc, w->rc };
        op1_pt q0 = wp(w, an[0], rr[0], w->wd);
        for (int k = 1; k < 6; k++) {
            const op1_pt q1 = wp(w, an[k], rr[k], w->wd);
            const int32_t m = (an[k - 1] + an[k]) / 2;
            if (-24550 * (tcos(m) >> 4) + 21955 * (tsin(m) >> 4) > 0) lnp(r, q0, q1, COL_BUMP);
            q0 = q1;
        }
    }
}

static int64_t cross(op1_pt o, op1_pt a, op1_pt b) { return (int64_t)(a.x - o.x) * (b.y - o.y) - (int64_t)(a.y - o.y) * (b.x - o.x); }
/* convex hull (monotone chain) of n points, in place order lost; -> hull in h, its size */
static int hull(op1_pt *p, int n, op1_pt *h)
{
    for (int i = 1; i < n; i++) {                              /* sort by x, then y */
        const op1_pt t = p[i]; int j = i;
        while (j > 0 && (p[j - 1].x > t.x || (p[j - 1].x == t.x && p[j - 1].y > t.y))) { p[j] = p[j - 1]; j--; }
        p[j] = t;
    }
    int k = 0;
    for (int i = 0; i < n; i++) { while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) k--; h[k++] = p[i]; }
    for (int i = n - 2, t = k + 1; i >= 0; i--) { while (k >= t && cross(h[k - 2], h[k - 1], p[i]) <= 0) k--; h[k++] = p[i]; }
    return k - 1;
}

/* a ring of the near face at radius r (Q10 px), warped as the tyre is */
static void ring(void *r, const wheel_t *w, int32_t rr, int32_t p, const uint8_t *c)
{
    int n = 6 + (rr >> 11);                                    /* segments by size: 10 for the hub .. 24, under 0.4 px off */
    n = n < 10 ? 10 : (n > 24 ? 24 : n);
    op1_pt a = wp(w, 0, rr, p);
    for (int i = 1; i <= n; i++) { const op1_pt b = wp(w, 8192 * i / n, rr, p); lnp(r, a, b, c); a = b; }
}
static void poly_line(void *r, const op1_pt *p, int n, const uint8_t *c)
{
    for (int i = 0; i < n; i++) lnp(r, p[i], p[(i + 1) % n], c);
}

/* the visible faces of lug i (back to front by the caller): its top, the side that faces the viewer, and for a far-row
 * block its near end. Each filled black, then outlined */
/* inside any polygon (even-odd) */
static int in_poly(const op1_pt *p, int n, op1_pt q)
{
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((p[i].y > q.y) == (p[j].y > q.y)) continue;
        const int32_t xc = p[i].x + (int32_t)(((int64_t)(q.y - p[i].y) * (p[j].x - p[i].x)) / (p[j].y - p[i].y));
        if (q.x < xc) in = !in;
    }
    return in;
}
static const op1_pt *near_face;               /* the near face's outline: lug faces wholly inside it are covered */
static int near_n;
static int covered(const op1_pt *q)
{
    for (int i = 0; i < 4; i++) if (!in_poly(near_face, near_n, q[i])) return 0;
    return 1;
}
static void lug(void *r, const wheel_t *w, int32_t a0, int i)
{
    const int32_t pitch = 8192 / LUG_N, hw = pitch * 9 / 20, hwt = pitch * 7 / 20;
    const int32_t c = a0 + i * pitch;
    const int32_t p0 = i & 1 ? w->wd * 42 / 100 : 0, p1 = i & 1 ? w->wd : w->wd * 58 / 100;
    const int32_t vt = -24550 * (tcos(c) >> 4) + 21955 * (tsin(c) >> 4);   /* top: faces the viewer */
    op1_pt q[4];
    const int32_t ea = c - hw, eb = c + hw, ta = c - hwt, tb = c + hwt;
    /* side at ea: normal (sin, -cos) of ea; at eb: (-sin, cos) */
    const int32_t va = -24550 * (tsin(ea) >> 4) - 21955 * (tcos(ea) >> 4), vb = 24550 * (tsin(eb) >> 4) + 21955 * (tcos(eb) >> 4);
    if (w->rt > w->rc + 256) {
        if (va > 0) { q[0] = wp(w, ea, w->rc, p0); q[1] = wp(w, ta, w->rt, p0); q[2] = wp(w, ta, w->rt, p1); q[3] = wp(w, ea, w->rc, p1);
                      if (!covered(q)) { fill_poly(q, 4, COL_DIM); poly_line(r, q, 4, COL_BUMP); } }
        if (vb > 0) { q[0] = wp(w, eb, w->rc, p0); q[1] = wp(w, tb, w->rt, p0); q[2] = wp(w, tb, w->rt, p1); q[3] = wp(w, eb, w->rc, p1);
                      if (!covered(q)) { fill_poly(q, 4, COL_DIM); poly_line(r, q, 4, COL_BUMP); } }
        if (i & 1) { q[0] = wp(w, ea, w->rc, p0); q[1] = wp(w, ta, w->rt, p0); q[2] = wp(w, tb, w->rt, p0); q[3] = wp(w, eb, w->rc, p0);
                     if (!covered(q)) { fill_poly(q, 4, COL_DIM); poly_line(r, q, 4, COL_BUMP); } }
    }
    if (vt > 0) {
        q[0] = wp(w, ta, w->rt, p0); q[1] = wp(w, tb, w->rt, p0); q[2] = wp(w, tb, w->rt, p1); q[3] = wp(w, ta, w->rt, p1);
        if (!covered(q)) {
            fill_poly(q, 4, (const uint8_t[4]){ 0, 0, 0, 0xFF });
            poly_line(r, q, 4, COL_WHITE);
        }
    }
}

/* ridge height (px) of road line i at pitch envelope amount amt: a fixed pattern per line, so it scrolls */
static int32_t bump(uint32_t i, int32_t amt)
{
    const uint32_t h = (i * 2654435761u) >> 29;                /* 0 .. 7 */
    return h < 6 ? 0 : (int32_t)(((h - 5) * (uint32_t)amt * 3u) >> 15);   /* about one place in four, up to 6 px */
}
/* a road point: s along travel, p across (Q10 px), from the contact point */
static inline op1_pt gp(int32_t s, int32_t p)
{
    op1_pt o = { OP1_Q10(CON_X) + ((s * TRV_X) >> 10) + ((p * BCK_X) >> 10), OP1_Q10(CON_Y) + ((s * TRV_Y) >> 10) + ((p * BCK_Y) >> 10) };
    return o;
}

/* ---- road lines: clipped to the screen and cut around the four label blocks, so the road can run the whole
 * diagonal and fill the space between them ---- */
static const int16_t KEEP_OUT[4][4] = {        /* x0, y0, x1, y1 px: the label blocks, and 3 px more toward the scene, so
                                                * that no moving stroke reaches the still label layer */
    { 0, 0, 108, 24 }, { 214, 0, 320, 24 }, { 0, 125, 108, 147 }, { 214, 125, 320, 147 } };
#define ROAD_SEG_MAX 1024
static op1_pt road_seg[ROAD_SEG_MAX][2];      /* for the tests: the road's pieces as last drawn */
static int    road_nseg;
/* the part [t0, t1] (Q16) of a -> b inside the box, if any (Liang-Barsky) */
static int box_in(op1_pt a, op1_pt b, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t *t0, int32_t *t1)
{
    const int32_t dx = b.x - a.x, dy = b.y - a.y;
    const int32_t pp[4] = { -dx, dx, -dy, dy }, qq[4] = { a.x - x0, x1 - a.x, a.y - y0, y1 - a.y };
    int64_t lo = 0, hi = 65536;
    for (int i = 0; i < 4; i++) {
        if (pp[i] == 0) { if (qq[i] < 0) return 0; continue; }
        const int64_t t = ((int64_t)qq[i] * 65536) / pp[i];
        if (pp[i] < 0) { if (t > lo) lo = t; } else if (t < hi) hi = t;
        if (lo > hi) return 0;
    }
    *t0 = (int32_t)lo; *t1 = (int32_t)hi;
    return 1;
}
static op1_pt lerp(op1_pt a, op1_pt b, int32_t t)
{
    op1_pt o = { a.x + (int32_t)(((int64_t)(b.x - a.x) * t) >> 16), a.y + (int32_t)(((int64_t)(b.y - a.y) * t) >> 16) };
    return o;
}
/* ---- hidden lines: what the tyre will cover is not drawn at all ---- */
static const op1_pt *cull;                    /* the tyre's silhouette (convex) while the street behind it is drawn */
static int cull_n, cull_sg;
/* inside a convex polygon whose winding sign is sg */
static int in_hull(const op1_pt *h, int n, int sg, op1_pt q)
{
    for (int i = 0, j = n - 1; i < n; j = i++)
        if (((int64_t)(h[i].x - h[j].x) * (q.y - h[j].y) - (int64_t)(h[i].y - h[j].y) * (q.x - h[j].x)) * sg < 0) return 0;
    return 1;
}
/* the part [t0, t1] (Q16) of a -> b inside the convex polygon (Cyrus-Beck) */
static int hull_in(op1_pt a, op1_pt b, const op1_pt *h, int n, int sg, int32_t *t0, int32_t *t1)
{
    int64_t lo = 0, hi = 65536;
    const int32_t dx = b.x - a.x, dy = b.y - a.y;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const int64_t ex = h[i].x - h[j].x, ey = h[i].y - h[j].y;
        const int64_t A = (ex * (a.y - h[j].y) - ey * (a.x - h[j].x)) * sg, B = (ex * dy - ey * dx) * sg;
        if (B == 0) { if (A < 0) return 0; continue; }
        const int64_t t = (-A * 65536) / B;
        if (B > 0) { if (t > lo) lo = t; } else if (t < hi) hi = t;
        if (lo >= hi) return 0;
    }
    *t0 = (int32_t)lo; *t1 = (int32_t)hi;
    return 1;
}
static void road_cut(void *r, op1_pt a, op1_pt b, const uint8_t *c, int k)
{
    if (k == 4) {
        if (a.x == b.x && a.y == b.y) return;
        int32_t t0, t1;
        if (cull && hull_in(a, b, cull, cull_n, cull_sg, &t0, &t1)) {   /* behind the tyre: only what sticks out */
            if (t0 > 256) road_cut(r, a, lerp(a, b, t0), c, 5);
            if (t1 < 65536 - 256) road_cut(r, lerp(a, b, t1), b, c, 5);
            return;
        }
        k = 5;
    }
    if (k == 5) {
        if (a.x == b.x && a.y == b.y) return;
        if (road_nseg < ROAD_SEG_MAX) { road_seg[road_nseg][0] = a; road_seg[road_nseg][1] = b; road_nseg++; }
        lnp(r, a, b, c);
        return;
    }
    int32_t t0, t1;
    if (!box_in(a, b, OP1_Q10(KEEP_OUT[k][0]), OP1_Q10(KEEP_OUT[k][1]), OP1_Q10(KEEP_OUT[k][2]), OP1_Q10(KEEP_OUT[k][3]), &t0, &t1)) {
        road_cut(r, a, b, c, k + 1);
        return;
    }
    if (t0 > 0) road_cut(r, a, lerp(a, b, t0), c, k + 1);
    if (t1 < 65536) road_cut(r, lerp(a, b, t1), b, c, k + 1);
}
static void road_line(void *r, op1_pt a, op1_pt b, const uint8_t *c)
{
    int32_t t0, t1;                                            /* on the screen, above its lower margin */
    if (!box_in(a, b, OP1_Q10(4), OP1_Q10(4), OP1_Q10(316), OP1_Q10(149) + 512, &t0, &t1) || t1 - t0 < 64) return;
    road_cut(r, lerp(a, b, t0), lerp(a, b, t1), c, 0);
}

/* ---- the street: road, curb, sidewalk, grass, and the knock's rocks, cracks and weeds ----
 * All of it is laid out in road coordinates (s along travel, p across, from the wheel's near face back) and scrolls
 * with the wheel; every pattern is a hash of a cell's index, so it is the same each time it comes round. Everything
 * at or behind the near face is drawn before the wheel (the wheel hides it), the front edge after. */
static inline uint32_t hsh(uint32_t i, uint32_t salt) { i = (i ^ salt) * 2654435761u; i ^= i >> 15; return i * 2246822519u; }
static inline op1_pt gph(int32_t s, int32_t p, int32_t h) { op1_pt o = gp(s, p); o.y -= h; return o; }
static const uint8_t COL_KERB[4]  = { 0xA0, 0xA2, 0xA6, 0xFF };
static const uint8_t COL_LINE[4]  = { 0xD8, 0xB0, 0x28, 0xFF };   /* the centre line: road-paint yellow */
static const uint8_t COL_GRASS[4] = { 0x3A, 0x9A, 0x4A, 0xFF };
static const uint8_t COL_WEED[4]  = { 0x8C, 0xE6, 0x4E, 0xFF };
static int vis_rocks, vis_cracks, vis_weeds, vis_tufts;   /* for the tests */

/* a tuft at ground point g: three blades, h high (Q10 px), leaning by the hash */
static void tuft(void *r, op1_pt g, int32_t h, uint32_t k, const uint8_t *c)
{
    for (int b = -1; b <= 1; b++) {
        const int32_t lean = (int32_t)((k >> (4 + 3 * (b + 1))) & 7) * 256 - 896 + b * 1200;
        const int32_t hb = h - (b ? h / 4 : 0);
        op1_pt a = { g.x + b * 900, g.y }, t = { g.x + b * 900 + lean, g.y - hb };
        road_line(r, a, t, c);
    }
}
/* a rock at (s, p), r across and h high (Q10 px): the hull of its footprint and its top, filled, outlined */
static void rock(void *r, int32_t s, int32_t p, int32_t rr, int32_t h)
{
    op1_pt q[7] = { gp(s - rr, p), gp(s, p - rr), gp(s + rr, p), gp(s, p + rr),
                    gph(s - rr / 2, p, h * 3 / 4), gph(s + rr / 3, p - rr / 3, h), gph(s, p + rr / 2, h * 4 / 5) }, hl[16];
    const int n = hull(q, 7, hl);
    for (int i = 0; i < n; i++) {                              /* not over a label, not off the screen */
        if (hl[i].x < OP1_Q10(4) || hl[i].x > OP1_Q10(316) || hl[i].y < OP1_Q10(4) || hl[i].y > OP1_Q10(149)) return;
        for (int k = 0; k < 4; k++)
            if (hl[i].x > OP1_Q10(KEEP_OUT[k][0]) && hl[i].x < OP1_Q10(KEEP_OUT[k][2]) && hl[i].y > OP1_Q10(KEEP_OUT[k][1]) && hl[i].y < OP1_Q10(KEEP_OUT[k][3])) return;
    }
    if (cull) {                                                /* wholly behind the tyre: nothing to draw */
        int hid = 1;
        for (int i = 0; i < n && hid; i++) hid = in_hull(cull, cull_n, cull_sg, hl[i]);
        if (hid) return;
    }
    static const uint8_t BLK[4] = { 0, 0, 0, 0xFF };
    fill_poly(hl, n, BLK);
    for (int i = 0; i < n; i++) road_line(r, hl[i], hl[(i + 1) % n], COL_BUMP);
    vis_rocks++;
}

static void draw_road(void *r, int32_t amt, int32_t shadow_w, int front, int32_t track)
{
    const int32_t s0 = -OP1_Q10(ROAD_LEN), s1 = OP1_Q10(ROAD_LEN), p0 = OP1_Q10(ROAD_P0), p1 = OP1_Q10(ROAD_P1);
    if (front) {                                               /* the near side, in front of the wheel: its curb's top at the
                                                                * road's edge (its face looks away), the curb's front, the
                                                                * sidewalk's front edge and its joints, as the far side's */
        const int32_t kh = OP1_Q10(KERB_H), pk = p0 - OP1_Q10(4), pw = pk - OP1_Q10(WALK_W);
        road_line(r, gph(s0, p0, kh), gph(s1, p0, kh), COL_KERB);
        road_line(r, gph(s0, pk, kh), gph(s1, pk, kh), COL_DIM);
        road_line(r, gph(s0, pw, kh), gph(s1, pw, kh), COL_GREY);
        const int32_t slab = OP1_Q10(SLAB_PX), soff = (int32_t)(scroll % (uint32_t)slab);
        for (int32_t d = s0 - soff; d < s1; d += slab) road_line(r, gph(d, pk, kh), gph(d, pw, kh), COL_DIM);
        const int32_t cs = OP1_Q10(GRASS_S), cp = OP1_Q10(GRASS_P);      /* and grass beyond it, as the far side's */
        const uint32_t first = scroll / (uint32_t)cs;
        const int32_t frac = (int32_t)(scroll % (uint32_t)cs);
        for (int row = 0; row < GRASS_ROWS; row++)
            for (int j = -GRASS_COLS; j <= GRASS_COLS; j++) {
                const uint32_t k = hsh(first + (uint32_t)j, 0x2a77 + (uint32_t)row * 613u);
                if ((k & 255) > 72) continue;
                const int32_t s = j * cs - frac + (int32_t)((k >> 8) & 1023) * cs / 1024;
                const int32_t p = pw - OP1_Q10(3) - row * cp - (int32_t)((k >> 18) & 1023) * cp / 1024;
                tuft(r, gph(s, p, kh), OP1_Q10(3) + (int32_t)((k >> 28) & 3) * 512, k, COL_GRASS);
                vis_tufts++;
            }
        return;
    }
    road_nseg = 0;
    vis_rocks = vis_cracks = vis_weeds = vis_tufts = 0;

    /* curb: the gutter at road level, the curb's front and back edges KERB_H up; the sidewalk behind it, with joints */
    const int32_t kh = OP1_Q10(KERB_H), pk = p1 + OP1_Q10(4), pw = pk + OP1_Q10(WALK_W);
    road_line(r, gp(s0, p1), gp(s1, p1), COL_GREY);
    road_line(r, gph(s0, p1, kh), gph(s1, p1, kh), COL_KERB);
    road_line(r, gph(s0, pk, kh), gph(s1, pk, kh), COL_DIM);
    road_line(r, gph(s0, pw, kh), gph(s1, pw, kh), COL_GREY);
    const int32_t slab = OP1_Q10(SLAB_PX), soff = (int32_t)(scroll % (uint32_t)slab);
    for (int32_t d = s0 - soff; d < s1; d += slab) {
        road_line(r, gph(d, p1, kh), gp(d, p1), COL_DIM);      /* the curb's joints down its face */
        road_line(r, gph(d, pk, kh), gph(d, pw, kh), COL_DIM);
    }

    /* the centre line, dashed, scrolling back as the wheel rolls on */
    const int32_t pm = (p0 + p1) / 2, dash = OP1_Q10(DASH_PX);
    const int32_t off = (int32_t)(scroll % (uint32_t)(2 * dash));
    for (int32_t d = s0 - off; d < s1; d += 2 * dash) road_line(r, gp(d, pm), gp(d + dash, pm), COL_LINE);

    /* grass beyond the sidewalk: a tuft in some cells of a grid, at a jittered place in the cell */
    {
        const int32_t cs = OP1_Q10(GRASS_S), cp = OP1_Q10(GRASS_P);
        const uint32_t first = scroll / (uint32_t)cs;
        const int32_t frac = (int32_t)(scroll % (uint32_t)cs);
        for (int row = 0; row < GRASS_ROWS; row++)
            for (int j = -GRASS_COLS; j <= GRASS_COLS; j++) {
                const uint32_t k = hsh(first + (uint32_t)j, 0x51ed + (uint32_t)row * 977u);
                if ((k & 255) > 72) continue;
                const int32_t s = j * cs - frac + (int32_t)((k >> 8) & 1023) * cs / 1024;
                const int32_t p = pw + OP1_Q10(3) + row * cp + (int32_t)((k >> 18) & 1023) * cp / 1024;
                tuft(r, gph(s, p, kh), OP1_Q10(3) + (int32_t)((k >> 28) & 3) * 512, k, COL_GRASS);
                vis_tufts++;
            }
    }

    /* the knock's bumpiness. Cracks across the pavement, more of them with the amount; weeds in them from 40 % */
    {
        const int32_t cs = OP1_Q10(CRACK_PX);
        const uint32_t first = scroll / (uint32_t)cs;
        const int32_t frac = (int32_t)(scroll % (uint32_t)cs);
        const uint32_t many = (uint32_t)amt >> 7;              /* 0 .. 255: how many cells have one */
        for (int j = -ROAD_LEN / CRACK_PX; j <= ROAD_LEN / CRACK_PX; j++) {
            const uint32_t k = hsh(first + (uint32_t)j, 0xc4ac);
            if ((k & 255) >= many * 4 / 5) continue;
            int32_t s = j * cs - frac + (int32_t)((k >> 8) & 255) * cs / 512;
            int32_t p = p0 + OP1_Q10(6) + (int32_t)(((k >> 16) & 1023) * (uint32_t)(p1 - p0 - OP1_Q10(30)) >> 10);
            op1_pt a = gp(s, p), mid = a;
            const int segs = 3 + (int)((amt * 3) >> 15);       /* longer with the amount: 3 .. 5 pieces */
            for (int seg = 0; seg < segs; seg++) {             /* a zigzag across the road */
                s += (int32_t)((k >> (seg * 3)) & 7) * 640 - 2240;
                p += OP1_Q10(5) + (int32_t)((k >> (12 + seg * 2)) & 3) * 1024;
                const op1_pt b = gp(s, p);
                road_line(r, a, b, COL_GREY);
                if (seg == 1) {                                /* and a branch off it */
                    mid = b;
                    road_line(r, b, gp(s + ((k >> 27) & 1 ? OP1_Q10(5) : -OP1_Q10(5)), p + OP1_Q10(3)), COL_GREY);
                }
                a = b;
            }
            vis_cracks++;
            if (amt > 13107 && (k >> 30)) {
                tuft(r, mid, OP1_Q10(2) + (int32_t)(((int64_t)(amt - 13107) * 3072) / 19660), k * 7u, COL_WEED);   /* 2 .. 5 px */
                vis_weeds++;
            }
        }
    }
    /* rocks in the wheel's track, where it hops (bump), bigger with the amount; a few pebbles elsewhere on the road */
    {
        const int32_t tick = OP1_Q10(TICK_PX);
        const uint32_t first = scroll / (uint32_t)tick;
        const int32_t frac = (int32_t)(scroll % (uint32_t)tick);
        for (int j = -ROAD_LEN / TICK_PX; j <= ROAD_LEN / TICK_PX; j++) {
            const int32_t s = j * tick - frac;
            const uint32_t id = first + (uint32_t)(j + 4);
            const int32_t h = bump(id, amt);
            if (h) {
                const uint32_t k = hsh(id, 0x70c4);
                rock(r, s, track / 4 + (int32_t)(((k & 1023) * (uint32_t)(track / 2)) >> 10), OP1_Q10(2) + h * 512, h * 1024);
            } else if (amt > 6554 && (hsh(id, 0xbeb1) & 255) < (uint32_t)amt >> 9) {
                const uint32_t k = hsh(id, 0x9eb1);
                const int32_t p = track + OP1_Q10(6) + (int32_t)(((k & 1023) * (uint32_t)(p1 - track - OP1_Q10(12))) >> 10);
                rock(r, s, p, OP1_Q10(1) + 512, 1536);
            }
        }
    }
    if (shadow_w > 0) {                                        /* the wheel off the road: its shadow where it was */
        road_line(r, gp(-OP1_Q10(6), 0), gp(-OP1_Q10(6), shadow_w), COL_GREY);
        road_line(r, gp(OP1_Q10(6), 0), gp(OP1_Q10(6), shadow_w), COL_GREY);
    }
}

/* the wheel's outlines and silhouette for this frame, before anything is drawn: the street behind it is culled by it */
static op1_pt nf[LUG_N / 2 * 5], ff[LUG_N / 2 * 5];
static int nn, fn;
static void wheel_prep(const wheel_t *w, int32_t sp)
{
    const int32_t a0 = -sp;
    op1_pt all[LUG_N * 5 + 24];
    nn = outline(w, a0, 0, nf); fn = outline(w, a0, 1, ff);
    for (int i = 0; i < nn; i++) all[i] = nf[i];
    for (int i = 0; i < fn; i++) all[nn + i] = ff[i];
    int na = nn + fn;
    if (w->inf) {                         /* blown up, the wall balloons past the tread's edge: the outline takes it in, */
        const int32_t rw = w->rc - (w->rc - w->rrim) / 4;      /* as far out as the line drawn on it, at its */
        int n = 6 + (rw >> 11);                                /* points (ring's segments) */
        n = n < 10 ? 10 : (n > 24 ? 24 : n);
        for (int i = 0; i < n; i++) all[na++] = wp(w, 8192 * i / n, rw + OP1_Q10(2), 0);   /* 2 px out: the line's */
                                                                                          /* whole stroke inside it */
    }
    sil_n = hull(all, na, sil);
    int64_t area = 0;
    for (int i = 0, j = sil_n - 1; i < sil_n; j = i++) area += (int64_t)sil[j].x * sil[i].y - (int64_t)sil[i].x * sil[j].y;
    cull_sg = area > 0 ? 1 : -1;
    cull_n = sil_n;
    near_face = nf; near_n = nn;
}

/* the easter egg (Shift + orange push on T1, not saved): the sidewall's markings, as a tyre maker's. With the valve at
 * 12 o'clock: GRIFTY at 9, TYR8 at 3, a checkered flag at 6. Angular stroke letters (a 5-row grid, 2 or 3 wide, a unit
 * apart), upright (their tops outward), set through wp so they tilt, squash and turn with the tyre. Where the sidewall
 * has no room for letters 4 px tall, each marking is a plain white bar */
typedef struct { char ch; int8_t w, n; int8_t s[11][4]; } glyph_t;
static const glyph_t GLYPHS[] = {
    { 'G', 3, 7, { { 3, 4, 1, 4 }, { 1, 4, 0, 3 }, { 0, 3, 0, 1 }, { 0, 1, 1, 0 }, { 1, 0, 3, 0 }, { 3, 0, 3, 2 }, { 3, 2, 2, 2 } } },
    { 'R', 3, 6, { { 0, 0, 0, 4 }, { 0, 4, 2, 4 }, { 2, 4, 3, 3 }, { 3, 3, 2, 2 }, { 2, 2, 0, 2 }, { 2, 2, 3, 0 } } },
    { 'I', 2, 3, { { 0, 4, 2, 4 }, { 1, 4, 1, 0 }, { 0, 0, 2, 0 } } },
    { 'F', 3, 3, { { 0, 0, 0, 4 }, { 0, 4, 3, 4 }, { 0, 2, 2, 2 } } },
    { 'T', 2, 2, { { 0, 4, 2, 4 }, { 1, 4, 1, 0 } } },
    { 'Y', 2, 3, { { 0, 4, 1, 2 }, { 2, 4, 1, 2 }, { 1, 2, 1, 0 } } },
    { '8', 3, 11, { { 1, 0, 2, 0 }, { 2, 0, 3, 1 }, { 3, 1, 2, 2 }, { 2, 2, 3, 3 }, { 3, 3, 2, 4 }, { 2, 4, 1, 4 },
                    { 1, 4, 0, 3 }, { 0, 3, 1, 2 }, { 1, 2, 0, 1 }, { 0, 1, 1, 0 }, { 1, 2, 2, 2 } } } };
static const glyph_t *glyph_of(char ch)
{
    for (unsigned i = 0; i < sizeof GLYPHS / sizeof GLYPHS[0]; i++) if (GLYPHS[i].ch == ch) return &GLYPHS[i];
    return &GLYPHS[0];
}
/* a bar on the sidewall from arc units x0 to x1 (from ac, clockwise) and radius r0 to r1, filled */
static void side_bar(const wheel_t *w, int32_t ac, int32_t da, int32_t x0, int32_t x1, int32_t r0, int32_t r1, const uint8_t *c)
{
    op1_pt q[8];
    for (int i = 0; i < 4; i++) {
        const int32_t x = x0 + (x1 - x0) * i / 3;
        q[i] = wp(w, ac - x * da, r1, 0);
        q[7 - i] = wp(w, ac - x * da, r0, 0);
    }
    fill_poly(q, 8, c);
}
/* a marking centred at angle ac between radii lo and hi: a word, or (word 0) the checkered flag, 20 x 4 units */
static void side_mark(void *r, const wheel_t *w, int32_t ac, int32_t lo, int32_t hi, const char *word, const uint8_t *c)
{
    const int32_t rm = (lo + hi) / 2;
    int32_t h = (w->rc - w->rrim) * 38 / 100;                  /* the letters' height, Q10 px */
    if (h > hi - lo) h = hi - lo;
    int32_t wn = 20;                                           /* the width in units */
    if (word) { wn = -1; for (const char *k = word; *k; k++) wn += glyph_of(*k)->w + 1; }
    if (h < OP1_Q10(4)) {                                      /* no room: a white bar, as long as the marking would be */
        const int32_t hb = h < OP1_Q10(2) ? h : OP1_Q10(2), un = (h > OP1_Q10(2) ? h : OP1_Q10(2)) / 4;
        if (hb < 512) return;
        const int32_t da = (int32_t)((int64_t)un * 1304 / rm);
        side_bar(w, ac, da, -wn / 2, wn - wn / 2, rm - hb / 2, rm + hb / 2, c);
        return;
    }
    const int32_t un = h / 4, da = (int32_t)((int64_t)un * 1304 / rm);   /* a unit, and along the arc in angle */
    int32_t x = -wn / 2;                                       /* the left edge, in units from the centre */
    if (!word) {                                               /* the flag: 10 x 2 checks, every other one filled */
        for (int cy = 0; cy < 2; cy++)
            for (int cx = 0; cx < 10; cx++)
                if ((cx + cy) & 1) side_bar(w, ac, da, x + 2 * cx, x + 2 * cx + 2, rm + (cy - 1) * 2 * un, rm + cy * 2 * un, c);
        for (int q = 0; q < 4; q++) {                          /* its outline, the long sides in four pieces round the arc */
            const op1_pt a = wp(w, ac - (x + 5 * q) * da, rm - 2 * un, 0), b = wp(w, ac - (x + 5 * q + 5) * da, rm - 2 * un, 0);
            const op1_pt d = wp(w, ac - (x + 5 * q) * da, rm + 2 * un, 0), e = wp(w, ac - (x + 5 * q + 5) * da, rm + 2 * un, 0);
            lnp(r, a, b, c); lnp(r, d, e, c);
        }
        lnp(r, wp(w, ac - x * da, rm - 2 * un, 0), wp(w, ac - x * da, rm + 2 * un, 0), c);
        lnp(r, wp(w, ac - (x + 20) * da, rm - 2 * un, 0), wp(w, ac - (x + 20) * da, rm + 2 * un, 0), c);
        return;
    }
    for (const char *k = word; *k; k++) {
        const glyph_t *g = glyph_of(*k);
        for (int i = 0; i < g->n; i++) {
            const int8_t *sg = g->s[i];
            const op1_pt p0 = wp(w, ac - (x + sg[0]) * da, rm + (2 * sg[1] - 4) * un / 2, 0);
            const op1_pt p1 = wp(w, ac - (x + sg[2]) * da, rm + (2 * sg[3] - 4) * un / 2, 0);
            lnp(r, p0, p1, c);
        }
        x += g->w + 1;
    }
}

static void wheel(void *r, const wheel_t *w, int32_t sp, int32_t spokes)
{
    const int32_t a0 = -sp;                                    /* rolling up the road turns it clockwise */
    static const uint8_t BLACK[4] = { 0, 0, 0, 0xFF };
    fill_poly(sil, sil_n, BLACK);                              /* the whole tyre hides the road behind it */
    far_edge(r, w, a0);                                        /* the far rim, where it shows */
    if (w->rt <= w->rc) poly_line(r, sil, sil_n, COL_BUMP);   /* a slick: its outline, with the edges across the tread */

    /* the lugs, back to front: by depth along the view, (-0.749 cos + 0.67 sin) r minus the block's mid depth */
    int ord[LUG_N];
    int32_t dep[LUG_N];
    const int32_t pitch = 8192 / LUG_N;
    for (int i = 0; i < LUG_N; i++) {
        const int32_t c = a0 + i * pitch;
        dep[i] = (int32_t)(((-24550 * (int64_t)tcos(c) + 21955 * (int64_t)tsin(c)) >> 15) * (w->rc >> 10) >> 15)
               - ((i & 1 ? w->wd * 71 / 100 : w->wd * 29 / 100) >> 10);
        int j = i;
        while (j > 0 && dep[ord[j - 1]] > dep[i]) { ord[j] = ord[j - 1]; j--; }
        ord[j] = i;
    }
    if (w->rt > w->rc) for (int i = 0; i < LUG_N; i++) lug(r, w, a0, ord[i]);   /* none on a slick */

    fill_poly(nf, nn, BLACK);                                  /* the near face, over the band */
    poly_line(r, nf, nn, COL_WHITE);

    /* the sidewall: a groove line, or the whitewall and its pinstripe */
    const int32_t span = w->rc - w->rrim;
    vis_whitewall = spokes > 16384;
    if (vis_whitewall) {
        const int32_t ww = (int32_t)(((int64_t)(spokes - 16384) * (span * 6 / 10)) >> 14);   /* up to 0.6 of the sidewall */
        /* a solid white band: the outer ring, then the inner one backwards (the even-odd fill leaves the hole) */
        op1_pt band[2 * (RING_N + 1)];
        const int32_t r0 = w->rrim + OP1_Q10(1), r1 = r0 + ww;
        for (int i = 0; i <= RING_N; i++) band[i] = wp(w, 8192 * i / RING_N, r1, 0);
        for (int i = 0; i <= RING_N; i++) band[RING_N + 1 + i] = wp(w, 8192 * (RING_N - i) / RING_N, r0, 0);
        fill_poly(band, 2 * (RING_N + 1), COL_WHITE);
        ring(r, w, r1, 0, COL_WHITE);
        ring(r, w, r1 + OP1_Q10(2), 0, COL_PIN);
    } else ring(r, w, w->rc - span / 4, 0, w->strain ? COL_KNOB[3] : COL_GREY);   /* blown up: the line strains red */
    if (tyre_badge) {   /* the easter egg's markings, from the valve (a0 + 600) at 12 o'clock: on the plain sidewall between the
                         * rim and the groove line; on a whitewall, black on the band where it is wide enough, else white
                         * above its pinstripe */
        int32_t lo = w->rrim + OP1_Q10(2), hi = w->rc - span / 4 - OP1_Q10(2);
        const uint8_t *col = COL_WHITE;
        if (vis_whitewall) {
            const int32_t ww = (int32_t)(((int64_t)(spokes - 16384) * (span * 6 / 10)) >> 14), b0 = w->rrim + OP1_Q10(1);
            if (ww >= OP1_Q10(6)) { lo = b0 + OP1_Q10(1); hi = b0 + ww - OP1_Q10(1); col = BLACK; }
            else { lo = b0 + ww + OP1_Q10(4); hi = w->rc - OP1_Q10(2); }
        }
        const int32_t av = a0 + 600;
        side_mark(r, w, av + 2048, lo, hi, "GRIFTY", col);     /* 9 o'clock */
        side_mark(r, w, av - 2048, lo, hi, "TYR8", col);       /* 3 o'clock */
        side_mark(r, w, av + 4096, lo, hi, 0, col);            /* 6 o'clock: the flag */
    }

    /* the rim: rigid, round, turning */
    wheel_t rg = *w;
    rg.q = 0; rg.inf = 0; rg.bl = 0;
    ring(r, &rg, w->rrim, 0, COL_WHITE);
    ring(r, &rg, w->rrim * R_DISH / R_RIM, w->wd / 5, COL_GREY);  /* the back of the dish */
    const int32_t rh = w->s * R_HUB;
    vis_spokes = (spokes * 20 + 16384) >> 15 << 1;             /* 0 .. 40, in pairs */
    for (int k = 0; k < vis_spokes; k++) {                     /* laced: each one leaves the hub and crosses its neighbours */
        const int32_t a = a0 + k * 8192 / vis_spokes, lace = k & 1 ? 1100 : -1100;
        lnp(r, wp(&rg, a, rh + (rh >> 2), w->wd / 10), wp(&rg, a + lace, w->rrim - OP1_Q10(1), 0), k & 1 ? COL_WHITE : COL_BUMP);
    }
    vis_spinner = spokes > 26214;
    if (vis_spinner) {                                         /* the knock-off: a gold cap and two flared ears */
        ring(r, &rg, rh, 0, COL_GOLD);
        ring(r, &rg, rh / 2, 0, COL_GOLD);
        for (int e = 0; e < 2; e++) {
            const int32_t a = a0 + 1024 + e * 4096, re = rh * 5 / 2;
            const op1_pt b0 = wp(&rg, a - 700, rh, 0), b1 = wp(&rg, a + 700, rh, 0), t0 = wp(&rg, a - 260, re, 0), t1 = wp(&rg, a + 260, re, 0);
            lnp(r, b0, t0, COL_GOLD); lnp(r, t0, t1, COL_GOLD); lnp(r, t1, b1, COL_GOLD);
        }
    } else {
        ring(r, &rg, rh, 0, COL_WHITE);
        for (int k = 0; k < 5 && vis_spokes < 12; k++) {       /* the plain wheel's five nuts, while the spokes are few */
            const op1_pt c = wp(&rg, a0 + 819 + k * 8192 / 5, rh * 2, 0);
            ln(r, c.x - 1024, c.y, c.x + 1024, c.y, COL_KNOB[0]);
            ln(r, c.x, c.y - 1024, c.x, c.y + 1024, COL_KNOB[0]);
        }
    }
    wheel_t vt = *w;                                           /* valve: its tip in the rubber, squashed with a flat (it */
    vt.inf = 0; vt.bl = 0;                                     /* poked out under it), not stretched by the blowing up */
    if (vt.q) vt.q += OP1_Q10(2);                              /* and its tip 2 px above the flat, its stroke inside */
    lnp(r, wp(&rg, a0 + 600, w->rrim + OP1_Q10(1), 0), wp(&vt, a0 + 600, w->rrim + OP1_Q10(5), 0), COL_KNOB[3]);
}

/* one block a corner, each encoder in its colour (blue and green on top, white and orange below): name, value and a
 * gauge of val (0..32767). unit: '%'; 'M' ms (from 1000 shown in seconds); 'S' tenths of a second; 'H' HOLD */
static void labels(void *r, const char *const name[4], const int32_t val[4], const int32_t num[4], const char unit[4], int yb)
{
    for (int i = 0; i < 4; i++) {
        const int32_t x0 = OP1_Q10(i & 1 ? 312 - LBL_W : 8), y = OP1_Q10(i < 2 ? 8 : yb);
        const int32_t len = (OP1_Q10(LBL_W) >> 5) * (val[i] >> 5) >> 5;
        char t[8]; int n = 0;
        int32_t v = num[i];
        char u = unit[i];
        if (u == 'M' && v >= 1000) { v = (v + 50) / 100; u = 'S'; }
        if (v < 0) { t[n++] = '-'; v = -v; }
        else if (u == 'T' && v > 0) t[n++] = '+';
        if (u == 'H') { t[n++] = 'H'; t[n++] = 'O'; t[n++] = 'L'; t[n++] = 'D'; }
        else if (u == 'S') { t[n++] = (char)('0' + v / 10); t[n++] = '.'; t[n++] = (char)('0' + v % 10); t[n++] = 'S'; }
        else {
            if (v >= 100) t[n++] = (char)('0' + v / 100);
            if (v >= 10) t[n++] = (char)('0' + v / 10 % 10);
            t[n++] = (char)('0' + v % 10);
            if (u == 'M') { t[n++] = 'M'; t[n++] = 'S'; } else if (u != 'T') t[n++] = '%';   /* T: semitones, bare */
        }
        t[n] = 0;
        sq_text(r, x0, y, LBL_S, name[i], 1024, COL_KNOB[i]);
        sq_text(r, x0 + OP1_Q10(LBL_W) - sq_width(LBL_S, t) + OP1_Q10(2), y, LBL_S, t, 1024, COL_KNOB[i]);
        ln(r, x0, y + OP1_Q10(11), x0 + OP1_Q10(LBL_W), y + OP1_Q10(11), COL_GREY);
        if (len > 512) ln(r, x0, y + OP1_Q10(11), x0 + len, y + OP1_Q10(11), COL_KNOB[i]);
    }
}

static void ty_screen_update(void *scr, int dt)
{
    const int slot = OP1_SCREEN_SLOT & 7;
    int16_t k[4];
    op1_synth_knobs_live(slot, k);
    const int16_t *w = op1_synth_knobs(slot);
    const int32_t clip = w[6] < 0 ? 0 : w[6], drive = w[5] < 0 ? 0 : w[5];
    const int32_t tread = drive > clip ? drive : clip;     /* the lugs: drive or clip, whichever is higher */
    int16_t rc[REC_N];
    rec_get(slot, rc);
    if (dt < 0) dt = 0;
    if (dt > 50) dt = 50;

    /* the wheel: size from the decay, lugs from clip, flat from sag, spokes from sub */
    int32_t env[4];
    t2_get(slot, env);
    wheel_t wh;
    wh.s = 563 + ((266 * env[T2_DECAY]) >> 15);                             /* 0.55 .. 0.81, Q10 */
    const int32_t kt = (tread * 1024) >> 15;                                 /* lug height 0 .. 1, Q10; 0: a slick */
    wh.rc = wh.s * R_C;
    wh.rt = wh.rc + wh.s * LUG_H * kt / 1024;
    wh.rout = wh.rt;
    wh.wd = wh.s * TYRE_W;
    wh.rrim = wh.s * R_RIM;
    wh.q = (int32_t)(((int64_t)(wh.rout - wh.rrim) * 13 / 20 * q15(k[3])) >> 15);   /* sag: up to 65 % of the sidewall */
    const int32_t rise = q15(k[3]) ? 0 : rc[REC_RISE];                                /* sag below 0: blown up, */
    {   /* the tread out by up to 16 % of the radius, faster towards -100 %; from 60 % a blister swells on it, from 80 %
         * the sidewall line strains red, and from 70 % the tyre quivers */
        const int64_t side = wh.rout, r2 = ((int64_t)rise * rise) >> 15;
        int32_t inf = (int32_t)((side * (6 * rise + 10 * r2) / 100) >> 15);
        int32_t bl = rise > 19660 ? (int32_t)((side * 8 / 100 * (rise - 19660)) / 13107) : 0;
        /* but no taller than the largest tyre, blown up 7 px: the screen's top. A smaller tyre blows up further */
        const int32_t room = 829 * (R_C + LUG_H) + OP1_Q10(7) - wh.rout;
        if (inf + bl > room) {
            const int32_t sc = room > 0 ? (int32_t)(((int64_t)room << 10) / (inf + bl)) : 0;
            inf = (int32_t)(((int64_t)inf * sc) >> 10); bl = (int32_t)(((int64_t)bl * sc) >> 10);
        }
        wh.inf = inf; wh.bl = bl;
        wh.ba = -(int32_t)(ang >> 8) + 1400;                                              /* it turns with the tyre */
        wh.strain = rise > 26214;
    }

    /* spin: the sounding pitch (12 .. 200 Hz counted), 32 Hz = one turn a second */
    const int32_t lv = vis_lvl;
    const int32_t want = lv > 1024 ? 1024 : lv;
    run = want > run ? run + ((want - run + 1) >> 1) : run - (run >> 5) - 1;
    if (run < want) run = want;
    int32_t hz = (int32_t)(((vis_inc >> 16) * 88200u) >> 16);
    hz = hz < 12 ? 12 : (hz > 200 ? 200 : hz);
    const uint32_t da = (uint32_t)(((hz * 66 * run) >> 10) * dt);            /* table units << 8 */
    ang += da;
    const uint32_t ds = (uint32_t)(((int64_t)da * wh.rout >> 10) * 6434 >> 21);   /* arc = angle x radius, Q10 px */
    scroll += ds;

    /* bounce: a kick on every restarted note and on every ridge that passes under the wheel */
    const int32_t amt = k[1];
    if (vis_trig != seen_trig) {
        seen_trig = vis_trig;
        bv = -OP1_Q10(40) - ((OP1_Q10(110) * (amt >> 5)) >> 10);
    }
    const uint32_t tick = (scroll + OP1_Q10(TICK_PX) * 4) / OP1_Q10(TICK_PX);
    if (tick != seen_tick) {
        seen_tick = tick;
        const int32_t h = bump(tick, amt);
        if (h > 0 && by == 0 && bv == 0 && run > 64) bv = -OP1_Q10(18) * h;   /* not over a note's kick */
    }
    /* the ceiling: the tyre's top no higher than 24 px, as high as the largest round tyre's highest kick goes. A big
     * blown-up tyre kicked hard, or kicks that land while it is still up (fast notes), would carry it into the labels,
     * whose still layer then keeps what it drew there */
    const int32_t by_min = OP1_Q10(24) + (wh.rout + wh.inf + wh.bl) + (wh.rout - wh.q + wh.inf) - OP1_Q10(CON_Y);
    for (int i = 0; i < dt; i++) {                                           /* one step a millisecond */
        bv += 922;                                                           /* 900 px / s^2 */
        by += bv / 1000;
        if (by < by_min) { by = by_min; if (bv < 0) bv = 0; }
        if (by >= 0) {
            by = 0;
            bv = -(bv * 45) / 100;
            if (bv > -OP1_Q10(15)) bv = 0;
        }
    }
    wh.hx = OP1_Q10(CON_X);
    wh.hy = OP1_Q10(CON_Y) - (wh.rout - wh.q + wh.inf) + by;
    vis_hub_y = wh.hy;
    static uint8_t quiver;
    if (rise > 22937) {                                                      /* about to go: a 1 .. 1.6 px quiver */
        quiver++;
        const int32_t qa = OP1_Q10(1) + (int32_t)((OP1_Q10(1) * 3 / 5 * (int64_t)(rise - 22937)) / 9830);
        const int32_t qs = quiver & 1 ? qa : -qa;                           /* along the road, so it stays on it */
        wh.hx += (qs * TRV_X) >> 10; wh.hy += (qs * TRV_Y) >> 10;
    }
    last_wheel = wh;

    /* the labels: what they say this frame */
    /* the four encoders, each in its colour: name, gauge, value. While Shift is held, the four Shift values */
    const int shift = op1_shift_held() != 0;
    const char *name[4];
    int32_t val[4], num[4];
    char unit[4];
    if (shift) {
        static const char *const sn[4] = { "CLIP", "KNCKLEN", "SUB", "GLIDE" };
        for (int i = 0; i < 4; i++) { name[i] = sn[i]; unit[i] = '%'; }
        val[0] = clip;
        val[1] = w[7] < 0 ? 0 : w[7];
        val[2] = rc[REC_SUB];
        val[3] = rc[REC_GLIDE];
        num[1] = (int32_t)((1500u * exp2neg((uint32_t)((32767 - val[1]) * 25514) >> 15) + 32768u) >> 16);  /* ms */
        unit[1] = 'M';
        for (int i = 0; i < 3; i++) if (i != 1) num[i] = (val[i] * 100 + 16384) >> 15;
        num[3] = (val[3] * 500 + 16384) >> 15;                               /* glide, ms */
        unit[3] = 'M';
        if (blue_pg) { name[0] = "SMP TUNE"; num[0] = rc[REC_STUNE]; val[0] = (rc[REC_STUNE] + 24) * 32767 / 48; unit[0] = 'T'; }
        if (green_smp) {
            const int32_t au = rc[REC_SATK];
            name[1] = "SMP ATK"; val[1] = au; num[1] = (((au * au) >> 15) * 100) >> 15; unit[1] = 'M';
        }
        if (white_smp) { name[2] = "SMP HPF"; val[2] = rc[REC_SHP]; num[2] = (val[2] * 100 + 16384) >> 15; unit[2] = '%'; }
    } else {
        static const char *const pn[4] = { "SHAPE", "KNOCK", "TONE", "SAG" };
        for (int i = 0; i < 4; i++) { name[i] = pn[i]; val[i] = k[i]; unit[i] = '%'; }
        num[0] = ((val[0] * 200 + 16384) >> 15) - 50;                        /* -50 feedback sine, 0 sine, 50 triangle, 100 saw, 150 feedback saw */
        num[1] = (val[1] * 100 + 16384) >> 15;
        num[2] = (val[2] * 100 + 16384) >> 15;                               /* tone: how open */
        if (!q15(k[3]) && rc[REC_RISE]) val[3] = rc[REC_RISE];              /* sag: the gauge shows how much, */
        num[3] = (val[3] * 100 + 16384) >> 15;                               /* the value, %, its way: */
        if (!q15(k[3]) && rc[REC_RISE]) num[3] = -num[3];                    /* below 0 it rises */
        if (white_smp) { name[2] = "S.TONE"; val[2] = 32767 - rc[REC_STONE]; num[2] = (val[2] * 100 + 16384) >> 15; }
        if (blue_pg) { name[0] = "DRIVE"; val[0] = drive; num[0] = (drive * 100 + 16384) >> 15; }
        if (green_smp) {                                                     /* SMP REV: it plays backwards */
            const src_t *sr = &srcb[src_i];
            name[1] = sr->ok && sr->rev ? "SMP REV" : "SAMPLE";
            val[1] = rc[REC_SLVL]; num[1] = (val[1] * 100 + 16384) >> 15;
        }
    }
    /* The labels are a still layer: the core keeps it and each frame restores only the moving areas (the whole screen
     * outside the label blocks, which the street and the tyre never enter), so they are drawn again only when what
     * they say changes. Without the core's region support op1_screen_static is always 1 and everything is drawn */
    /* the still label areas are the label text and gauges and 1 px: 0..105 and 217..319, x 0..21 at the top and
     * 128..144 at the bottom (LBL_YB_T1 - 2 .. + 14); the moving area is the rest. KEEP_OUT is 3 px bigger */
    static const op1_rect_t DYN[5] = { { 106, 0, 216, 159 }, { 0, 22, 105, 127 }, { 0, 145, 105, 159 },
                                       { 217, 22, 319, 127 }, { 217, 145, 319, 159 } };
    uint32_t lh = 2166136261u;
    for (int i = 0; i < 4; i++) {
        lh = (lh ^ (uint32_t)(uintptr_t)name[i]) * 16777619u;
        lh = (lh ^ (uint32_t)num[i]) * 16777619u;
        lh = (lh ^ (uint32_t)(val[i] >> 5)) * 16777619u;     /* the gauge moves in steps of 32 */
        lh = (lh ^ (uint32_t)unit[i]) * 16777619u;
    }
    static uint32_t last_lh;
    if (lh != last_lh) { last_lh = lh; op1_screen_static_dirty(scr); }
    op1_screen_regions(scr, DYN, 5);
    void *r = op1_raster();
    if (op1_screen_static(scr)) {
        op1_screen_draw(scr, dt);
        labels(r, name, val, num, unit, LBL_YB_T1);
        op1_screen_static_done(scr);
    }

    /* the moving parts, only inside DYN */
#ifdef OP1_HOST
    host_mark_dynamic();
#endif
    fill_begin(scr, r);
    wheel_prep(&wh, (int32_t)(ang >> 8));
    cull = sil;                                                /* the street behind the tyre: what it covers is skipped */
    draw_road(r, amt, by < -OP1_Q10(2) ? wh.wd : 0, 0, wh.wd);
    cull = 0;
    wheel(r, &wh, (int32_t)(ang >> 8), rc[REC_SUB]);
    draw_road(r, amt, 0, 1, wh.wd);                            /* in front of it: not culled */
}

const op1_screen_t SCREEN_tyr8 = { ty_screen_init, ty_screen_update };

/* ======================= T2 page (GUI thread): the envelope =======================
 * R246 turns the four T2 words; this page draws what they mean here. The curve's four parts are in their knob's
 * colour: attack (blue) rises straight, decay (green) falls to the sustain level (white), release (orange) falls from
 * it to nothing after the key goes up (a grey tick); at HOLD it falls at the decay's rate. Labels as on T1. */
#define ENV_X0    20                          /* the curve's area, px */
#define ENV_TOP   34
#define ENV_BASE  96
static void ty_t2_init(void *scr) { (void)scr; }

/* an exponential fall from y0 to y1 over [x0, x0 + wx], five time constants (2^-7.2), as 16 lines */
static void fall(void *r, int32_t x0, int32_t wx, int32_t y0, int32_t y1, const uint8_t *c)
{
    int32_t px = x0, py = y0;
    for (int i = 1; i <= 16; i++) {
        const int32_t e = (int32_t)exp2neg((uint32_t)i * 1843u);            /* Q16 */
        const int32_t x = x0 + (wx * i) / 16, y = y1 - (int32_t)(((int64_t)(y1 - y0) * e) >> 16);
        ln(r, px, py, x, y, c);
        px = x; py = y;
    }
}

static void ty_t2_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    const int slot = OP1_SCREEN_SLOT & 7;
    int32_t u[4];
    t2_get(slot, u);
    const int hold = u[T2_RELEASE] >= REL_HOLD;
    const int32_t rv = hold ? 32767 : u[T2_RELEASE] * 32767 / REL_HOLD;

    /* the curve: widths in px, attack 4 .. 50, decay 16 .. 90, the held part 40, release 16 .. 90 (HOLD: as the decay) */
    const int32_t wa = 4 + ((46 * u[T2_ATTACK]) >> 15), wd = 16 + ((74 * u[T2_DECAY]) >> 15), ws = 40;
    const int32_t wr = hold ? wd : 16 + ((74 * rv) >> 15);
    const int32_t x0 = OP1_Q10(ENV_X0), x1 = x0 + OP1_Q10(wa), x2 = x1 + OP1_Q10(wd), x3 = x2 + OP1_Q10(ws);
    const int32_t top = OP1_Q10(ENV_TOP), base = OP1_Q10(ENV_BASE);
    const int32_t ys = base - (int32_t)(((int64_t)(base - top) * u[T2_SUSTAIN]) >> 15);
    ln(r, x0, base, x3 + OP1_Q10(wr), base, COL_GREY);                       /* the zero line */
    ln(r, x2 + OP1_Q10(ws), base + OP1_Q10(3), x2 + OP1_Q10(ws), ys - OP1_Q10(6), COL_GREY);   /* key up */
    ln(r, x0, base, x1, top, COL_KNOB[0]);
    fall(r, x1, OP1_Q10(wd), top, ys, COL_KNOB[1]);
    ln(r, x2, ys, x3, ys, COL_KNOB[2]);
    fall(r, x3, OP1_Q10(wr), ys, base, COL_KNOB[3]);

    static const char *const nm[4] = { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" };
    const char *name[4] = { nm[0], nm[1], nm[2], nm[3] };
    int32_t val[4] = { u[0], u[1], u[2], u[3] }, num[4];
    char unit[4] = { 'M', 'S', '%', 'M' };
    num[0] = attack_ms(u[T2_ATTACK]);
    num[1] = (int32_t)((40u * exp2neg((uint32_t)((32767 - u[T2_DECAY]) * 21799) >> 15) + 32768u) >> 16);   /* s / 10 */
    num[2] = (u[T2_SUSTAIN] * 100 + 16384) >> 15;
    if (hold) unit[3] = 'H', num[3] = 0;
    else num[3] = (int32_t)((4000u * exp2neg((uint32_t)((32767 - rv) * 35407) >> 15) + 32768u) >> 16);   /* ms */
    labels(r, name, val, num, unit, LBL_YB_T2);

    /* the play mode, under orange's block (RELEASE) and in its colour: its push on this page steps it */
    {
        static const char *const modes[MODE_N] = { "MONO GLIDE", "MONO RETRIG" };
        const char *const mt = modes[mode_of(slot)];
        sq_text(r, OP1_Q10(312) - sq_width(LBL_S, mt) + OP1_Q10(2), OP1_Q10(123), LBL_S, mt, 1024, COL_KNOB[3]);
    }

    /* the sample layer's source, under green's block (its push steps it): SMP AUTO 2, SMP 3, or NONE where there is no
     * recording. Looked for again every 300 ms and on a change */
    int16_t rc[REC_N];
    rec_get(slot, rc);
    static src_t look; static int look_t = 1000; static int16_t last_src = -1; static int last_slot = -1;
    look_t += dt < 0 ? 0 : dt;
    if (look_t >= 300 || rc[REC_SRC] != last_src || slot != last_slot) {
        look_t = 0; last_src = rc[REC_SRC]; last_slot = slot;
        find_source(slot, rc[REC_SRC], &look);
    }
    char t[16]; int n = 0;
    static const char *const pre[2] = { "SMP ", "SMP AUTO " };
    for (const char *c = pre[rc[REC_SRC] == SRC_AUTO]; *c; c++) t[n++] = *c;
    if (rc[REC_SRC] != SRC_AUTO) { t[n++] = (char)('0' + rc[REC_SRC]); if (!look.ok) { t[n++] = ' '; t[n++] = '-'; } }
    else if (look.ok) t[n++] = (char)('1' + look.slot);
    else t[n++] = '-';
    t[n] = 0;
    sq_text(r, OP1_Q10(312) - sq_width(LBL_S, t) + OP1_Q10(2), OP1_Q10(24), LBL_S, t, 1024, look.ok ? COL_KNOB[1] : COL_GREY);
}

const op1_screen_t SCREEN_tyr8_t2 = { ty_t2_init, ty_t2_update };
