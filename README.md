# Drifter (schwung-drifter)

**English** | [日本語](README.ja.md)

**An instrument made only for chaotic drones with a strong low end**, running on
Schwung on Ableton Move (a Schwung sound generator module). It starts sounding
when you press Move's Play button and goes silent when you stop. It is not a
synth you play from a keyboard.

> A module inspired by the REON driftbox SIII.
> It has no connection with REON and is not endorsed by REON.

It is written from scratch using only the feature list REON has published and
general DSP techniques (a clean-room implementation). None of the firmware, code
or circuits of REON's products are used. It does not aim to be a faithful clone of
the hardware either.

---

## Overview

At the heart of the sound are **cross FM** and **hard sync** between two VCOs.

- The two VCOs shake each other's frequency at audio rate (cross FM) while VCO1
  forcibly resets VCO2's cycle (hard sync). Using both at once is the default
  (CHARACTER = FM+SYNC)
- Underneath, two kinds of low end are always laid down: **a stable sub bass
  untouched by FM (CLEAN SUB)** and **a wavering, distorted low end (DIRTY SUB)**
- A 24 dB/oct nonlinear ladder filter is modulated at audio rate by the VCOs
  (DRIFT Y), creating metallic, rough harmonics
- With the single **MOTION** knob, the FM, sync and filter keep moving by
  themselves, slowly (irregularly at maximum)
- The **RANDOM** pad makes, in one press, a drone limited to ranges that actually
  work in a track

The aim is a sound where "dark (DARK DRONE)", "industrial (INDUSTRIAL)" and
"chaotic (CHAOTIC)" live together. However wild the harmonics get, the low end
never thins out.

Deliberately **not included**: envelopes such as ADSR, glide, tempo-synced LFOs,
pitch from notes, velocity, pitch bend, polyphony, waveform selection, and fine
on/off switches for modulation.

## Features

- 2 VCOs: VCO1 = square (sync master), VCO2 = sawtooth (sync slave). 27.5 Hz (A0)
  to 440 Hz (A4)
- Cross FM (both directions, audio rate) and hard sync (one way, VCO1 → VCO2).
  CHARACTER chooses FM / SYNC / FM+SYNC
- A 24 dB/oct nonlinear ladder low-pass (zero-delay feedback), self-oscillating.
  Audio-rate modulation by the VCOs
- BASS ENGINE: CLEAN SUB (sine) + DIRTY SUB (phase modulation + saturation),
  sounding at VCO1's pitch folded into 40–80 Hz
- MOTION ENGINE: 5 slow modulations at different speeds (sine + smooth random).
  No BPM sync, no periodic gating
- Delay: one knob (time, feedback and amount linked)
- RANDOM pad: 3 styles (CHAOTIC / DARK / INDUSTRIAL)
- Sounds only while Move is playing (with fades on start and stop)
- Mono output (left and right the same)

---

## Getting started

### 1. Load it into a slot

