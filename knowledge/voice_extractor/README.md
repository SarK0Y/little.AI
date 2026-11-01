# voice_extractor

A deterministic audio experiment that tries to make music less dominant by
**destroying persistent tones, low-frequency energy, and isolated broadband
attacks**. The aim is to leave a voice candidate for listening—not to claim
that the remaining signal is voice-only.

There are no neural networks, trained models, training data, or reference-voice
databases. The processing uses explicit, hand-written signal-processing rules.
It is experimental and is not a reliable speech/music separator.

## Current default experiment: destructive filtering

The default processing path is:

1. Read a PCM16 mono 16 kHz RIFF/WAVE recording.
2. Analyze overlapping 1024-sample Hann-window frames (64 ms) with a 256-sample
   hop (16 ms), using the in-source radix-2 FFT.
3. Detect narrow local spectral peaks that stand clearly above their nearby
   spectral surroundings and the frame's average magnitude.
4. Track each peak through a temporal neighbourhood. A frequency bin is treated
   as persistent when a peak is found in that bin or an immediate neighbour in
   more than 62% of the neighbourhood frames.
5. Attenuate persistent peaks by up to 88%. This targets stable musical notes
   and partials; it can also attenuate sustained vowels.
6. Apply a low-frequency cut: retain only 8% of bins below 90 Hz, then smoothly
   increase the gain to full level at 180 Hz. This targets kick/bass energy.
7. Detect isolated broadband attacks from positive spectral flux, a sharp drop
   in flux in the next frame, and spectral flatness. When detected, attenuate
   the 900–7600 Hz region by up to 72%. This targets clicks and claps.
8. Reconstruct the candidate with Hann overlap-add.

The filter uses temporal persistence rather than simply deleting every narrow
frequency band. Speech vowels also contain narrow harmonics, while some
instruments have moving pitches and broadband attacks; the distinction is not
perfect.

## Output files

For an output prefix such as `out/test`, the program writes:

- `out/test_voice_candidate.wav` — the original recording after stable-tone,
  low-frequency, and transient suppression. It is a candidate, not isolated
  voice.
- `out/test_removed_tones.wav` — the time-domain difference between the input
  and the candidate. Listen to this file as well: it reveals what the current
  filters removed. It may contain voice, artifacts, or both.

The parent directory for the output prefix must already exist.

## Build

Requires a C++17 compiler and `make`.

```sh
make clean && make
```

The Makefile uses `-O2 -std=c++17 -Wall -Wextra -Wpedantic` by default.

## Input format

Input must be **16-bit PCM, mono, 16,000 Hz WAV**. Convert another audio format
with ffmpeg:

```sh
ffmpeg -i input.ext -ar 16000 -ac 1 -c:a pcm_s16le input_16k.wav
```

## Run

```sh
# Default destructive-filtering experiment
./voice_extractor input_16k.wav

# Choose an output prefix (create its parent directory first)
mkdir -p out
./voice_extractor input_16k.wav out/test

# Compare with the older modulation-group + consonant/vowel experiment
./voice_extractor input_16k.wav out/legacy --legacy

# Older experiment without its consonant/vowel gate
./voice_extractor input_16k.wav out/legacy_raw --legacy --raw
```

The default output prefix is `voice`. `--legacy` selects the previous
modulation-group/C-V implementation; `--raw` disables that implementation's
C/V gate and is meaningful with the legacy path.

## Current parameters

| Parameter | Value |
|---|---:|
| Sample rate | 16,000 Hz |
| FFT size | 1,024 samples (64 ms) |
| FFT hop | 256 samples (16 ms) |
| FFT-bin spacing | 15.625 Hz |
| Narrow-peak test | Local maximum; over 2× nearby spectral average and 2× frame mean |
| Persistence neighbourhood | ±16 frames (about 256 ms each side; 512 ms total span between endpoints) |
| Required persistence | Greater than 62% |
| Maximum persistent-tone attenuation | 88% |
| Low-frequency gain | 8% below 90 Hz; ramps to 100% at 180 Hz |
| Transient detector | Flux > 0.42, next-frame flux < 58% of current, flatness > 0.12 |
| Maximum transient attenuation | 72% from 900–7600 Hz |
| Reconstruction | Hann overlap-add |

These are deliberately aggressive starting values for listening tests, not
validated optimal settings.

## Limitations and things to listen for

- **Low voices:** the low cut can thin male voices and remove useful vocal body.
- **Vowels:** sustained vowel harmonics can look like persistent musical tones
  and be attenuated.
- **Consonants:** plosives and sharp consonants can resemble clicks or claps and
  be attenuated by the transient detector.
- **Percussion:** sustained cymbals, noisy percussion, and attacks overlapping
  speech may not satisfy the isolated-attack test and can survive.
- **Instruments:** vibrato, tremolo, distortion, and pitch bends can make
  musical tones less stable, allowing them to survive.
- **Dense music:** overlapping partials and spectral leakage make individual
  peaks harder to classify.
- **Singing:** sustained sung notes may be treated like instrumental notes.
- **No linguistic recognition:** the default pass does not identify phonemes,
  syllables, words, or speakers.

## Listening protocol

Compare the original, `*_voice_candidate.wav`, and `*_removed_tones.wav` at
matched playback volume. Try at least:

1. Instrumental music without voice.
2. Isolated spoken voice.
3. Spoken voice over music.
4. Singing over instrumental music.

Ask whether kick/bass, stable instrumental partials, and isolated clicks/claps
move into the removed component while speech remains intelligible in the
candidate. Pay special attention to low male voices and consonants such as
P, T, K, S, and F. Keep the original recording for comparison; waveform
appearance alone does not establish separation quality.

The older dynamic modulation-group and consonant/vowel experiment remains in
the same source file and can be selected with `--legacy`.
