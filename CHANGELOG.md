# Changelog

## 0.9.24
- Fixed: a click on every key press in the OP-1's poly mode, legato too, at any setting. TYR8 is mono and moved its
  one sound to each new key's OP-1 voice, and the OP-1's handling of each voice's output made the move audible (its
  mono mode, one voice, never clicked). The sound now stays on one voice for as long as it rings; the newest key hands
  over its pitch, velocity and note. A note can come one audio block (1.45 ms) later than its key, depending on the
  OP-1's voice order.

## 0.9.23
- Fixed: on a blown-up slick the red strain line still stuck out of the tyre by half its width (the outline ran
  through its middle). The outline now takes it in with 2 px to spare, and a flat's valve tip stays 2 px inside.

## 0.9.22
- A new TYR8 starts plain: shape 0 % (a sine), knock 0, knock length at its shortest, tone open, sag 0, drive 0, clip
  0, sub 0, MONO GLIDE with the glide in the middle (250 ms). Saved sounds keep their settings.

## 0.9.21
- Fixed: kicked hard (full knock, or notes landing while it is still in the air), a big blown-up tyre bounced into the
  labels, and lines it drew there stuck (the labels are a still layer). The tyre's top now stops at 24 px, as high as
  the largest round tyre's highest kick goes.
- Fixed: fully blown up, the sidewall's red strain line could run off the tyre; the outline now takes in the
  ballooning wall. And on a fully flat tyre the valve poked out under it once a turn; it squashes with the flat.

## 0.9.20
- README: set the OP-1's own play mode (Shift + T2) to poly or mono, not unison. Unison was behind the clicks on
  every note and the missing retrigger knock found on hardware; a slot keeps the play mode of its previous sound.