1. Open Schwung's screen and choose the slot of the track you want to use.
2. Choose **Drifter** from the Sound Generator list.
3. **Press Move's Play button.** It fades in over about 20 ms and the drone starts.
4. On stop it fades out over about 30 ms to silence (the delay's tail goes with it).

The clip can be empty. You do not need to put notes in it (notes do not change
the sound).

Right after adding the module (right after installing or updating), **always
restart Move** before using it. Without a restart, old and new versions of the
module get mixed up, and problems such as knobs not working occur.

### 2. Knobs to try first (MAIN page)

Page 1, **MAIN**, holds only the knobs you touch while playing.

1. **Knob 4 (FM DRV)**: turn this first. Gentle harmonics on the left, metallic
   around the middle, more chaotic the further right.
2. **Knob 5 (SWEEP)**: moves the hard-sync harmonics — the classic "nyeeow"
   harmonic sweep. It moves separately from FM DRV.
3. **Knobs 6 and 7 (CUTOFF / RESO)**: the filter. Turning RESO close to fully
   right adds self-oscillation. The low end (SUB) stays even with CUTOFF closed.
4. **Knobs 2 and 3 (DRFT X / DRFT Y)**: the joystick. The middle is 0, and both can
   turn either way. X is the frequency ratio of the two VCOs, Y the filter's
   audio-rate modulation. Both **move further towards the treble the further right
   you turn** (the low end comes separately from the SUB, so it stays when you
   turn them up).
5. **Knob 8 (MOTION)**: at 0 the sound stays still. The higher it goes, the more
   the timbre moves slowly by itself, and fully right it runs wild irregularly.
6. **Knob 1 (VOLUME)**: overall level.

### 3. Set the foundation (SETUP page)

Move to page 2, **SETUP**, with the jog wheel. These are settings you rarely touch
once set.

1. **Knobs 1 and 2 (V1 FRQ / V2 FRQ)**: the drone's base pitches. Defaults are
   VCO1 = 55 Hz (A1) and VCO2 = a fifth above it (detuned slightly to beat).
2. **Knob 3 (MIX)**: the level balance of VCO1 and VCO2.
3. **Knob 4 (BASS)**: thickness of the low end.
4. **Knob 5 (DIRTY)**: how dirty the low end is (wobble and distortion).
5. **Knob 6 (DELAY)**: the delay. None at 0; longer, darker and deeper as it goes up.
6. **Knob 7 (CHAR)**: FM / SYNC / FM+SYNC (default).
7. **Knob 8 (STYLE)**: the direction of the sounds the RANDOM pad makes.

### 4. Making sounds with the RANDOM pad

While the Drifter screen is open, **the top-right pad becomes RANDOM** (lit). Each
press gives a new drone following SETUP's STYLE. The top of the screen briefly
shows something like "RANDOM CHAOTIC".

- **What changes**: FM DRV, SWEEP, CUTOFF, RESO, MOTION, V1 FRQ, V2 FRQ, MIX, BASS,
  DIRTY, DELAY.
- **What returns to 0 (centre)**: DRFT X / DRFT Y, so from the new sound you can
  move the joystick either way.
- **What does not change**: VOLUME, CHAR, STYLE.
- Pressing it with Move's Record on records the change to the random values as
  automation.

**How the STYLEs differ** (all are built on "a fat low end + complex harmonics";
they are not separate genres)

| STYLE | Tendency |
|---|---|
| **CHAOTIC** (default) | The standard style with all three elements. Largest MOTION, moves the most |
| **DARK** | Lower and darker (lower CUTOFF), restrained FM and sync, more low end and delay |
| **INDUST** | Strong FM and sync, more DIRTY, less delay. VCO2 sometimes lands on an inharmonic ratio |

RANDOM narrows its ranges and combinations to "make a drone you can use in a track
as it is", not to "make extreme settings" (see "Inside RANDOM" below).

**While the screen is open, the other 31 pads do nothing** (they are not lit).
Because of how Schwung works, a module cannot borrow just one pad; it has to borrow
all 32 together. When the screen is closed the pads return to Move.

---

## Controls (detail)

### Parameter pages

It is operated on the same screen as Schwung's standard knob screen (`ui_chain.js`
draws it with Schwung's own page engine; the only difference is that it uses the
pads). The jog wheel switches between the 2 pages.

| Page | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| **MAIN** | VOLUME | DRIFT X | DRIFT Y | FM DRIVE | SYNC SWEEP | CUTOFF | RESONANCE | MOTION |
| **SETUP** | VCO1 FREQ | VCO2 FREQ | VCO MIX | BASS | DIRTY SUB | DELAY | CHARACTER | RANDOM STYLE |

### Each parameter

