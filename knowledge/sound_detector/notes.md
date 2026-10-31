# sound_detector — notes

Two time-domain ideas, no FFT, no scipy: **fading** and **bands**.

- `bands.py` — ~230 lines: biquad bandpass bank (10 bands, 100 Hz–6.3 kHz),
  rectified envelope followers, per-band release-edge measurement, 5 rules.
  Self-check on synthetic signals + CLI for wav files.
- `mod.py` — modulation clocks per band (syllable 3–8 Hz, beat 0.3–2.5 Hz)
  by autocorrelation of a detrended decimated envelope. No FFT.
- `sound_detector.cpp` — the two above ported to C-style C++17 with
  `std::variant` errors: `--selftest` + wav CLI, zero dependencies.

## The core idea

A voice is **n frequencies that fade in unison** — a handful of harmonics
share one 30–80 ms release. A song is many frequencies fading at *staggered*
times. Hiss fades all at once; a drone never fades. The biquad bank gives us
a per-band envelope, and `_fades` finds every 20 dB descent in each band.
The discriminator is `short_fades / fades` (per-band descents < 150 ms over
all): voice high, music ≈ 0.

## What is measured

| feature | how |
|---|---|
| `rms_db` | silence gate at -50 dBFS |
| `fades_per_band` | every 20 dB descent per band, in ms |
| `short_fades` | descents < 150 ms, summed over all bands |
| `fades` | descents on the summed envelope (total activity) |
| `spread_hz` | energy-weighted width of the band profile |
| `band_std` | flatness of the band profile in dB (hiss = flat) |

## Rules

```
silence      rms < -50 dBFS
no fades     -> noise if band_std < 4.5, else music   (hiss vs drone/pad)
short_fades > 33% of fades -> voice
>= 2 fades   -> music
1 fade       -> voice if spread > 700 Hz, else music   (short clips)
```

## Measured (16 kHz mono, 1–2 s windows)

| input | truth | got | short/fades | spread | flat |
|---|---|---|---|---|---|
| synth voice (4 Hz syllables) | voice | voice | 76/11 | 541 Hz | 6.9 dB |
| synth chord, 120 ms decay | music | music | 0/3 | 340 Hz | 9.4 dB |
| synth pad (sustained) | music | music | 0/0 | 282 Hz | 10.9 dB |
| synth white noise | noise | noise | 0/0 | 1949 Hz | 4.1 dB |
| `alsa/Front_Center.wav` (real speech) | voice | voice | 37/3 | 554 Hz | 6.7 dB |
| `speech-dispatcher/test.wav` (TTS, 0.5 s) | voice | voice | 7/1 | 990 Hz | 5.7 dB |
| KDE chime | music | music | 0/5 | 647 Hz | 7.4 dB |
| `alsa/Noise.wav` | noise | noise | 0/0 | 1171 Hz | 3.5 dB |

## Confirmed

- Voice is n harmonics fading in unison; short fade ratio is high.
- Music is many freqs fading at staggered times; short fade ratio ≈ 0.
- Noise has no release edges at all, and a flat band profile (< 4.5 dB std).
- 10 biquads per band, ~0.6 s of numpy per 1 s of audio. C++ will be
  realtime with no dependency.

## Open / known weak spots

- Short clip (<1 s) has ≤2 total fades, so the ratio test is skipped and
  `spread` decides; the threshold needs device data, not 8 files.
- Dense music mixes have both fades and a flat spectrum; `band_std` is only
  trusted in the no-fade branch for this reason.
- TTS sample released in 200 ms (above the 150 ms cutoff); the ratio
  test still passed it.
- `n_bands_fading` is *not* the discriminator (voice and music both have all
  10 bands active). The right discriminator is `short_fades / fades`.
- Next step: 20–30 one-second clips per class from the actual device, tune
  3 thresholds (band_std 4.5, fade cutoff 150, spread 700) and nothing else.

## Dense-mix collapse (the headline failure)

Scan of `02. Lost Yo Mind.mp3` (227 s, 45×5 s windows) through `bands.classify`:
**voice=45, music=0**. Every section, including instrumental, reads voice.

Cause: in a dense mix the next note covers the previous tail, so the summed
envelope never drops 20 dB → `fades` collapses to 0–1 → both fallback branches
(spread, short-fade ratio) land on voice. No threshold fixes this; the
"clear release edge" premise does not hold for dense pop.

