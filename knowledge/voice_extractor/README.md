

# voice_extractor

A small C++ experiment that groups frequency bands according to how similarly their **amplitude envelopes move over time**.

The working hypothesis is that frequency regions belonging to the same voice often share timing: syllable energy, consonant bursts, and phrase dynamics appear across several bands. The program looks for that shared envelope motion.

It does **not** identify speakers or perform full blind source separation.

## Signal path

1. Read a PCM16, mono, 16 kHz RIFF/WAV file.
2. Split it into 16 overlapping biquad band-pass filters, centred from 90 Hz to 7 kHz (`Q = 1.35`).
3. For each band, form an amplitude envelope with full-wave rectification (`abs(sample)`) followed by a one-pole exponential low-pass smoother (8 ms time constant).
4. Keep every eighth envelope sample, giving a 2 kHz envelope stream.
5. Remove the slow local envelope level with a 125 ms-radius moving-average detrend. This emphasizes changing energy rather than a band's steady level.
6. Compare every pair of complete, detrended envelopes using Pearson correlation. The comparison searches offsets from -40 ms to +40 ms and computes means and energies over the actual overlapping samples.
7. Join two bands when their best positive correlation is at least `0.32`. Connected components of this similarity graph become the output groups.

Speech modulation is often irregular, so the grouping decision does **not** require a stable periodic modulation rate.

The printed `mod-Hz` value is only an optional autocorrelation diagnostic; a zero value does not disqualify a band from grouping. The printed strength is a relative envelope-variation measure, not a confidence score or probability.

## Build

From this directory:

```sh
make
```

Or build directly:

```sh
c++ -O2 -std=c++17 -Wall -Wextra -Wpedantic \
    voice_extractor.cpp -o voice_extractor -lm
```

Remove generated build files with:

```sh
make clean
```

## Run

```sh
./voice_extractor input.wav
```

By default, output files are named:

```text
voice_group_00.wav
voice_group_01.wav
...
```

You can choose another output prefix:

```sh
./voice_extractor input.wav ./out/speaker
```

This writes `./out/speaker_group_00.wav`, and so on. The output directory must already exist.

If your input is not PCM16 mono at 16 kHz, convert it first:

```sh
ffmpeg -i input.wav -ar 16000 -ac 1 -c:a pcm_s16le input_16k.wav
```

The program also prints a pairwise modulation-similarity matrix and a table showing each band's centre frequency, optional modulation-rate diagnostic, relative modulation strength, and assigned group.

## Interpreting the result

A group means only that its bands have sufficiently similar envelope motion under this comparison. It is evidence of shared modulation, **not proof that the bands came from one speaker**.

The program sums the original band-pass signals in each group and writes that sum as a WAV file. It keeps singleton groups too: a band that does not correlate strongly enough with another band is still written as its own group.

## Current limitations

- Two speakers with synchronized speech or similar dynamics may be merged.
- One speaker can be split across groups when different frequency regions have dissimilar envelopes.
- Music, percussion, reverberation, background noise, and short clips can distort the similarity measurements.
- The fixed `0.32` similarity threshold and ±40 ms lag range are experimental parameters, not universal values.
- Summing selected bands is not source reconstruction; it can sound thin, incomplete, or noisy because the filter bank does not isolate a voice by itself.
- This is a first-pass modulation-coherence experiment, not a production speaker-separation system.

Treat the similarity matrix as a diagnostic when tuning the threshold against real recordings.