| Key | Range | What it does |
|---|---|---|
| `volume` | 0–1 | Output level (x^1.5 curve, follows smoothly over about 15 ms, complete silence at 0). Default 0.7 |
| `drift_x` | −1 to +1 | Moves VCO2's frequency relative to VCO1. **−1 octave fully left, +3 octaves fully right** (fine near the centre, larger towards the ends). Changes the harmonic make-up and the beating. VCO1's pitch and the sub bass do not move |
| `drift_y` | −1 to +1 | Audio-rate modulation of the filter. The + side uses VCO2 (sawtooth), a tearing, bright wobble. The − side uses VCO1 × VCO2 (the product of ring modulation), bell-like inharmonics. Up to ±5 octaves. **On the + side the cutoff itself also opens by up to +4 octaves** (the − side is modulation only). No effect on the sub bass |
| `fm_drive` | 0–1 | Depth of the cross FM. At 1, VCO2 → VCO1 is ±6 octaves (VCO1 → VCO2 is 0.8 times that). With an x^1.3 curve, the sound changes from early on. Turning it up also opens the cutoff by up to +1 octave. The instantaneous pitch of a VCO swung by FM is kept between 1.5 octaves below and 5 octaves above its own pitch (see "FM range" below). At maximum the VCOs' average pitch rises a little under an octave (the SUB keeps the low end) |
| `sync_sweep` | 0–1 | SYNC / FM+SYNC: raises the slave (VCO2) by 0–5 octaves = the sync harmonics. FM: raises VCO2 by 0–3 octaves = the FM ratio. Turning it up also opens the cutoff by up to +1 octave |
| `vcf_cut` | 0–1 | Cutoff 40 Hz to 16 kHz (logarithmic). Default 0.55 (about 1.1 kHz). The actual cutoff adds FM DRIVE (up to +1 oct), SYNC SWEEP (up to +1 oct) and the + side of DRIFT Y (up to +4 oct) |
| `vcf_reso` | 0–1 | Resonance. Self-oscillates above about 0.95. The output does not run wild when it goes up |
| `motion` | 0–1 | Strength and speed of the autonomous modulation (see "Inside MOTION" below) |
| `vco1_freq` | 0–1 | VCO1's frequency. 27.5 Hz to 440 Hz (4 octaves, one octave per 0.25). Default 0.25 = 55 Hz |
| `vco2_freq` | 0–1 | VCO2's frequency (same range). The default is 6 cents above a fifth over VCO1 |
| `vco_mix` | 0–1 | 0 = VCO1 only, 1 = VCO2 only (a constant-power balance) |
| `bass` | 0–1 | Thickness of the low end (controls CLEAN SUB and DIRTY SUB together) |
| `dirty_sub` | 0–1 | Strength of DIRTY SUB's wobble and distortion |
| `delay` | 0–1 | One delay. 0 = none, 1 = 0.9 s, feedback 0.75, dark repeats |
| `character` | FM / SYNC / FM+SYNC | Default FM+SYNC (see "CHARACTER" below) |
| `random_style` | Chaotic / Dark / Indust | Style of the RANDOM pad. Does not change the sound itself |

Parameters with options can be written either by number or by option name.

### CHARACTER (FM / SYNC / FM+SYNC)

| Mode | FM VCO2 → VCO1 | FM VCO1 → VCO2 | Hard sync |
|---|---|---|---|
| **FM** | FM DRIVE | FM DRIVE × 0.8 | none |
| **SYNC** | none (VCO1 stays clean) | FM DRIVE | VCO1 → VCO2 |
| **FM+SYNC** (default) | FM DRIVE | FM DRIVE × 0.8 | VCO1 → VCO2 |

- In every mode, sync goes **one way only, with VCO1 as master and VCO2 as slave**.
  If the two VCOs reset each other, their cycles chase each other into an
  unmanageable buzz, so there is no two-way sync. Influence in the other direction
  is carried by FM alone.
- In FM+SYNC, VCO2 is reset on the cycle of a master shaken by FM, so both the
  murk of FM and the sharp harmonics of sync remain.
