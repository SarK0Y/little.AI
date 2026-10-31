# voice_extractor

Experimental, deterministic speech-region extraction using a narrow
filterbank, time-varying modulation groups, and a primitive consonant/vowel
(C/V) event gate.

**Design constraint:** this implementation deliberately avoids trained
statistical models, neural networks, training corpora, and reference-voice
databases. It uses explicit signal-processing measurements and hand-chosen
thresholds. It is a research prototype, not a production speech/music
separator.

## What it tries to detect

Speech is not merely a set of frequencies that rise and fall together. It is
an evolving sequence of articulatory events: noisy or constricted consonant
regions, voiced resonant vowel regions, and transitions between them.

The current implementation approximates two event classes:

- **C — consonant-like:** an energetic frame with weak periodicity, a high
  zero-crossing rate, and a relatively strong high-frequency component.
- **V — vowel-like:** a periodic frame whose filterbank envelope has at least
  two broad local peaks. Those peaks are only a crude resonance/formant
  proxy; they are not accurate LPC formant estimates.

A V frame is retained when a C frame occurs within 220 ms on either side.
The accepted region is expanded slightly to preserve transitions and
coarticulation. **Important:** this first version uses temporal proximity, not
strict directional C-before-V grammar, and it does not recognize phonemes,
syllables, words, or speakers.

## Signal path

1. Read PCM16, mono, 16 kHz RIFF/WAVE audio.
2. Split the signal into 48 logarithmically spaced band-pass filters covering
   70 Hz–7.2 kHz, with Q = 6.
3. Track each band's rectified, smoothed amplitude envelope (8 ms smoothing),
   decimated to 2 kHz.
4. Every 100 ms, analyze a 1.2 s local envelope window. Bands with sufficiently
   similar local envelope movement are grouped using complete-link Pearson
   correlation (threshold 0.55). Group IDs are tracked between windows by
   shared-band overlap, so membership can change over time.
5. Independently analyze 25 ms waveform frames at 10 ms intervals. For each
   frame, measure:
   - normalized autocorrelation periodicity over the 75–400 Hz pitch range;
   - zero-crossing rate;
   - energy ratio in filterbank bands centered at or above 2.5 kHz;
   - a simple count of broad peaks in the filterbank envelope.
6. Apply hand-written C/V rules and construct a soft speech gate by
   interpolating the event mask between analysis frames.
7. Sum each tracked frequency group into its own PCM16 WAV file, optionally
   applying the speech gate.

## Build

Requires a C++17 compiler and `make`.

```sh
make clean && make
```

The Makefile uses `-O2 -std=c++17 -Wall -Wextra -Wpedantic` by default.

## Input format

The input must be **16-bit PCM, mono, 16,000 Hz WAV**. Convert other formats
with ffmpeg:

```sh
ffmpeg -i input.ext -ar 16000 -ac 1 -c:a pcm_s16le input_16k.wav
```

## Run

```sh
# Speech-event gate enabled (default); output files use voice_group_NN.wav
./voice_extractor input_16k.wav

# Choose an output prefix; the parent directory must already exist
./voice_extractor input_16k.wav ./out/voice

# Disable the C/V gate and hear the dynamic modulation groups alone
./voice_extractor input_16k.wav ./out/raw --raw
```

The output directory is not created automatically. For example, create it
before using `./out/voice`:

```sh
mkdir -p out
```

Each output is named `<prefix>_group_00.wav`, `<prefix>_group_01.wav`, etc.
The program prints the number of tracked groups and the detected C, V, and
paired-V event counts. Group IDs are heuristic frequency-group tracks; they
are **not verified speaker identities**. A single speaker may produce
multiple groups, and multiple sources may be merged.

## Current thresholds

The initial values are intentionally visible in `voice_extractor.cpp` so
they can be tuned against real recordings:

| Measurement | Current rule |
|---|---:|
| C-frame periodicity | < 0.32 |
| C-frame zero-crossing rate | > 0.075 |
| C-frame high-band energy ratio | > 0.10 |
| V-frame periodicity | > 0.38 |
| V-frame resonance score | >= 0.40 |
| C-to-V temporal search | +/- 220 ms |
| Event analysis frame / hop | 25 ms / 10 ms |
| Grouping window / hop | 1.2 s / 100 ms |
| Group envelope correlation | >= 0.55 |

The C/V thresholds also require frame RMS above a relative/absolute noise
floor. The high-band ratio is calculated from filterbank envelope energy,
not from a separate FFT classifier.

## Limitations and next experiments

This is a deliberately small first test of an acoustic-event grammar. Expect
false positives and false negatives:

- Percussion, pick attacks, distortion, and noisy instruments can resemble C
  events.
- Sustained pitched instruments can resemble V events.
- Voiced consonants may fail the current C rule; weak vowels may fail the V
  rule; whispered speech lacks the periodicity expected by this V detector.
- Fricatives and plosives have different time/frequency shapes, but are
  currently treated as one broad C class.
- The filterbank peak count is not robust formant tracking. LPC or another
  explicit spectral-envelope estimator may improve the vowel test without
  requiring a learned model or database.
- The current C/V test checks for a nearby consonant-like frame on either
  side of a vowel-like frame. A future version should test directional event
  sequences, transition shape, and repeated syllable-like patterns.
- Singing vocals, heavily processed voices, overlapping speakers, and dense
  music remain difficult cases.

For listening tests, compare the default gated output with `--raw`. Use
isolated speech, instrumental music, speech over music, and music with
singing. Listen for both missed speech and music leaking through. Change one
threshold at a time and keep examples of each failure; the goal is to improve
the physical rules, not to hide errors behind a trained classifier.
