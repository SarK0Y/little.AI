# voice_extractor

A deterministic experiment in **destroying persistent musical tones** and
listening to what remains. Instead of trying to identify a speaker or learn a
voice from examples, the default pass looks for narrow spectral peaks that
remain near the same frequency over time, then softly attenuates them.

**No neural networks, training data, statistical model, or reference-voice
database is used.** All decisions are explicit signal-processing rules.
This is an experimental filter, not a reliable speech/music separator.

## Current experiment: stable-tone destruction

The default path is:

1. Read PCM16 mono 16 kHz RIFF/WAVE audio.
2. Analyze overlapping 1024-sample Hann-window frames (64 ms) with a 256-sample
   hop (16 ms), using an in-source radix-2 FFT.
3. In each frame, mark narrow local spectral peaks that stand clearly above
   their nearby spectral surroundings and the frame's average magnitude.
4. Follow each frequency bin through a temporal neighbourhood. A peak is
   considered persistent when a peak remains within that bin or its immediate
   neighbours for more than 62% of a sufficiently long neighbourhood.
5. Softly attenuate persistent peaks (up to 88%) while retaining transient or
   frequency-moving components.
6. Reconstruct the remaining signal with Hann overlap-add.

The pass writes two files:

- `<prefix>_voice_candidate.wav` — original audio after stable narrow peaks
  have been attenuated. This is only a *candidate*; it is not guaranteed to
  contain voice alone.
- `<prefix>_removed_tones.wav` — the component removed by subtracting the
  candidate from the original. Listen to this too: it shows what the filter
  classified as persistent tonal material.

The important heuristic is temporal stability, not simply narrow bandwidth.
A voice also contains narrow harmonics, and an instrument can have broadband
attacks. The filter therefore requires persistence over time rather than
removing every narrow frequency component.

## Build

Requires a C++17 compiler and `make`.

```sh
make clean && make
```

The default Makefile flags are `-O2 -std=c++17 -Wall -Wextra -Wpedantic`.

## Input format

Input must be **16-bit PCM, mono, 16,000 Hz WAV**. Convert other formats with
ffmpeg:

```sh
ffmpeg -i input.ext -ar 16000 -ac 1 -c:a pcm_s16le input_16k.wav
```

## Run

```sh
# New stable-tone destruction experiment
./voice_extractor input_16k.wav

# Choose an output prefix (the parent directory must already exist)
./voice_extractor input_16k.wav ./out/test

# Run the previous modulation-group + C/V experiment instead
./voice_extractor input_16k.wav ./out/legacy --legacy

# Previous experiment without its C/V gate
./voice_extractor input_16k.wav ./out/legacy_raw --legacy --raw
```

Create output directories yourself, for example `mkdir -p out`.

## Initial parameters

| Parameter | Value |
|---|---:|
| FFT size | 1024 samples (64 ms) |
| FFT hop | 256 samples (16 ms) |
| Frequency-bin spacing | 15.625 Hz |
| Peak search | local maximum, 2x nearby spectral average, 2x frame mean |
| Persistence neighbourhood | +/- 16 frames (about 512 ms each side) |
| Required persistence | > 62% |
| Maximum attenuation | 88% |
| Reconstruction | Hann overlap-add |

These are starting values for listening experiments, not validated optimal
settings.

## What can go wrong

- Sustained vowels can be classified as stable tones and partially removed.
- Vibrato, tremolo, distortion, and pitch bends can make instruments appear
  less stable and let them survive.
- Drums and other broadband/percussive instruments are not targeted by this
  first pass.
- Dense chords create overlapping peaks and spectral leakage.
- A stable note shorter than the persistence window may survive; a sustained
  voice note may be attenuated.
- This pass does not explicitly model consonants, formants, syllables, or
  speaker identity.

## Listening experiment

Listen to both generated files at matched playback volume:

1. `*_voice_candidate.wav`
2. `*_removed_tones.wav`

Try instrumental music, isolated speech, speech over music, and singing over
instrumental music. The key question is whether stable musical partials move
into the removed file while speech articulation remains intelligible in the
candidate. Keep the original alongside both outputs; do not judge only by
waveform appearance.

The older dynamic modulation-group and consonant/vowel experiment remains
available with `--legacy` for comparison. Its source is retained in the same
file, so the two approaches can be compared without losing the previous work.