- In every mode all 8 knobs do something (in SYNC mode FM DRIVE becomes "FM on the
  synced slave").
- Switching modes lowers the level for a few ms and swaps during that time, so it
  does not click.

### BASS ENGINE (the low end)

Two kinds of low end are added **after** the filter. Closing the cutoff or raising
the resonance (a ladder filter loses low end as resonance goes up) never thins the
low end.

- **CLEAN SUB**: a sine wave. Its pitch is **VCO1 FREQ's pitch moved by whole
  octaves into the 40–80 Hz range** (55 Hz by default). It is completely
  unaffected by FM, sync, DRIFT X/Y and MOTION, so however wild the harmonics above
  get, the same low end always sounds. Turning V1 FRQ across an octave boundary
  moves it smoothly by an octave over about 40 ms.
- **DIRTY SUB**: a sine wave with **the same phase** as CLEAN SUB, phase-modulated
  by VCO1's triangle (one way; nothing goes back from DIRTY SUB to the VCOs), its
  modulation depth wobbling slowly, saturated, and passed through its own low-pass
  (350–1250 Hz). Being made from the same phase, the two low ends add rather than
  cancel. The modulation depth is limited to a range where the fundamental does not
  thin out (at most about 1.25 radians).
- **BASS**: turning it up brings in CLEAN SUB first, then in the second half DIRTY
  SUB joins and replaces part of CLEAN SUB. It moves so that "the low end gets
  fatter and denser", not so that "the same sound gets louder".
- The output stage has a 25 Hz high-pass (2nd order), removing unneeded infrasound
  and DC. So it does not bury a typical kick too much, the amount of sub is kept
  modest and the output leaves headroom.

### Inside MOTION

- 5 modulations: FM depth, amount of sync, filter modulation (added to DRIFT Y),
  cutoff (±1.2 octaves), VCO2's ratio (±0.12 octave).
- Each is the sum of "sine waves at speeds that do not divide into each other" and
  "a random movement that glides smoothly to a new target every 1.5–4.5 s", so **it
  never repeats the same movement**. Above MOTION 0.6, faster, smaller random
  movement is added.
- MOTION's value changes the depth (completely still at 0) and the speed (0.35 to
  3.35 times) together.
- It never touches the low end (CLEAN SUB / DIRTY SUB pitch), VCO1's pitch or the
  overall level. There is no periodic movement that suggests a rhythm, and no tempo sync.

### Delay

The single DELAY knob moves time (60 ms to 0.9 s), feedback (0.12–0.75) and wet
amount together. No delay at 0. The higher it goes, the longer, the more repeats and
the darker (the setting that loses more highs the longer it gets follows the
specification's request for "a dark spatial spread at large values"). Feedback stops
at 0.75 and the loop saturates softly, so it never oscillates and runs away. Turning
DELAY makes the time follow smoothly, so the pitch of the repeats wavers a little.

### Playing and stopping

- It watches Move's transport (the host's beat position `get_beat_position`,
  negative while stopped). On a host without it, it judges from MIDI clock
  (Start / Continue / Clock to play; Stop, or 0.5 s with no clock, to stop). **When
  neither is known, it does not sound.**
- About a 20 ms fade-in when playback starts and about a 30 ms fade-out on stop
  (including the delay's tail).
- Once stopped and the fade-out has finished, the DSP computes nothing. The next
  time it plays, phases, filter and delay start empty (the previous tail does not
  sound).
- MIDI notes, velocity and pitch bend are ignored. Neither pitch nor timbre changes.

### Automation (recording movements with Move's record)

With Schwung's own automation feature, knob movements can be recorded into and
played back from Move's clips (Schwung 1.6 or later). No special setting is needed
on the module side.

**Recording**
1. Select Drifter's track on Move and play the clip.
2. Press Move's **Record** to arm recording.
3. Turn knobs on Schwung's Drifter screen. The movements are recorded into that clip.
4. Stop Record. From then on, the recorded movements play back every time the clip loops.

**Clearing**

| Scope | How |
|---|---|
| One knob only (the current clip) | On the knob screen, **hold Delete and touch that knob (turn it a little)** |
| One step only | Hold the step, press Delete, and touch a knob (releasing without touching a knob clears every parameter on that step) |
| The whole track | **Slot Settings → Clear Lanes** of that slot |

**When it does not work**
- **The automation is not heard while playing / sticks at the value you turned**:
  check on Schwung's **Scenes** screen whether that parameter is locked in a
  snapshot. A parameter locked in a scene sounds at the scene's value, which comes
  after the automation.
- **Turning knobs does not change the sound**: Move may not have been restarted after
  updating the module. Restart it.
- The knob's pointer on screen keeps pointing at **the knob's own value**, and the
  automation's value is shown by a separate small mark. The pointer not moving is normal.

---

## Signal flow

```
          MOTION (5 slow modulations)
             │ FM / SYNC / VCF modulation / CUTOFF / VCO2 ratio
             ↓
   VCO1 (square) ⇄ cross FM ⇄ VCO2 (sawtooth)
        └──── hard sync (1 → 2) ────┘
                     │
                    MIX
                     ↓
       ladder VCF ← DRIFT Y (audio-rate modulation by the VCOs)
                     │
                     + CLEAN SUB (sine, no FM)
                     + DIRTY SUB (same phase, phase-modulated by VCO1 → saturation → LPF)
                     ↓
                   DELAY (1 knob)
                     ↓
          play / stop fades → 25 Hz HPF → limiter → VOLUME → output (mono)
```

The VCOs, mixer, VCF and both subs are computed at twice the host rate (88.2 kHz)
and decimated once with a 31-tap half-band filter. The delay and output stage run at
44.1 kHz.

**Order of FM and SYNC processing** (per sample, 2× oversampled):
1. Advance VCO1, FM'd by VCO2's triangle from **one sample earlier** (if it passes
   the end of its cycle, record the moment it did).
2. Advance VCO2, FM'd by VCO1's **current** triangle. If VCO1 finished its cycle
   within this sample (and sync is on), reset VCO2's phase to 0 at that exact moment.
3. The FM modulating signal is a **triangle** made from each VCO's phase (FM by a
   square only flips between two pitches; with a triangle the first half of FM
   DRIVE is gentler).

