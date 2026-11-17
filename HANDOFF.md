# TYR8 handoff

For the next agent or person who picks this up. Written 2026-11-16 at version 0.9.3, by the agent that built it
with the owner over one long session. Read sections 1 to 4 before touching anything; the rest is reference.

---

## 1. What this is

TYR8 is an 808 bass synth for the **original Teenage Engineering OP-1** (Blackfin CPU, not the OP-1 field)
running the **Hundekraft** custom firmware. It ships as one plugin file, `tyr8.knl`, that users install through
the Hundekraft web workshop. The screen shows a tyre in three-quarter view that rolls, bounces, changes size and
grows treads with the sound.

- Repo: https://github.com/DavidHanover/TYR8 (branch `main`, remote `origin` over SSH).
- Licence: GPL-3.0-or-later with the OP-1 firmware permission (it links against the kennel SDK). `LICENSE` and
  `NOTICE` in the root are copies from that SDK.
- Plugin ID: `tyr8`. **Never change it.** Saved sounds are tied to the ID, and a different ID installs as a
  separate plugin.

### State at handoff

| Thing | State |
|---|---|
| Source | 0.9.9 committed on top of `e6a25ca` (0.9.3, pushed). The 0.9.9 commit is not pushed. |
| Not tried on hardware | Most of 0.9.4 to 0.9.9: the T2 page, the Shift-turn takeover, the code-drawn screen (pixel fills), the tail fade. The owner tests by ear on their OP-1. |
| Host checks | 139 pass (`./build.sh test`). |
| Plugin file | `build/tyr8.knl`, 37,460 bytes, version 0.9.9, built with Blackfin GCC 13.3.0. Not committed (`build/` is ignored). |
| GitHub release | **Not created.** The README links to the releases page, which is empty. The owner was going to make a release with `tyr8.knl` attached (planned as `v0.9.3`; the code is now 0.9.9). There is no `gh` CLI on this machine. |
| Reddit post | Drafted in `build/reddit_post.md` (ignored). Has two placeholders: the demo clip and a "Tested on" line. |
| Demo video | `build/tyr8_demo.mp4` (302 MB) and `build/tyr8_demo_reddit.mp4` (48 MB). See section 12. |

### What has and has not been verified

Be precise about this when you report to the owner or write anything public.

- **Verified on the PC only:** everything in the test suite. The tests build the plugin's C source for x86 against
  stand-ins for the firmware. They are not an emulator.
- **Verified by the packer:** the file builds and links against the Hundekraft 3.3 SDK, and its imports
  (`op1_engine_shift`, `op1_engine_record`, `op1_engine_record_store`, `op1_l1_sine_quarter`, `sq_text`) are all
  in that core's export table.
- **Seen on real hardware by the owner:** it loads, plays and draws. The owner has been installing builds on their
  own OP-1 through the session and giving feedback by ear. The demo video shows the screen working. I do not know
  which version they last installed, and I have never seen the hardware myself.
- **Never measured by anyone:** CPU load (especially poly mode with an effect on), behaviour on other panel
  revisions, save and reload across a power cycle, removal and reinstall.

Several real bugs were found only because the owner listened on hardware (see section 9). Treat "the tests pass"
as necessary, not sufficient.

---

## 2. Where everything is

```
TYR8-Kick-Synth/
  plugin/tyr8/
    tyr8.c          the whole plugin: engine (top half) and screen (bottom half), 684 lines
    plugin.json     manifest: ID, defaults, flags, version
    gen_svg.py      writes tyr8.svg and tyr8_geom.h; edit this, not the outputs
    tyr8.svg        generated screen art (committed, because the packer embeds it)
    tyr8_geom.h     generated geometry constants shared with tyr8.c
  test/
    tyr8_test.c     host test: #includes tyr8.c directly so it can reach the statics
    harness/op1_host.h   PC stand-ins for firmware addresses and calls
    render_frames.py     turns the test's frames.json into PNG previews (no dependencies)
  toolchain/Dockerfile   bfin-elf binutils 2.42 + GCC 13.3.0 + python3
  build.sh          fetch SDK, generate SVG, run checks, pack the .knl
  docs/screen.png   a PC-rendered preview used in the README
  README.md         user-facing: install, every control, play modes
  CHANGELOG.md      one entry so far
  HANDOFF.md        this file
  sdk/              (ignored) Hundekraft 3.3 source + SDK, downloaded by build.sh
  build/            (ignored) tyr8.knl, test binary, out/*.wav, out/screen_*.png, reddit post, demo videos
```

Outside the repo:

- Docker images on this machine: `bfin-elf-toolchain` (1.4 GB, the cross compiler) and `linuxserver/ffmpeg`
  (used once for the demo video). Docker Desktop must be running with WSL integration on, or the `docker`
  command is not found.
- The machine is WSL2 Ubuntu. No sudo. `gcc`, `python3`, `curl`, `git` are installed; `gh`, `ffmpeg`, `numpy`,
  `pip`-installed packages are not (numpy was installed into a scratch folder that may be gone).

### The SDK and its documentation

The original handoff this project started from had truncated links. The real sources:

- **`Hundekraft/op1-workshop`** on GitHub: the workshop site. Its `llms.txt` is written for AI assistants building
  kennel plugins and is the single most useful document. Hosted at
  https://hundekraft.github.io/op1-workshop/llms.txt. Its `source/` folder holds a source-and-SDK tarball per
  firmware version.
- **`sdk/hundekraft-3.3-source/ext/kennel/`** (after `./build.sh` has run): the SDK itself.
  - `synth_sdk/op1_plugin.h`: the API. Long, and the comments are the documentation. Read it.
  - `synth_sdk/kennel/kn_exports.c`: the exact functions a plugin may import. A header declaration alone does
    not mean the core exports it.
  - `tools/kennel/pack.py`: the packer. Also the reference for which `plugin.json` keys exist.
  - `synth_sdk/plugins/`: about 90 example plugins. `kastle` (own envelope, Shift layer), `pinboard` and `dx`
    (input hook, record), `supersaw` (simple screen) were the ones I learned from.
  - `docs/screen-art.md`, `PLUGIN_GUIDE.md` (two levels up).
- **`pcppcp/op1r-src`**: the upstream SDK that kennel grew from. Its `synth_sdk/PLUGINS.md` is a shorter guide,
  and it has a fuller host harness. The APIs have diverged; build against the Hundekraft bundle.

---

## 3. Build, test, install

```sh
./build.sh          # everything: SDK fetch (first run), SVG, host checks, pack build/tyr8.knl in Docker
./build.sh test     # host checks only, no Docker
```

- The host checks run with address and undefined-behaviour sanitizers. They have caught real bugs (shifts of
  negative values, a kick that cancelled itself). Keep them on.
- `./build.sh test` writes about 20 WAV previews and 15 screen PNGs to `build/out/`. The WAVs are what the OP-1
  would output at 44.1 kHz, without its 35 Hz bus high-pass. The PNGs come from a simple rasteriser of my own and
  show layout and motion, not the OP-1's actual pixels.
- The output file must be named `<id>.knl`; the packer refuses anything else.
- No need to clean `build/` between builds.

**Install or update on an OP-1:** open the workshop in Chrome or Edge, connect by USB, drop `build/tyr8.knl` into
**Your plugins**, **Review changes**, **Send to OP-1**, press **T1** on the unit. It restarts. Then synth →
Shift + T1 → tyr8. Same steps to update; the same ID replaces the old file. The browser keeps its own copy of the
file, so you must drop the new one in again after every rebuild.

**Bump `version` in `plugin.json` on every build you hand to the owner.** They asked for this explicitly once, and
it is how they tell builds apart in the workshop's review screen.

---

## 4. Rules of the platform (learned the hard way)

These come from the SDK docs and from mistakes made in this session.

### Audio code

- It runs **in the audio interrupt**. A crash, an endless loop or a block that takes too long locks the OP-1 until
  it is power-cycled.
- **Integers only.** There is no FPU. No float anywhere in `tyr8.c`.
- **No division, modulo or 64-bit arithmetic per sample.** Both are slow library calls. Once per block (per call
  of `render`) is fine, and the code uses `int64_t` and `/` there freely.
- **No libc.** No `memset`, `memcpy`, `printf`, `malloc`. Write the loops.
- A voice renders **128 samples at 88.2 kHz** per call; the firmware decimates 2:1 afterwards. About 689 calls a
  second, so one block is 1.451 ms. Every time constant in the code is derived from that.
- `OP1_PITCH_INC[n]` is the phase increment per sample for the voice being rendered (2^32 is one cycle). It is
  only valid during that voice's `render`, and already includes the OP-1's own pitch bend and LFO.
- Use `op1_sin_tab()` / `op1_sin_q()` for sine lookups **from `render` only**. From `note_on` or a screen, use
  `OP1_SINE_TABLE` directly. The screen code does.
- The synth bus has a **35 Hz high-pass** after the voices. Nothing an engine does can get sub below that out.
- A voice's output is `int16`. **There is no way to exceed full scale.** When the owner asks for "louder", the
  only levers are density (drive, clip) and the OP-1's own mixer.

