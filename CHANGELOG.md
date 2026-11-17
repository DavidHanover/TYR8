# Changelog

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