**Anti-aliasing**: PolyBLEP is applied to the steps of VCO1's square and of VCO2's
sawtooth (both the natural wrap and the sync reset). The size of a sync step is known
only after the reset happens, so VCO2 is output one sample (11 µs) late and the
correction for the near side of the step is added to the sample not yet output. So
that FM does not push a VCO's instantaneous frequency too high, it is capped at
17.6 kHz.

**FM range**: however deep FM DRIVE goes, the instantaneous pitch of a VCO swung by
FM is kept between **1.5 octaves below and 5 octaves above** that VCO's own pitch.
Without the limit, deep cross FM + sync falls into one of two dead ends: the master
slowing to a few Hz (below the audible range, turning the drone into crackling clicks),
or the master pinned at the ceiling with the slave reset every sample (turning into
DC). Both were measured to make the sound about 15 dB quieter. Also, the synced slave's
(VCO2's) triangle starts from −1 at every reset, so it is biased negative. Used for FM
it would drag the master down, so the bias (DC) is removed before it is used for FM.

**Output safety**: a 25 Hz high-pass, a limiter acting instantly and recovering over
250 ms (threshold 0.95), and a soft clip bringing anything above 0.8 smoothly into
0.98. In normal patches the limiter hardly acts (the default patch at VOLUME 0.7 peaks
around −8 dBFS with an RMS around −17 dBFS). Should NaN / Inf ever appear, it returns
to silence within one block and recovers.

## Inside RANDOM

Rather than simply setting every knob to a uniform random value, it builds patches
under these constraints (`STYLE_RANGES` and `randomPatch()` in `ui_core.mjs`).

- Each style sets the range of each knob (DARK has lower CUTOFF and more delay, INDUST
  more FM, sync and DIRTY, and so on).
- **VCO1 FREQ is limited to 31–91 Hz** (a low drone). **VCO2 is set as an interval
  from VCO1** (unison, fourth, fifth, octave, octave + fifth, two octaves, each detuned
  by a few cents). INDUST uses an inharmonic ratio 40 % of the time.
- **BASS is 0.4 or more** (there is always a low end).
- The sum of FM DRIVE and SYNC SWEEP is at most 1.3 (so the sound does not collapse with
  both at maximum). When MOTION is above 0.7, FM DRIVE is lowered by 15 % (because
  MOTION itself moves FM by ±0.3).
- RESO is at most "0.85 − 0.3 × FM DRIVE" (the stronger the FM, the less resonance).
  With CUTOFF below 0.4, RESO is at most 0.55 (self-oscillation down low clashes with
  the sub).
- CUTOFF never closes below VCO1's range.
- VOLUME, CHARACTER and STYLE are not changed. DRIFT X / Y return to 0.

**Verification** (`tests/test_random.c`): `tests/test_ui.mjs` writes out 400 patches
per style, and 150 of them per style (450 in all) are actually rendered for 3 s each
(with MOTION moving), checking the following.

| Item | Pass condition | Measured (x86, v0.8.0–v0.8.1; patches change every run, so 3 runs) |
|---|---|---|
| NaN / Inf | none | none |
| Peak | below 0.98 | at most 0.54 |
| RMS | −28 to −9 dBFS | −20.7 to −9.4 dBFS |
| Level spread within a style (10–90 %) | within 10 dB | about 3.5–4.2 dB |
| Low-band (below 150 Hz) energy | at least 25 % of the total | minimum 33–41 % |
| High-band (above 4 kHz) energy | at most 12 % of the total | maximum 4.4–9.1 % |
| DC | below 0.005 | at most 0.0015 |
| Dropouts (RMS every 100 ms) | never below −40 dBFS | quietest 100 ms at −23 to −27 dBFS |

## Building

Requirements: `gcc`, `python3`, `aarch64-linux-gnu-gcc` for Move, and Node.js for the
UI tests. Schwung's plugin API header (`third_party/schwung/host/plugin_api_v1.h`,
unchanged from Schwung 1.7.3) is bundled, so this repository builds on its own.