### Threads

- `note_on` and `note_off` run on the **key thread**; `render` and `finished` on the **audio thread**; the screen
  on the **GUI thread**; the input hook on the **KeyHandler thread**.
- `render` can preempt everything else. Never leave shared state half-written where `render` reads it. In this
  code the key thread only writes small `volatile` bytes and words, and `render` owns all the sound state.
- `op1_engine_record_store` may be called from the input hook, not from the audio thread or the main loop.

### The screen

- The native SVG renderer takes a small subset: groups, lines, circles, polylines, paths with M / L / C / Z only.
  No CSS, no opacity, no quadratic curves, no arcs. **A number with more than four decimals panics the parser at
  boot.** The packer and `svg_check.py` check this; `gen_svg.py` writes two decimals.
- **Nested transforms are not known to compose.** Every moving part is a sibling group with its own complete
  matrix. This is why `#wheel-spin` is not inside `#wheel-rig`.
- I could not confirm that a `<circle>` squashes into an ellipse under a non-uniform matrix, so every ring in the
  art is a four-cubic **path**, which is transformed point by point.
- Matrices are **row vectors in Q10**: `{a, b, 0, c, d, 0, tx, ty, 1024}` with `x' = x*a + y*c + tx`,
  `y' = x*b + y*d + ty`. Screen space is 320 × 160.
- Keep art off the lower edge. The ground line is at y = 140, not at the bottom.
- A polyline that turns back on itself by more than about 170° draws a spike to the screen edge.
- Text is drawn with `sq_text` from `seq/seq_text.h` (a stroke font in the core: uppercase letters, digits and
  `. % : + - / #`). Six units wide and six tall per character at scale 1024.

### Saved state

- A sound has 8 words. Words 0 to 3 are the four knobs, word 4 is the engine selector (never touch), words 5 to 7
  are free. **R246's patch parser only accepts 0..32767 in a plugin engine's words;** a word with bit 15 set
  panics the save and can stop the boot.
- Beyond that, an engine can keep a **record** of up to 16 `int16` values with the sound
  (`op1_engine_record*`). A value of 0 should mean the default, because stock or older firmware drops the record.
- Changing what a saved value means breaks existing sounds. Section 9 lists the times this has already happened.

---

## 5. How `tyr8.c` works

One file, two halves. The top comment in the file is accurate as of this handoff.

### 5.1 Controls and where they are stored

Since 0.9.4 there are no push pages. The layout was the owner's own design (2026-11-17).

