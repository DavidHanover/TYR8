# Changelog

## 0.9.40
- SUB and the sample layer no longer fight. SUB crossfades the output to its clean sine (at 100 % all of it), which
  took the sample with it: SUB 100 % and SAMPLE 100 % left almost nothing, and at lower SAMPLE the sample was lost
  under the sine. Now the sub steps aside while a sample plays (a level follower on the sample: all the way from an
  eighth of full scale, back over a few ms after it), and above 75 % SAMPLE it fades out with the 808. With no
  sample it is as it was.
- Grass beyond the near sidewalk too.
- The tyre's new thing is longer.

## 0.9.39
- The street has a sidewalk on the near side too: a curb at the road's edge and a sidewalk with its joints, in front
  of the tyre, scrolling with it.
- The road's centre line is yellow.
- Something new on the tyre, for whoever goes looking.

## 0.9.38
- The top of `SAMPLE` crossfades before the clipper: from 75 to 100 % the 808 and its sub fade out after the drive
  and tone, and the sample carries on through its drive and the clipper, so DRIVE and CLIP still shape it at 100 %.
  The clean path straight to the output (0.9.33) is gone: it was there to make the sample hit, and with the
  sampler's gain read right (0.9.37) it hits through the clipper.

## 0.9.37
- The sampler's gain is taken as the sampler plays it: a quarter of its knob is 0 dB and the top +24 dB, with the
  sampler's hard clip at full scale. TYR8 took the top of the knob as 0 dB, so a sample boosted on the sampler came
  out up to 16x (24 dB) quieter in TYR8, and never clipped. The gain and clip come first, as on the sampler, then SMP
  HPF and the rest: at SAMPLE 100 % a slammed, squared-off sample on the sampler is slammed and squared off in TYR8.
  Sounds whose source sampler has its gain above a quarter now play the sample louder.

## 0.9.36
- The play mode moved to T2: push orange there to switch between MONO GLIDE and MONO RETRIG, shown under RELEASE.
  A T1 orange push no longer does anything.
- T1's lower labels (white and orange) sit in the bottom corners, as far from the edge as the top ones are.

## 0.9.35
- The sample's Shift + turns moved: `SMP TUNE` is Shift + blue on blue's DRIVE page (it was Shift + green), the attack
  fade is Shift + green on the sample page, renamed `SMP ATK` (it was `SAMPATT` on Shift + white), and `SMP HPF` is
  Shift + white on the sample page (it was Shift + blue). The plain turns are as they were: DRIVE, SAMPLE, S.TONE.

## 0.9.34
- Green's push swaps between knock and the sample layer with or without Shift, as blue's and white's do. Shift +
  green push on the sample used to flip the sample's direction instead, so green couldn't be swapped back with Shift
  held. The flip is gone: the sample plays the way the sampler is set to (sounds saved with it flipped play the
  sampler's way).

## 0.9.33
- The top of `SAMPLE` crossfades to the sample alone. Up to 75 % it is layered on the 808 as before (75 % is the old
  100 %'s layering at a little over half its level). From 75 to 100 % the body and sub fade out after the clipper and
  the sample crosses to a clean path straight to the output, at the sampler's own level (recording x its gain x
  velocity), not saturated or clipped: at 100 % it hits as it does on the sampler. SMP HPF, S.TONE, SAMPATT, SMP TUNE
  and ATTACK apply to it, DRIVE and CLIP don't. With no recording found the note plays as usual. A new note that cuts
  the clean sample carries on from it over 1.5 ms (no click), and at 100 % the voice is free once the sample is done.
  Sounds saved with SAMPLE above 75 % now play with less body under the sample.

## 0.9.32
- `SAMPATT` turns at full speed, 32 detents end to end (it was 128, and the first 10 stayed under 1 ms): a quarter of
  the turn is 6 ms, half 25 ms, all of it 100 ms.

## 0.9.31
- `SAMPATT` fades the sample in after its drive and `S.TONE`, not before them. Before the drive, the drive pushed the
  faded start back up: at full drive a 25 ms fade left the click's first 4 ms at about 90 % of their treble. Now the
  fade is the same at any drive (under 1 % there).

## 0.9.30
- The sample layer's pitch follows the key's pitch block by block, as the OP-1's sampler does, instead of taking it
  from the note's first sample. A new note's pitch can still be on its way from the note before (portamento, or the
  OP-1's own smoothing), so a low note played after a high one played its sample a little sharp for a hit or two.
  A legato note (MONO GLIDE) doesn't bend the sample still playing from the note before.

## 0.9.29
- Blue has two pages, switched by a blue push (with or without Shift): SHAPE with CLIP on Shift, and DRIVE with
  `SMP HPF` on Shift. Shift + blue push used to switch Shift + blue between drive and clip.
- `SMP HPF`: a 12 dB / octave high-pass on the sample layer before its drive, off at 0, then 30 Hz to 3 kHz, so a
  sample's low end can be taken out to sit on the 808. Saved with the sound (record 14).

## 0.9.28
- The sample layer has its own tone: push white to swap TONE and SUB for `S.TONE` (the sample's own low-pass) and
  `SAMPATT` (a fade-in, 0 .. 100 ms, rolling off its attack); push again to swap back. The sample now runs through
  its own copy of the drive and joins the note after TONE, before the clipper, so TONE no longer darkens it. When it
  ends, its chain rings out for 12 ms and fades over 12 more.
- `KNOCK LEN` is `KNCKLEN`: at values like 855 ms the name ran into the value. A check now holds every label clear of
  its longest value.

## 0.9.27
- The sample layer starts from the sampler's own settings: its tune (the sample follows the key you play, at the
  pitch the sampler would give that key), its gain (scales SAMPLE) and its direction (reverse plays backwards). TYR8's
  controls go from there: SMP TUNE is now -24 .. +24 semitones on top of the sampler's tune, and Shift + green push
  (on the sample) plays it the other way round from the sampler; SAMPLE reads SMP REV while it plays backwards.
  Saved with the sound (record 9 now holds 25 + semitones, record 11 the flip).

## 0.9.26
- The sample layer follows the sampler's own trim: it plays between the start and end points set on the sampler's
  page (the slot's knob words 0 and 3), picked up on the next note. A slot that no longer runs the sampler plays its
  whole recording. The sampler's tune, loop and envelope still don't apply.

## 0.9.25
- A sample layer: push green to swap it over to `SAMPLE` (level, 0 off .. 100 %) and, with Shift, `SMP TUNE`
  (-12 .. +12 semitones); push again for knock and knock length. It plays a recording made with the OP-1's sampler on
  another slot (the silence before it skipped), once on every new note, mixed in at the oscillator's stage after the
  amp envelope: drive, tone and the clipper shape it too, and it plays whole whatever the decay. Glided legato notes
  don't play it. On T2 a green push picks the slot (AUTO, the first other slot with a recording, or 1 .. 8); the line
  under DECAY shows it. Saved with the sound (record 8 .. 10); older sounds load with it off.

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