```bash
scripts/build.sh arm64      # dist/drifter/dsp.so and dist/drifter-module.tar.gz
scripts/build.sh native     # desktop dsp.so for the tests
scripts/test.sh             # all tests + ARM64 build (--quick skips the 60 s stress test)
```

`scripts/check_abi.py` fails the build if the ARM `.so` is not AArch64, uses
functions newer than Move's glibc 2.34, or exports anything other than
`move_plugin_init_v2`.

Tests (`scripts/test.sh`):
- `tests/test_engine.c` (with ASan + UBSan, 202 checks): defaults (FM+SYNC is the
  default mode) and ranges; the mapping of each knob (VCOs; the sub always within
  40–80 Hz and a whole number of octaves from VCO1; cutoff; delay); complete silence
  and no computation while stopped; fade-in on play and silence within 40 ms of stop
  (including the delay); VCO pitch; DRIFT X moving VCO2 by −1 to +3 octaves without
  moving the sub; FM DRIVE increasing the swing of VCO1's instantaneous pitch step by
  step, changing the timbre from early on, keeping level differences within 6 dB, the
  pitch centre within an octave and the instantaneous pitch within −1.5 to +5 octaves;
  SYNC repeating the waveform on the master's cycle and SWEEP adding harmonics; FM
  DRIVE and SWEEP acting independently of each other in FM+SYNC; sync steps being
  band-limited; no click on CHARACTER switches; DRIFT Y acting on both signs and
  sounding different for each; **CLEAN SUB alone** (the difference with and without
  BASS) staying a pure sine within 5 % amplitude even at FM 1 + MOTION 1; DIRTY SUB
  adding harmonics while keeping the fundamental, without highs; BASS increasing the
  low end step by step; MOTION 0 stopping FM and cutoff completely, MOTION 1 moving
  them a lot, smoothly and within range, without moving the sub; **turning every knob
  one at a time from minimum to maximum** with level jumps per 50 ms under 9 dB and
  peaks at or below 0.99; 32 extreme combinations at 22.05 / 44.1 / 48 / 96 kHz; **a
  60 s render with every knob at maximum**; 20,000 blocks of random operation
  (including play/stop switching).
- `tests/smoke.py`: `dlopen`s the real `dsp.so` through the same v2 API as Schwung and
  checks that the metadata matches `module.json`; the order of the 2 pages; reading and
  writing every parameter (numbers and option names); that writing old versions' keys
  does not crash; saving and restoring state, loading old formats (v0.5.0–v0.7.0) and
  rejecting broken data; silence while stopped, no sound from MIDI notes, fade-in on
  play and fade-out on stop; **that the output is exactly the same, sample for sample,
  when MIDI notes and pitch bend are sent**; complete silence at VOLUME 0; every knob at
  maximum and minimum; play detection from MIDI clock and stopping when the clock
  breaks off; the instance limit; a 48 kHz host.