What did work on that track: the **beat clock** at 0.4–0.8 Hz, c ≈ 0.3–0.5,
in 3+ bands simultaneously, in the 60–125 s windows — a real pulse where the
fade features saw nothing.

## Modulation detector, C++ (`sound_detector.cpp`)

Ports `mod.py` exactly: 10 biquads → 8 ms envelope → decimate ÷8 (2 kHz) →
200 ms sliding-max detrend → autocorrelation over a lag range. No FFT.
C-style, `std::variant` for errors, `--selftest` (3 synthetic cases) plus WAV CLI.

```
g++ -std=c++17 -O2 -Wall -Wextra -o sound_detector sound_detector.cpp
./sound_detector --selftest          # all passed
./sound_detector file.wav            # 227 s file: 41 s, 45 windows
```

Fixes made while bringing it up (all were real bugs, not slowness):

- `buf[2048]` on the stack but a 5 s window decimates to 10 000 samples →
  40 KB stack smash (this was the "hang").
- lag range computed inverted (`DEC/min_hz > DEC/max_hz`) → search loop never
  executed → every band returned 0.
- detrend was a running-max-from-left, not the reference's 200 ms sliding max.
- window hard-coded 5 s rejected files < 5 s; now the tail is one window
  (min 1 s — autocorrelation needs it).
- confidence cutoff 0.2 (internal) → 0.3, matching `mod.py`'s callers; without
  it `Noise.wav` got a false voice label from two 19 Hz hits at c = 0.26 / −0.02.
- vote rule: syllable clock wins outright (`syll_bands >= 2`). The old
  `syll > beat` comparison failed on synthetic voice, whose 4 Hz syllable rate
  also produces a 2 Hz beat harmonic.

### C++ results (5 s windows)

| file | truth | vote |
|---|---|---|
| Lost Yo Mind.mp3, 45 windows | music | **music=44, ?=1** |
| Front_Center.wav (1.4 s speech) | voice | music=1 (beat 1.2 Hz, 2 bands) |
| Noise.wav | noise | ? (no clock ≥ 0.3) |
| KDE chime | music | ? (no clock) |
| synth voice / music / noise | — | voice / music / ? |

The same files through `mod.py` produce identical clocks (Front_Center beat
1.2–1.3 Hz in 8 bands at c 0.33–0.43; fc_loop beat 0.7 Hz, c 0.80) — the port
matches the reference, including its failures.

## Modulation: what held and what did not

Held:

- Dense pop mix: beat clock in 6–10 bands at 0.8–2.4 Hz, confidence ≥ 0.3,
  in **every one** of 45 windows → 44/45 music. This is the fix for the
  dense-mix collapse above; fades gave 0/45.
- Synthetic 4 Hz AM voice: syllable clock 4.0 Hz in all 10 bands (c ≈ 0.77),
  beat harmonic at 2 Hz. Synthetic 1 Hz music: beat only.
- White noise and the chime: no clock survives the 0.3 confidence + 2×median
  spike test.

Did not hold:

- **Real speech has no detectable syllable clock.** `Front_Center` (1.4 s),
  `fc_loop` (7.1 s, the same speech tiled) and `test.wav` all return 0 syllable
  bands. Sweeping detrend 20/50/100/200 ms changes nothing (1 or 0 hits,
  never ≥ 2 bands). The 3–8 Hz clock exists for synthetic AM bursts, where
  the modulation is perfectly periodic, but real syllables are too irregular
  for a single autocorrelation peak to clear `thresh` + the median test.
- So the syllable-vs-beat vote can never say "voice" on real audio today.
  Front_Center reads music (its 1.2 Hz beat is real speech rhythm).

### Lead worth one more probe

Peaks/s of the envelope after an **8 Hz lowpass + 300 ms detrend** (raw peak
counting is useless — carrier ripple gives 180–530/s):

| class | median peaks/s |
|---|---|
| speech (Front_Center, fc_loop) | 10 |
| dense music (lost.mp3) | 21 |
| chime | 23 |
| white noise | 40 |
| test.wav — *voice truth* | 42 ← breaks it |

Speech < music < noise in ordering, which is the right shape (voice holds
steady vowels, a dense mix is transient after transient), but `test.wav`
inverts it. Needs device clips before any threshold is written down.

