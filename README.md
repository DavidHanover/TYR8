# TYR8

An 808 bass synth for the original Teenage Engineering OP-1 running the
[Hundekraft](https://hundekraft.github.io/op1-workshop/) custom firmware. It is one plugin file, `tyr8.knl`,
installed through the Hundekraft workshop.

If you enjoy this, toss me some [cheddar](https://buymeacoffee.com/davidhanover)!!

![TYR8 screen](docs/screen.png)

*Screen preview rendered on a PC; the OP-1's own renderer may differ in detail.*

- **Needs:** an original OP-1 (not the OP-1 field) on Hundekraft 3.3 or later. Older cores refuse the file.
- **Status:** early release. A plugin runs inside the OP-1's audio engine, and a faulty one can hang the unit until
  it is switched off. Back up first and use it at your own risk.

## Install

1. Download `tyr8.knl` from the [releases](../../releases) page.
2. Open the [workshop](https://hundekraft.github.io/op1-workshop/) in a browser with Web MIDI (Chrome or Edge) and
   connect the OP-1 by USB.
3. Under **Your plugins**, add `tyr8.knl`. Press **Review changes**, then **Send to OP-1**, then **T1** on the OP-1.
4. After it restarts: **synth → Shift + T1 → tyr8**.
5. Check the slot's play mode: **Shift + T2** opens the OP-1's play mode page. Use **poly** or **mono**, not
   **unison**. A slot keeps the play mode of whatever sound it had before, and in unison the OP-1 steals and fades
   its voices on every key, so every note clicks, and releasing a key doesn't retrigger the one held under it. TYR8
   does its own mono either way (push orange for MONO GLIDE or MONO RETRIG).

To update, add the newer file the same way; it replaces the old one. To remove it, use **Remove** under **Your
plugins**. Sounds saved with tyr8 keep their name and come back if you reinstall.

## Controls

The four encoders on **T1** shape the sound, and the OP-1's own envelope page, **T2**, sets the volume envelope.
Each T1 encoder has a value on a plain turn and a second with **Shift** held. The corner of the screen in that
encoder's colour shows its name and setting.

### T1

| Encoder | Turn | Shift + turn |
|---|---|---|
| Blue | `SHAPE`: the wave. 0 % sine, 50 % triangle, 100 % saw; below 0 % and above 100 % the output feeds back into the oscillator, until there is no steady pitch | `DRIVE`: distortion before the clipper, up to 30 dB: fatter, then gritty. **Shift + push blue** switches this to `CLIP`, the final clipper from a rounded soft clip to a hard brick wall (squarer and brighter); its last quarter slams it, up to 16x at 100 %; push again for drive |
| Green | `KNOCK`: how far the pitch starts above the note before dropping onto it, up to 16 times its frequency | `KNOCK LEN`: how long that drop takes, 20 ms (a click) to 1.5 s (a slow dive) |
| White | `TONE`: a low-pass after the drive. 100 % is open; lower is darker, down to 140 Hz | `SUB`: blends a clean sine in after the clipper, so it stays clean: an octave below the note, or at the note itself where an octave down would be under the OP-1's 35 Hz bus filter (it crossfades between them around 40 to 45 Hz). At full it is the whole output |
| Orange | `SAG`, -100 to 100 %: above 0 the pitch falls while the note lasts (about 2 semitones over the decay at 25 %, 4 at 50 %, 7 at 75 %, 11 at 100 %); below 0 it rises, up to about 7 semitones at -100 % | `GLIDE`: how long the pitch slides between overlapping notes, 0 to 500 ms. `MONO GLIDE` only |

**Push orange** (with or without Shift) to switch between the two play modes. The current one is shown under orange's label.

| Mode | What it does |
|---|---|
| `MONO GLIDE` | One voice. A note played while another is held and still sounding does not restart: the pitch glides to it. Letting go glides back to the key still held. |
| `MONO RETRIG` | One voice. Every note restarts on its own pitch with no glide. Letting go restarts the key still held. |

### T2: envelope

TYR8 draws its own envelope page, with the curve in the encoders' colours.

| Encoder | Label | Range | What it does |
|---|---|---|---|
| Blue | `ATTACK` | 0 → 1 s | Fades the note in. Most of the turn is under 300 ms. |
| Green | `DECAY` | 0.1 → 4 s | How fast the level falls from the hit to the sustain level. It also sets the size of the tyre on T1. |
| White | `SUSTAIN` | 0 → 100 % | The level a held note settles at. |
| Orange | `RELEASE` | 10 ms → 4 s, then `HOLD` | How fast the note dies after the key goes up. At the top of the turn, `HOLD`, the note carries on at the decay rate instead. |

**For the classic 808 one-shot,** set `SUSTAIN` to 0 and `RELEASE` to `HOLD`: every note rings its full decay
however short the key press. The T2 values are stored with the sound like any OP-1 envelope, so a sound starts with
whatever envelope the slot had before; set those two first.

### The screen

A tyre rolls along an isometric street that runs the whole diagonal of the screen, between the labels: the road,
a curb, a sidewalk and grass behind it, all scrolling past. Each part follows a setting:

| What you see | Follows |
|---|---|
| Size, small car tyre to monster truck | `DECAY` (T2) |
| Tread: a smooth racing slick at 0, then staggered off-road blocks rising | `DRIVE` or `CLIP`, whichever is higher |
| A flat: the bottom squashes onto the road and the sidewall bulges; below 0 the tyre is blown up, taller and ballooning | `SAG` |
| Rim, from a plain dish to laced lowrider wire spokes, then a whitewall with an orange pinstripe, then a gold knock-off spinner | `SUB` |
| Spin, and the road's dashed centre line scrolling under it | the pitch, while a note sounds |
| A bounce on every new note, and a rough road: rocks in the tyre's path (it hops over them), cracks in the pavement, and from 40 % weeds in the cracks | `KNOCK` |

### Good to know

- **Pitch:** the engine sounds two octaves below the key, so the OP-1's octave 0 is already bass.
- **Loudness:** the clipper is always last for everything but the sub, and always driven, so the output is at the
  same peak level at any setting: 4 dB under full scale. The OP-1's synth bus has a 35 Hz high-pass after the
  synths, and a square-ish bass through it overshoots by up to 6 dB; at full scale every note clipped (at -4 dB a hard-driven note can still touch it). Drive and clip
  add density, not peak level. Turning the sub up trades some density for
  weight: about 2 dB less loudness at full.
- **Saving:** every value above, and the play mode, is saved with the sound. Whether Shift + blue is on drive or
  clip is not.
- **Tail:** once a note is dying away and has fallen below about −51 dBFS, it fades to silence over about 70 ms
  instead of trailing off through the last few bits of the 16-bit output, which turns to grit when an effect after
  TYR8 brings it up. Held notes at their sustain level are left alone.
- **Lowest notes:** the OP-1 filters its synth output below about 35 Hz, so the very lowest notes lose weight.
- **Sounds from 0.9.3** load with some values in new places: they need their envelope, tone, glide and knock length
  set again.

## Signal path

Oscillator (with feedback from the output) → amp envelope → drive → tone → clipper → sub blended in → attack fade.

## Build from source

```sh
./build.sh          # fetches the SDK on first run, runs the checks, writes build/tyr8.knl
./build.sh test     # checks only, no Docker: writes build/out/*.wav and build/out/screen_*.png
```

Needs gcc, python3, curl, and Docker for the plugin file. The first full build makes a `bfin-elf-toolchain` image
from `toolchain/Dockerfile` (Blackfin GCC 13.3.0; about five minutes, once). The script downloads the Hundekraft 3.3
source and SDK into `sdk/`, verifies its checksum, and packs with the SDK's own `tools/kennel/pack.py`.

- `plugin/tyr8/` — `tyr8.c` (engine and screen), `plugin.json`, and `gen_svg.py`, which writes the (empty)
  `tyr8.svg`: the screen is drawn in code.
- `test/` — checks that build the plugin for the PC and test what it promises, plus a preview renderer for the
  screen. These are stand-ins for the firmware, not an emulator.
- `toolchain/` — the cross-compiler image.

The audio code is integer-only fixed point, as the SDK requires: it runs in the OP-1's audio interrupt at 88.2 kHz.

## License and credits

GPL-3.0-or-later with the OP-1 firmware permission; see `LICENSE` and `NOTICE`. Built on the kennel plugin SDK by
Hundekraft and op1r by pcppcp. Not affiliated with Teenage Engineering.