- The output is 2.5 dB louder, peaking at -4 dB (0.9.17's -6.5 was too quiet). After the OP-1's 35 Hz bus high-pass
  an ordinary note may now go about 1.5 dB over at its onset, the heaviest patches about 2 (they went 5.5 and 6 over
  at full scale).

## 0.9.19
- Below 0 the tyre is blown up much further, on the verge of bursting: the tread out by up to 16 % of the radius
  (a mid-size tyre about 10 px taller), the sidewalls ballooning, from -60 % a blister swelling on the tread (it turns
  with the tyre), from -70 % a quiver, from -80 % the sidewall line strained red. The largest tyre is held to 7 px
  more so it stays on the screen.

## 0.9.18
- SAG's knob is spread over what was useful: 100 % is 0.9.17's 70 % (about 11 semitones down over the decay) and
  -100 % its -50 % (about 7 up), the same curve between.

## 0.9.17
- Fixed: every note clipped on the OP-1 (red, a click). TYR8 sent out a square-ish bass at full scale, and the synth
  bus's 35 Hz high-pass after it throws the edges and the onset up to 6 dB over. The output now peaks at -6.5 dB, so it
  is 6.5 dB quieter; turn the OP-1's volume up to match.

## 0.9.16
- Fixed: with the OP-1 starting more than one voice a key (unison), no note after the first restarted (no knock, a
  glide from the old pitch instead) until the last tail had died away. The voices of one press were taken as a key
  held under another. Now only a key pressed before this one, and still down, makes a note legato.
- Fixed: a restart over a loud tail could add up past full scale and wrap into a click; the output is held at full
  scale.

## 0.9.15
- SUB is an octave down: a clean sine at half the note's pitch, while that is at 45 Hz or more. Where it would fall
  under the synth bus's 35 Hz high-pass it stays at the note's own pitch (40 Hz and under), with a crossfade between,
  ramped so a glide or a sag through it doesn't click. It follows glide and sag, not the knock.
- SUB goes all the way: at 100 % the output is only the clean sine (it stopped at 60 %).

## 0.9.13
- SAG goes from -100 % to 100 %. Below 0 the pitch rises while the note lasts (up to an octave), and the tyre blows
  up: taller, its sidewalls ballooning out. Sounds saved before keep their sag.
- SAG's law is gentler, so the whole turn is heard: about 3 semitones over the decay at 25 %, 7 at 50 %, 12 at 75 %
  and 18 at 100 % (it was two octaves at 100 %, linear, and mostly done by a third of the turn).
- CLIP's last quarter slams the clipper: from 4x at 75 % up to 16x (+12 dB) at 100 %, so the note stays a square
  wall far down its tail. At 100 % it squares a closed tone bright again; back off to 75 % for a dark wall.

## 0.9.12
A lighter screen, the same picture.

- The labels are a still layer: the firmware keeps them and redraws only the moving area, so their ~170 lines are
  drawn only when a value, Shift or the mode changes.
- Nothing the tyre covers is drawn: the street behind it is cut at its outline, and lug faces behind the near face
  are skipped.
- Circles use segments by size (10 for the hub, up to 24).
- Together about 35-38 % fewer lines on an ordinary frame. The largest tyre is a little smaller (0.81) so that its
  widest bulge stays clear of the label layer.

## 0.9.11
- The road is a street: wider, with a curb, a sidewalk and grass behind it, all scrolling.
- Knock makes the road rough instead of drawing ridges: rocks in the tyre's path, which it hops over, cracks in the
  pavement, and from 40 % weeds growing in them.

## 0.9.10
- The back of the tyre is hidden by the tyre: its far rim is drawn only where it shows, along the outline, not
  across the tread at the top and bottom (most visible on the slick).

## 0.9.9
- POLY is gone: it sounded bad and six full voices were too much for the OP-1. Orange's push switches between
  `MONO GLIDE` and `MONO RETRIG`; a sound saved in POLY plays in `MONO GLIDE`.
- Orange turns `SAG`; Shift + orange is `GLIDE`. Sounds saved before keep their sag and glide in each other's places.
- The decay tail no longer trails off through the last few bits of the 16-bit output (about 600 ms of a coarse
  staircase at the defaults, heard as digital noise when a crusher or a lot of gain comes after TYR8). Once a note is
  dying away and below about -51 dBFS, it fades to exact silence in about 70 ms. Held notes are not affected.

## 0.9.8
- The road runs the whole diagonal of the screen, wider, with its lines cut around the label blocks, a scrolling
  dashed centre line, and the part in front of the tyre drawn over it. Knock ridges are sparser.

## 0.9.7
- Sub is blended in after the clipper, so it stays a clean sine and is heard: at the default drive the note's
  fundamental goes from 83 % of the output at sub 0 to 96 % at full (before, turning the sub up made it thinner,
  because the clipper squared it again). Full sub is about 2 dB less loud and much rounder.
- The tyre is wider. Its tread follows drive or clip, whichever is higher, and is a smooth racing slick when both
  are at 0.

## 0.9.6
New screen art.

- The road is isometric now, running diagonally under the tyre in the same projection, with lines that scroll, ridges
  with the knock, and the tyre's shadow when it bounces.
- The tyre is drawn in code: filled black rubber, staggered off-road blocks across the tread with clip (a grooved
  slick at 0).
- Sag flattens the tyre onto the road; sub grows lowrider wire spokes, then a whitewall with an orange pinstripe,
  then a gold knock-off spinner.

## 0.9.5
Lighter on the CPU, same sound.

- The per-sample loop is built in 16 versions, one for each combination of tone, sub, feedback and pitch/attack
  moves, so the parts not in use cost nothing. At the default settings a voice runs about 218 instructions a sample
  instead of 376 (static count on the Blackfin build), about 40 % less. The output is sample-identical.
- Larger labels on both screens.

## 0.9.4
New control layout, with no push pages.

- T2 is now TYR8's envelope page: attack (0 to 1 s), decay, sustain and a new release (10 ms to 4 s, or HOLD to ring
  out the decay). The OP-1's own T2 knobs turn it and it is saved with the sound as the sound's envelope.
- T1: blue shape / Shift drive or clip (Shift + push blue switches), green knock / Shift knock length, white tone /
  Shift sub, orange glide / Shift sag. Push orange steps the play mode, with or without Shift.
- Tone, sub and sag are fine controls instead of 64 steps. The tone knob is respread so that the whole turn is heard
  on a driven bass (it reaches 140 Hz, was 190 Hz).
- Sounds saved with 0.9.3 need their envelope, tone, glide and knock length set again.

## 0.9.3
First public version.

- Monophonic / polyphonic 808 bass engine with its own amp envelope, pitch knock, glide and sag.
- Shape from feedback sine through sine, triangle and saw to feedback saw.
- Two-stage drive, tone filter, clean sub, and a soft-to-hard clipper that is always last.
- Three play modes: mono glide, mono retrigger, poly.
- Screen: a rolling tyre in three-quarter view with a labelled readout for each encoder.
