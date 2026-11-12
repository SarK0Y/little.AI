# sound_detector — notes

Two time-domain ideas, no FFT, no scipy: **fading** and **bands**.

- `bands.py` — ~230 lines: biquad bandpass bank (10 bands, 100 Hz–6.3 kHz),
  rectified envelope followers, per-band release-edge measurement, 5 rules.
  Self-check on synthetic signals + CLI for wav files.

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