- `tests/test_ui.mjs` (Node.js): runs `ui_core.mjs` with Schwung's real page engine
  (pointing `DRIFTER_SHARED` at a copy of the Move's `/data/UserData/schwung/shared`
  gives the same version as the device) and checks the order of the 2 pages; borrowing
  the pads while the screen is shown and lighting only the top-right one; knob
  operation; the other pads doing nothing; RANDOM rewriting every target knob without
  touching the excluded ones and returning DRIFT X/Y to 0; the chosen STYLE being used;
  the range and combination constraints over 9000 patches; per-style tendencies; and
  returning the pads and LEDs to Move when the screen closes.
- `tests/test_random.c`: the verification in "Inside RANDOM" above.
- `--bench`: computation time for 60 s with every knob at maximum.

**CPU**: with every knob at maximum, about 1 % of one core on an x86 desktop, and
**about 4.6 % on a real Move (Cortex-A53, one core; about 134 µs per 128-sample
block)**. Nearly zero while stopped.

## Installing

```bash
scripts/build.sh arm64
python3 scripts/install.py http://<Move's IP address>:7700
```

This sends `dist/drifter-module.tar.gz` to Schwung Manager's custom module install. To
install a tarball downloaded from Releases, add `--package <file>`. Installing the same
tarball from the Manager's web page also works. It goes into
`modules/sound_generators/drifter/`.

**Restart Move after installing or updating.**

## Changes in v0.8.1

- DRIFT X: the right side widened from +1 to **+3 octaves** (the left side stays at
  −1 octave).
- DRIFT Y: on the + side the cutoff itself now also opens by up to +4 octaves (the −
  side is unchanged).
- FM DRIVE: maximum from ±3.5 to **±6 octaves**. SYNC SWEEP: maximum from 3.5 to **5
  octaves** (2 → 3 in FM mode). Turning either up opens the cutoff by up to +1 octave
  each, so the harmonics they create are heard rather than shaved off by the filter.
- To fix the sound disappearing with deep FM, the FM swing is limited to −1.5 to +5
  octaves of each VCO's pitch, and the bias of the synced VCO2's triangle is removed
  (see "FM range"). v0.8.0's "correction that keeps the pitch centre under FM" was
  dropped, because with deep FM it pushed the VCOs below the audible range.
- The saved-data format is the same as v0.8.0 (v3). The same knob positions sound like
  bigger changes than in v0.8.0.

## Changes from v0.7.0, and old sets

v0.8.0 rebuilt it from a keyboard-played mono synth into **an instrument only for
drones**. Parameters went from 30 to 16, and 4 pages became 2.

- **Removed**: per-VCO waveform, FM/SYNC switches, MOD and level; the VCF modulation
  source switch (VCO1 / VCO2 / LFO) and VCF MOD; the LFO (RATE / WAVE / destinations);
  DRIFT destination switching; Delay Time / Feedback (merged into the single DELAY);
  GATE HOLD; TRIGGER; SUB (replaced by BASS and DIRTY); sounding and pitch from notes.
- **Kept under the same key names**: `volume`, `drift_x`, `drift_y`, `vcf_cut`,
  `vcf_reso`, `vco1_freq`, `vco2_freq`. But `vco1_freq` / `vco2_freq` changed range
  (0.3 Hz–14 kHz → 27.5–440 Hz), and `drift_x` / `drift_y` now have fixed destinations.
- **Saved data**: the state format went up to `{"v":3,"p":[16 values]}`. Loading an old
  format (`v:1` from v0.5.0, `v:2` from v0.6.0–v0.7.0) **does not fail**: it carries
  over only VOLUME, CUTOFF and RESONANCE, and everything else becomes v0.8.0's defaults
  (DRIFT X / Y at the centre). The old versions' sound is not reproduced.
- If old versions' keys (`vco1_mod`, `gate_hold`, `trigger` and so on) are written
  from sets, automation or scenes, they are simply ignored. Automation recorded on old
  keys stops having an effect, so clear it if needed (see "Clearing" under
  "Automation").

## Compatibility with Schwung