| Encoder | T1 turn (word) | T1 Shift + turn | T2 (R246's envelope words) |
|---|---|---|---|
| Blue | `SHAPE` (0) | `DRIVE` (5) or `CLIP` (6); Shift + push switches | `ATTACK` 0..1 s |
| Green | `KNOCK` (1) | `KNOCK LEN` (7) | `DECAY` 0.1..4 s |
| White | `TONE` (2) | `SUB` (record 5) | `SUSTAIN` |
| Orange | `SAG` (3) | `GLIDE` (record 6) | `RELEASE` 10 ms..4 s, top 5 % `HOLD` |

- **Turn:** the firmware's own knob handling writes words 0 to 3, 0..32767.
- **Shift + turn:** all taken by `ty_input` (`"engine_input": true`), which runs before the core's own Shift
  handling. It steps by `delta / 4` (256 a detent, more when turned fast), the same rate as the core's fine Shift
  turn. Words go through `op1_engine_word_set`, record values through `op1_engine_record_store`.
  `op1_engine_shift` is no longer called.
- **Pushes:** orange steps the play mode (record 0), with or without Shift. Shift + blue toggles `blue_clip`, a
  UI-only flag that is **not saved**. Other pushes are taken and do nothing.
- **T2:** `"t2_page": true` (needs `own_envelope`). R246's T2 knobs keep turning the slot's four envelope words
  (attack, decay, release 64..16320; sustain 0..32767), saved with the sound as its `"adsr"`. `t2_get()` reads them
  with `op1_t2_words(slot)` and normalises to 0..32767; all four 0 (never on the OP-1) reads as the 808 defaults.
  The page itself is `SCREEN_tyr8_t2`, drawn in code: the curve and four labels. Picking the engine keeps the slot's
  previous envelope words, so a new TYR8 sound starts with whatever envelope the slot had.
- **Envelope rule:** while the key is down the level falls to sustain at the decay rate; after it is up it falls to 0
  at the release rate, or at the decay rate with `HOLD`. Sustain 0 + `HOLD` is the 808 one-shot.
- **Record layout** (`REC_*`): `[0]` mode, `[5]` sub, `[6]` sag, each 0..32767. `[1]..[4]` held 0.9.3's push-page
  values and are ignored.
- **Defaults** (`plugin.json`): words `[8192, 12000, 32767, 0]` and page-2 `[14000, 12000, 4500]`: plain sine, a
  medium knock, tone open, no glide, drive 43 %, clip 37 %, knock length about 36 ms.
- **Tone law:** 12 kHz down 6.4 octaves to 140 Hz by `closed * (2 - closed)`. When it was linear in octaves, the
  top half of the knob was inaudible on a driven bass, because the clipper after the tone puts treble back.

### 5.2 Key tracking and play modes

The firmware allocates six voices and calls `note_on(self)` / `note_off(self)` per voice. The engine's own sound
state is **not** in the firmware's voice objects; it is the static array `tv[6]` of `voice_t`.

- `held[v]`, `stamp[v]` (note-on order), `legato_req[v]`, `beat[v]` (a heartbeat so a voice the firmware killed
  without a `note_off` is not treated as held), and `owner` (the newest voice) are the key state.
- **`MONO GLIDE` and `MONO RETRIG`** use `tv[0]` whichever firmware voice is the owner. Non-owner voices render
  silence and stay allocated only while their key is down, so the engine can fall back to them.
- **`POLY` was removed in 0.9.9** (the owner: it sounded bad and made the OP-1 chug). Everything plays on `tv[0]`;
  a saved mode value 2 reads as `MONO GLIDE`.
- In `render`, a note is new when `stamp[v] != t->seen_stamp`.
  - `MONO GLIDE`: if another key was held and the voice is still audible, the note is **legato** (no restart, the
    pitch slides by the glide time). Otherwise it restarts.
  - `MONO RETRIG`: always restarts, never glides.
  - On `note_off` of the owner in either mono mode, the newest key still held becomes the owner. In glide mode
    that is a legato return; in retrigger mode it is a restart.
- **A restart** sets phase 0, both envelopes to full, resets sag, and sets `restart`. From silence it also clears
  the filter and feedback state so two hits are sample-identical (there is a test for this).
- `finished()` tells the firmware when to free a voice. The engine has `"own_envelope": true`, so the OP-1's T2
  envelope does not run for it and velocity is applied by the engine.

### 5.3 The signal path

Per block, the knob laws are computed once (`exp2neg` is a small 2^-x table used for every exponential mapping).
Per sample:

1. **Pitch.** `inc[n] >> OCT_DOWN` (two octaves down) plus the glide offset, minus the sag, plus the pitch
   envelope (`KNOCK`, up to 16× the note's frequency, decaying over `KNOCK LEN`, 20 ms to 1.5 s).
2. **Feedback.** The phase the waves are read at is the oscillator's phase plus the average of the last two
   feedback samples times `fb`. `fb` is non-zero only in the bottom and top quarters of the shape knob.
3. **Oscillator.** Interpolated sine, a triangle and a saw, all rising through zero at phase 0. Shape is four
   quarters: feedback sine → sine → triangle → saw → feedback saw. It is **displayed** as −50 % to 150 % so that
   the plain waves read 0, 50 and 100.
4. **Sub source.** A sine read at the **unbent** phase (`p`, not `q`), so feedback never touches it.
5. **Amp envelope × stage-1 gain** in one multiply (`gg`, Q27), with the drive's bias added first. From here the
   signal is **Q18** (1.0 = 262144).
6. **Drive** ("fattener"), all scaled by the one knob:
   - bias 0 to 0.3 of the envelope's level, so the wave is clipped off-centre;
   - stage 1: gain 1 to 8×, soft saturation blended in over the first quarter of the knob;
   - AC coupling (a 7 Hz high-pass), faded in over the first eighth of the knob;
   - treble lift: the part above about 220 Hz again, 0 to 3×;
   - stage 2: gain 1 to 4×, then a saturation whose knee follows the clip knob.
7. **Tone.** Two one-pole low-passes in series, 12 kHz down to about 190 Hz. Exactly bypassed when open.
8. **Sub blend.** Crossfade toward the clean sine, up to 75 %.
9. **Feedback tap.** Taken here through a fixed soft clip, so the clip knob does not change how wild feedback is.
10. **Clipper, always last.** Driven 1.76× (soft) to 4× (hard); the knee goes from the cubic soft clip
    `x − (4/27)x³` to a brick wall. The 1.76× is chosen so that a plain sine with no drive just reaches the
    ceiling: the output is at full scale at every setting.
11. **Attack.** A linear fade of the finished signal, 0 to 100 ms.
12. **Restart declick.** On the first sample of a restart, `tail` is set to the difference from the last output
    sample and fades over 1.5 ms, so a note that cuts off a sounding one is continuous.
13. **Velocity and output gain,** back to 16 bits.
14. **Tail fade (0.9.9):** `t->fade`. The output level is the largest block-end sample over 16 blocks (`pk`, no
    per-sample cost). When the envelope's target is 0 (key up, or sustain 0) and `pk` is below `FADE_LSB` (96 LSB,
    -51 dBFS, scaled by velocity), the fade falls with a 15 ms time constant and ends the note. It rides on the
    attack's per-sample ramp (`F_MOD`). Found by the owner: a crusher with lots of gain after TYR8 made the last
    ~600 ms of a tail, a staircase of a few output levels, audible as digital noise.

**Sag** multiplies the frequency by a factor that decays every block, so pitch falls linearly in octaves over
time: up to two octaves over the amp decay's length, stopping three octaves down. It resets on a restart.

### 5.4 Fixed-point formats

This is where most of the subtle bugs were. A product must fit in 32 bits, so signal precision and coefficient
precision trade off.

| Quantity | Format |
|---|---|
| Envelopes (`lvl`, `pe`, `atk`, `sag`) | Q30 (`ONE30`) |
| Oscillator output, knob words | Q15 |
| Signal after the envelope | Q18 (`Q18_ONE`), saturations flatten at 1.5 (`Q18_LIM`) |
| Stage gains `g1`, `g2`, `lift` | Q8 |
| `mix`, `cpl`, `clip` | Q12 |
| Tone coefficient `tc`, `sub` | Q11 |
| Final clipper gain `cg` | Q10 |
| Filter states `hp`, `dcs` | Q18 << 8 |
| Screen geometry | Q10 px; trig from the table is Q15 |

- **Never write `x << n` on a value that can be negative.** The sanitizer flags it and it is undefined. Use `* n`.
  Right shifts of negatives are relied on as arithmetic (GCC), which is fine.
- `cube18()` computes the cubic term at 15-bit precision and subtracts it from an 18-bit `x`. That keeps a quiet
  tail linear and precise, because the cubic term vanishes for small `x`.
- `warm()` estimates where stage 1's output will average for a running note and starts the AC coupling there.
  Without it, a heavily driven hit loses its whole first half-wave (see section 9).

### 5.5 The screen

Since 0.9.6 everything on T1 is drawn by code (`tyr8.svg` is empty, as kastle's is): lines with
`op1_raster_line`, and the black rubber, the lug sides and the whitewall as **filled polygons written straight into
the raster's pixels** (`fill_poly`, even-odd scanline, RGB565, the layout dx_gfx.c uses; each fill feeds
`op1_screen_hash` so the frame skip sees it). Pixel fills and lines are assumed to land in call order, as in dx.
**Not yet seen on hardware.**

- **Projection:** dimetric, one for everything: travel `(0.894, -0.447)`, up `(0, -1)`, depth `(-0.67, -0.335)`.
  `wp()` maps a wheel point (angle, radius, depth) to the screen. The view direction is `(-0.749, 0.67, 1)`, which
  decides which lug faces show.
- **Painter's order:** road, the tyre's convex hull in black (hides the road), the far face's outline, the 16 lugs
  sorted back to front (visible faces only, each filled then outlined), the near face filled black over the band,
  then the near face's lines, sidewall or whitewall, rim, spokes, hub or spinner, valve.
- **Lugs:** 16 blocks; even ones on the near half of the tread, odd ones on the far half (staggered). Their height
  is drive or clip, whichever is higher (the owner's choice); at 0 there are none (a racing slick, outlined by the
  silhouette), just above 0 they lie flat and read as a grooved grid. Tread width `TYRE_W` 48, size 0.55 to 0.86.
- **Flat (sag):** in `wp()`, tyre points (beyond the rim) below a floor are pushed onto it and the low sidewall
  bulges along the road; the hub drops by the same amount so the flat sits on the road. The rim is drawn with an
  unflattened copy of the wheel, so it stays round.
- **Spokes (sub):** 0 to 40 laced spokes (`vis_spokes`), whitewall above 50 %, gold spinner above 80 %, five lug
  nuts while there are fewer than 12 spokes.
- **Size:** decay, 0.55 to 0.92. **Spin, bounce, ridges:** as before, the ridges on the road's cross lines.
- **Road (0.9.8):** a band `ROAD_P0`..`ROAD_P1` deep and `ROAD_LEN` each way, every line clipped to the screen
  (above y = 150) and cut around the label blocks (`KEEP_OUT`, `road_cut`). The part behind the near face is drawn
  before the wheel, the part in front after it. A dashed centre line scrolls; ridges with the knock.
- **Layout limits:** the largest, flattest tyre stays clear of the label blocks and above y = 150; the tests check
  the real silhouette, and every road line, against the label text.
- **Cost:** 400 to 600 raster lines a frame plus about 35 fills, on the GUI thread. If the OP-1's screen gets slow,
  thin the rings (`RING_N`) or the spokes first.

---

## 6. The test suite

`test/tyr8_test.c` includes `tyr8.c` directly, drives six voices the way the firmware would, records the summed
output, and checks claims. It also runs the screen for a few hundred frames and dumps what it drew.

Helpers worth knowing:

- `set(shape, knock, knock_len, decay, drive, clip, sustain, glide)`: the sound's words. `SINE` is 8192.
- `mode(m)`, `third(sub, tone, attack)`, `sag(v)`: the record.
- `on(voice, hz, velocity)`: `hz` is the frequency that should **sound**; the helper multiplies by 4 for the
  two-octave shift. `offk(voice)`, `run_ms(ms)`, `reset()`.
- `freq(a, b)` (zero crossings), `peak(a, b)`, `tail_snr(a, b, f)` (how cleanly a decaying tone repeats cycle to
  cycle, in dB), `sine_part(a, b, f)` (fraction of the signal that is a sine at `f`).
- `frame(label)` runs one screen update; frames with a new label become a PNG.

What it covers: one-shot behaviour, identical hits, the knock, decay and sustain ranges, mono and legato and
glide, all three play modes, the clipper bounds, the drive's brightness and asymmetry, attack, tone, sub, sag,
feedback at both ends, tail cleanliness at three drive settings, the input hook, and screen layout (labels in
their corners and clear of the wheel, nothing below the ground or off screen).

Limits to keep in mind:

- The mocks are mine. `op1_engine_record`, the input events, Shift and the screen calls behave the way I read the
  headers, not necessarily the way the firmware does.
- Some thresholds were set from measured values after a change. They detect regressions; they are not
  independent specifications.
- Nothing measures CPU cost.

---

## 7. What the owner asked for, in order

Useful for understanding why things are the way they are. Each item was a direct request.

1. Build the plugin from a handoff spec (monophonic 808, 8 parameters, rolling tyre screen).
2. Three-quarter perspective for the tyre.
3. Colour-coded labels per encoder; a much dirtier drive, "like Sausage Fattener".
4. Play two octaves lower by default.
5. Much louder: "it should damn near blow your speakers out".
6. Orange click to switch mono glide / mono retrigger / poly.
7. Labels spread to the four corners.
8. Three more controls on the other three clicks (sub, tone, attack), with click-to-switch behaviour.
9. `P.DEC` renamed because it meant nothing to them (now `KNOCK LEN`).
10. The shape knob did nothing audible → extended to a saw.
11. Mono retrigger should retrigger the held key on release.
12. The attack did nothing audible → moved after the clipper, 100 ms.
13. Atonal "end of the world" drill subs: **clip must be last, no matter what**; feedback at both ends of the
    shape knob; longer, deeper knock; sag.
14. Shape displayed as −50 % to 150 %.
15. Digital noise on the decay tail → precision rework.
16. The clip knob did nothing audible; verify the sub is always a sine.
17. Repo, README, Reddit post, demo video.

---

## 8. Design decisions and the reasons

- **Own envelope** (`own_envelope`): an 808 needs a one-shot that ignores key length, and the stock ADSR's
  release would chop the tail. Cost: T2 does nothing, and the engine must report `finished`.
- **Envelope before the drive:** so the tail cleans up as it decays. Cost: quiet tails get amplified, which is
  what the precision work in section 9 fixed.
- **Shift + turn instead of a second page:** an engine's page 2 only has three words for four knobs.
- **The handoff's clipper formula was wrong.** `x − x³/3` clamped at ±1.5 peaks at 0.667 and folds back to 0.375.
  `x − (4/27)x³` does what its comment claimed: unity gain when small, exactly ±1 with a flat top at ±1.5.
- **Clip is last, except the sub (0.9.7).** The owner's rule for the drive chain. The sub used to go through the
  clipper too, and measuring it showed why that failed: the clipper is driven so hard that the sum is near-square
  whatever is added, so turning the sub up *lowered* the fundamental's share (58 % to 47 % at the defaults). Since
  0.9.7 the sub's clean sine is crossfaded in after the clipper (up to 0.6, `SUB_MAX`); the output still never
  exceeds full scale. The owner asked for the sub to be brought up and was told about the exception.
- **The feedback tap is before the final clipper, through a fixed soft clip.** When it was after, the clip knob
  also set how unstable the feedback was, and a small amount lost its pitch at hard settings.
- **Mono retrigger never glides.** The glide control only applies in `MONO GLIDE`.
- **The tyre's size follows amp decay** (the original spec allowed decay or drive).

---

## 9. Bugs found, and what they teach

| Symptom the owner reported | Cause | Fix |
|---|---|---|
| Shape knob inaudible | Sine and triangle are nearly the same through a clipper (triangle measured *duller*) | Extended to a saw, later to feedback |
| Attack inaudible | The fade was before the drive, whose gain squashed any ramp to a few ms | Fade the finished signal after the clipper |
| (found while testing the attack) | Heavily driven hits were silent for their first half-wave, up to 9 ms late | `warm()`: start the AC coupling where it will settle |
| Digital noise on the tail | Three separate rounding problems amplified by the drive: 16-bit signal precision before the gain, an envelope that stepped once per block, a bias term that stepped coarsely | Q18 signal path, gain folded into the envelope multiply, Q30 per-sample ramp |
| Clip knob inaudible | An earlier soft saturation had already shaped the signal | Clip now sets both knees and the final drive |
| Sub "always a sine?" | It was read at the feedback-bent phase | Read at the unbent phase |
| (found in tests) | A ground bump passing under the wheel cancelled a note's bounce | Bump kicks only when the wheel is at rest |
| (found by sanitizer) | Left shifts of negative values | Multiply instead |

The pattern: **a control that passes its test can still be inaudible.** Half of these were features that worked
exactly as coded and did nothing a person could hear, because of what came after them in the chain. When you add
or change a control, measure its effect on the *final output* at the *default* settings, not in isolation, and
write that as the test. Several tests in the suite now do this ("is heard at the default drive").

The other pattern: an early version of the tail-noise test was misleading, because at high drive the wave's shape
changes as it decays and that looked like noise. Check what a metric is really measuring before trusting it.

---

## 10. Known issues and risks

- **CPU is unmeasured on hardware.** The SDK's cycle meter (`plugin_cost.py`, `tier2.py` and its emulator) is not
  in the public bundle. A static count of the Blackfin build: before 0.9.5 the per-sample loop was about 376
  instructions (36 32-bit multiplies), roughly 40,000 per voice block. The bundled plugins declare 3,500 to 25,000
  in the same unit, and the SDK's figure of 0.478 % per 1,000 makes that about 20 % a voice. 0.9.5 builds the loop
  in 16 versions (`run_block`, flags `F_TONE`, `F_SUB`, `F_FB`, `F_MOD`) so that unused parts drop out: 218
  instructions a sample at the defaults, up to 372 with everything on. Code size went from 10.8 to 23.8 KB of
  text. `-O3`, unrolling and scheduling flags made no difference to the loop. Poly was later removed (0.9.9). The test prints an `audio hash` of
  every sample it renders: a speed-up must leave it unchanged (`751ed29c` at 0.9.5, `f4c381f0` from 0.9.7, when the sub
  moved; `46bb11db` from 0.9.9, the tail fade).
- **`blue_clip` (Shift + blue on drive or clip) is global and unsaved.** It carries across sounds and slots and
  resets at power-on. The label shows it only while Shift is held.
- **The T2 page and the Shift-turn takeover (0.9.4) are untested on hardware.** They follow the SDK headers and
  pinboard's example. Check that T2 shows TYR8's page, that its knobs move the curve, and that every Shift + turn on T1
  moves its value at a sensible rate.
- **Switching play mode while notes sound** restarts or cuts them.
- **Sag on held notes** keeps falling for as long as the key is down (to a floor three octaves down) when sustain
  is up.
- **Feedback is deliberately chaotic at the extremes** and its character depends on drive. It has not been tuned
  by ear across the range.
- **The saw is not band-limited.** At bass pitches the aliasing is minor.
- **Saved-sound compatibility has been broken several times during development:** the two-octave shift, the shape
  knob's layout (plain sine moved from 0 to 8192), the drive's range, `KNOCK LEN`'s range, the clip knob's
  strength. Nobody else had the plugin then. **After the public release this must stop**; changing what a saved
  value means needs a migration or a new ID, and the ID must not change.
- **The key tracking assumes** the firmware calls `note_off` per voice and keeps a voice allocated while
  `finished` returns 0. A heartbeat guards against voices killed without a `note_off`. This has not been
  stress-tested on hardware with slot changes mid-note.
- **The author name "David Hanover"** in `plugin.json`, the copyright lines in `tyr8.c` and `gen_svg.py`, and the
  commit was inferred from the owner's email address and matched their git config. They never explicitly
  confirmed it for publication.
- **`docs/screen.png` is a PC preview,** captioned as such. A photo or capture of the real screen would be better.

---

## 11. Ideas that were discussed and not built

- **Velocity to drive or knock.** Skipped: the OP-1's keys always play at full velocity, so it only helps with
  sequencers and external MIDI.
- **A noise or click transient layer.** Skipped twice in favour of the pitch knock.
- **Boosting the summed synth bus** for more level. The SDK has a hook (`"sum_block"`); I never checked whether
  it permits gain above full scale.
- **Going straight from sine to saw,** dropping the triangle, if the first half of the plain range still feels
  too subtle.
- **A third value for orange.** Orange's push is the play mode, so it has no third knob value.
- **Submitting the plugin to the Hundekraft workshop catalogue.** The maintainer accepts plugins and tests on
  hardware; that would reach more users than Reddit.

---

## 12. Release and promotion state

- **GitHub release:** not made. Tag `v0.9.3`, title "TYR8 0.9.3", attach `build/tyr8.knl`, paste the
  `CHANGELOG.md` entry. Rebuild first if anything changed; the committed source builds the 18,700-byte file.
- **Reddit post:** `build/reddit_post.md`, for r/OP1users. Recommended as a video post with the write-up as the
  first comment. Before posting: remove or fill the two placeholders, confirm the resource-monitor shortcut it
  mentions (Shift + help, then T1; that comes from the op1r docs and was never confirmed on Hundekraft), and
  make the release so the download link works.
- **Demo video:** the owner supplied `build/1116262026.mp4` (camera footage) and `build/Document 1.wav` (mastered
  audio). I replaced the audio and synced it:
  - The WAV starts 0.4189 s before the video's timeline; measured by cross-correlation and stable to half a
    millisecond over the whole clip. The music repeats every 3.117 s, so short-window correlation gives false
    matches; use the whole file or windows of 20 s.
  - Both outputs are trimmed to 99.75 s (the end of the audio).
  - The original has a −90° rotation tag but the pixels are already an upright landscape shot, so the outputs
    drop the tag (`-display_rotation 0`) instead of rotating.
  - `tyr8_demo.mp4` is a stream copy of the picture; `tyr8_demo_reddit.mp4` is H.264 CRF 23, 48 MB.

---

## 13. Working with the owner

- They test on their own OP-1 and report by ear, in short messages. Their reports have been reliable: every
  "this knob doesn't do much" turned out to be true once measured. Believe them and measure, then explain the
  cause in plain terms.
- They want **audible, obvious** results. A subtle control reads as a broken one.
- They decide quickly and state preferences as rules ("the clip should be last, no matter what"). Follow the
  rule, and tell them what it costs elsewhere.
- They are comfortable with discussion before code when they ask for it ("let's discuss"), and will pick from
  options if you give a recommendation.
- They like American spelling in public-facing text ("tire"). The source and README use "tyre" in places because
  I wrote them that way; the Reddit post uses "tire". Worth making consistent if they notice.
- After each change they want: what changed, the new file size and version, and what is still untested. Say
  plainly when something has only been checked on the PC.
- Outward-facing actions (pushing, releasing, posting) were done only after they supplied the target or asked.
  Keep it that way.

---

## 14. Checklist for making a change

1. Read the relevant part of `sdk/.../synth_sdk/op1_plugin.h` and check `kn_exports.c` before using any new call.
2. Change `tyr8.c` (and `gen_svg.py` for art). No floats, no per-sample division or 64-bit maths, no libc, no
   left shifts of signed values.
3. Check every new multiply fits in 32 bits at the extremes of both operands. Write the worst case in a comment.
4. Add or update a test in `test/tyr8_test.c` that measures the effect on the final output at realistic settings.
5. `./build.sh test`, and look at the PNGs if the screen changed (`build/out/screen_*.png`).
6. If a saved value's meaning changes, stop and think about existing sounds (section 10).
7. Update `README.md` (user-facing control tables) and the top comment of `tyr8.c`.
8. Bump `version` in `plugin.json`, add a `CHANGELOG.md` entry, `./build.sh`.
9. Tell the owner the size and version, and what is unverified. Commit and push only when asked.