- Plugin API **v2** (`move_plugin_init_v2`). Multi-instance (a fixed pool of 6, no
  memory allocation on the audio thread). The host runs at 44.1 kHz (22.05–96 kHz also
  work).
- Every entry point is called on the audio (SPI) callback, so the DSP does no memory
  allocation, file I/O, locking, threading or logging at all.
- Saved state: `{"v":3,"p":[16 values]}`. It is loaded only when every value is valid
  and in range.
- `ui_hierarchy` is returned empty (so Schwung opens `ui_chain.js`), and the page
  layout is returned in `grid_hierarchy`.
- Confirmed working on Schwung 1.7.3.

## Signal Chain

`component_type: "sound_generator"`, `chainable: true`. All 16 parameters are
published in `chain_params`, so they can be operated from the chain, the knob screen,
LFO destinations and automation alike.

Schwung puts a slot that has been silent for a while to sleep to save CPU (while
asleep it computes only about every 0.5 s). Left like that, sound could start up to
0.5 s after pressing Play, so while stopped it outputs a tiny DC of −75 dBFS
(inaudible) to keep the slot awake. The stopped DSP computes nothing, so it uses almost
no CPU. At VOLUME 0 it does not output the DC either, and is completely silent.

## Differences from the hardware

- Being digital, the VCOs do not drift. There is no analog noise either.
- There are no CV / gate / audio inputs. Pitch is set only by the VCO FREQ knobs, and
  it cannot be played from a keyboard.
- The DRIFT joystick is two knobs. It does not spring back to the centre when let go.
- The waveforms are fixed (VCO1 = square, VCO2 = sawtooth, the FM modulating signal a
  triangle).
- Sync is one way only, VCO1 → VCO2.
- The BASS ENGINE (CLEAN SUB / DIRTY SUB), MOTION, RANDOM and the link with Move's
  playback were added in this module.
- Anti-aliasing goes only as far as 2× oversampling + PolyBLEP, so where FM DRIVE and
  SYNC SWEEP are both deep some aliasing appears (Move's CPU takes priority).
- The VCF is a TPT ladder with one tanh at its input stage. It approximates the
  saturation of a transistor ladder; it is not a model of the circuit itself.

## To check by ear on the hardware

Numeric tests only show that "level, lows, highs, DC, dropouts and NaN" are within
range. Whether it is "a good sound" needs checking by ear on Move:

1. **Default patch**: does Play alone give a heavy, stable low drone? Does it click on
   start and stop?
2. **FM DRV**: does the change from gentle harmonics in the left third to metallic in
   the middle to chaotic at the right end feel smooth across the whole knob? Does it
   change too suddenly anywhere?
3. **SWEEP**: in FM+SYNC, is the sync sweep heard as something separate from FM DRV?
   At high values, is there grating aliasing (high tones unrelated to the pitch)?
4. **DRIFT X / Y**: can they be moved finely near the centre? Is the difference in
   texture between Y's + and − usable?
5. **BASS / DIRTY**: does the low end get fatter on both Move's speaker and headphones
   (or monitors)? Played with a kick, are the lows too muddy, and is there headroom in
   the output (no distortion even at BASS 1 + DIRTY 1 + VOLUME 1)?
6. **RESO**: is the self-oscillation at the right end too harsh?
7. **MOTION**: completely still at 0, slow in the middle, irregular at the right end?
   Even at maximum, does it never suddenly go silent, suddenly get loud, or produce
   grating highs?
8. **DELAY**: do large values give a dark, deep space? Does the amount of darkness
   suit you (it can be changed if you would rather it were not dark)?
9. **CHAR**: can the difference between the three modes be heard? Does switching click?
10. **RANDOM**: press it about 10 times per STYLE — is every result a drone usable as
    it is? Are the levels consistent?
11. **CPU**: on Schwung Manager's CPU page, is Drifter's load in a practical range (the
    benchmark gives about 4.6 % of one core with every knob at maximum)?

## Licence

MIT License, Copyright (c) 2026 Hiro Ikezawa (`LICENSE`). The licence of the bundled
Schwung header is in `THIRD_PARTY_LICENSES.md`. The names "driftbox" and "REON" are used
only to explain what inspired this module.
