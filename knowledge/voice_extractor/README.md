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
5. Fully mute bins classified as persistent peaks. This targets stable musical
   notes and partials; it can also remove sustained vowel harmonics.
6. Apply a low-frequency cut: retain only 8% of bins below 90 Hz, then smoothly
   increase the gain to full level at 180 Hz. This targets kick/bass energy.
7. Detect percussion in three bands using normalized positive spectral flux:
   - **Clap-like events:** simultaneous mid-band (250–900 Hz) and high-band
     (900–8000 Hz) attacks, a noise-like spectrum, and a rapid high-band flux
     drop. Attenuate 250–8000 Hz strongly, then recover gradually over about
     832 ms. Speech consonants can trigger this detector too.
   - **Drum-like events:** a strong low-band (below 250 Hz) attack with a
     rapid flux drop and sufficient low-band energy. Attenuate 35–420 Hz to
     10% for a six-frame event window (about 96 ms).
8. Find up to eight isolated, dynamically varying frequency bins as
   **modulation seeds**. For each other bin, compare its amplitude changes with
   each seed over time (allowing a small ±2-frame lag). When correlation is at
   least 0.72, apply the seed's smoothed, normalized amplitude envelope as a
   gain mask to that target bin.
9. Reconstruct the candidate with Hann overlap-add.

This is modulation cloning, not waveform cloning: a target bin retains its own
original STFT content. Only its gain/envelope follows a selected seed. Bins
that do not match a seed are not recovered by this step. Seed selection and
correlation are deliberately simple heuristics and can make false matches.

The separate `--legacy` path still contains the earlier modulation-group and
consonant/vowel experiment. The default path now also has the simpler seed-based
modulation propagation described above.

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
| Persistent-tone gain | 0% (fully muted) |
| Low-frequency gain | 0% below 90 Hz; ramps to 100% at 180 Hz |
| Clap detector | High-band flux > 0.48, mid-band flux > 0.34, flatness > 0.18, global flux > 0.30, with a next-frame high-flux drop |
| Clap attenuation | Starts at 2% gain from 250–8000 Hz; gradual recovery/release over about 832 ms |
| Drum detector | Low-band flux > 0.72, low-band energy > 12% of mid+high energy, with a next-frame low-flux drop |
| Drum attenuation | 90% from 35–420 Hz for up to 6 frames (96 ms) |
| Modulation seeds | Up to 8 isolated, dynamically varying bins; 180–7000 Hz |
| Modulation matching | Pearson correlation ≥ 0.72; ±2-frame lag |
| Modulation mask | Smoothed seed envelope, clamped to 15–100% gain |
| Reconstruction | Hann overlap-add |

These are deliberately aggressive starting values for listening tests, not
validated optimal settings. The hard spectral cuts may remove parts of speech
that overlap instrumental frequencies.

## Voice-candidate amplitude normalization

The default destructive pass peak-normalizes `*_voice_candidate.wav` to a
0.95 full-scale peak after reconstruction. This compensates for level loss
from filtering and modulation gating. The gain is applied to the candidate
only; `*_removed_tones.wav` remains the unnormalized input-minus-filtered
residual, so the two files are not intended to sum sample-for-sample after
candidate normalization. Near-silence (peak at or below 1e-6) is left alone.

## Limitations and things to listen for

- **Low voices:** the hard sub-90 Hz cut and fade to 180 Hz can thin male voices
  and remove useful vocal body.
- **Vowels:** sustained vowel harmonics can look like persistent musical tones
  and be completely removed.
- **Consonants:** plosives and sharp consonants can resemble clicks or claps
  and may be heavily attenuated for much of the recovery window.
- **Percussion:** sustained cymbals and noisy percussion may not produce the
  expected attack/decay pattern and can survive. Some bass notes may also
  trigger the low-band drum detector.
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

Ask whether kick/bass, stable instrumental partials, and clap/drum attacks
move into the removed component while speech remains intelligible in the
candidate. Pay special attention to low male voices and consonants such as
P, T, K, S, and F. Keep the original recording for comparison; waveform
appearance alone does not establish separation quality.

The older dynamic modulation-group and consonant/vowel experiment remains in
the same source file and can be selected with `--legacy`.


## New experiment: template-powered voice reconstruction

A separate executable, `voice_reconstruct`, builds a small **formant/spectral
alphabet** from a clean isolated voice recording and uses it to synthesize a
new voice from a source mix.

The source recording is not used as audio material in the output. It supplies:

- a frame-by-frame pitch contour (including vibrato when pitch tracking is
  confident);
- dominant seed-like spectral peaks across 18 frequency bands, used to select
  likely template frames;
- a restrained loudness contour.

The clean template supplies the actual sound character:

- 16th-order LPC spectral envelopes (a compact approximation of vocal
  resonances/formants);
- spectral descriptors for matching;
- empirical occurrence probabilities for recurring spectral/formant patterns.

Template frames are analyzed every 10 ms using 25 ms windows. The matching
stage compares the strongest local spectral peak in each of 18 frequency
bands and adds a small continuity cost
to discourage rapid frame-to-frame switching. The selected template LPC envelope is excited by a smooth harmonic glottal-like
signal at the source pitch for voiced frames, or deterministic noise for
unvoiced/uncertain frames. The result is a newly synthesized signal; no source
waveform samples are copied or mixed into it.

The synthesis stage uses **reflection coefficients** rather than interpolating
raw LPC coefficients. Reflection coefficients are bounded and interpolated
between adjacent template frames, then converted back into a stable LPC filter.
This avoids the unstable-filter problem in the first prototype. The voiced
excitation is a multi-harmonic periodic waveform rather than a one-sample
impulse, and the output level is normalized by RMS (with a peak limiter) rather
than allowing one isolated spike to set the entire file's gain. Non-finite and
runaway filter samples are guarded before WAV writing.

### Build and run

```sh
make clean && make
./voice_reconstruct source_mix.wav clean_singer_template.wav reconstructed.wav
```

Both WAV files must be PCM16 mono at 16 kHz. Convert stereo or other sample
rates first, for example:

```sh
ffmpeg -i input.wav -ac 1 -ar 16000 -c:a pcm_s16le converted.wav
```

Use a clean template containing the same singer where possible. A template
with a range of vowels, consonants, and transitions gives the alphabet more
coverage. A short template containing only one vowel will produce repetitive
and incomplete results.

### Important experimental limits

- This is deterministic DSP, not phoneme recognition or a trained voice
  conversion system.
- The current matcher uses coarse spectral evidence, not semantic or
  phonetic recognition. Music can therefore cause the wrong template frame
  to be selected.
- Pitch tracking from a dense mix can make octave errors or follow bass
  instruments. Vibrato is retained only to the extent that the source pitch
  tracker follows it correctly.
- The source loudness contour is clamped, but loud percussion can still
  influence it.
- LPC is an approximation of the vocal spectral envelope. It does not
  reproduce every detail of the original singer's articulation.
- The first build is a proof-of-concept. Listen to the result and compare
  different template recordings before judging the method.

The original `voice_extractor` remains available and unchanged as the
destructive-filtering experiment. `voice_reconstruct` is a separate path.

# Log/time-shift vocal reconstruction experiment

This experiment tests a time-shifted logarithmic feature derived from a clean voice template and a contaminated recording.

It is a separate experimental path from:

- `voice_extractor` — destructive filtering and modulation-coherence experiment.
- `voice_reconstruct` — template-powered voice synthesis.
- `voice_separate` — template-guided source-preserving separation.

## Feature concept

For a signal \(S\), the proposed feature is:

\[
Y[i]=\ln\left(
\sum_{k=b}^{t}
\frac{S[i+k]+S[i+k+\text{shift}]}{2}
+\epsilon
\right)
\]

The `shift` parameter represents a time shift, not a frequency shift.

For STFT-based processing, this operation is evaluated independently for each frequency bin over time. A small epsilon prevents taking the logarithm of zero.

The feature is intended to describe local temporal structure. It does not uniquely identify voice: percussion, harmonics, and other instruments may produce similar features.

## Synthetic percussion benchmark

The benchmark contaminates a clean vocal template with synthetic percussion at several signal-to-interference ratios (SIRs).

The generated test cases include:

| SIR | Meaning |
|---:|---|
| +12 dB | Voice dominates |
| +6 dB | Voice is stronger |
| 0 dB | Equal voice and interference power |
| −6 dB | Interference is stronger |
| −12 dB | Strong interference |

The benchmark produces:

- `clean.wav` — original template
- `synth_percussion.wav` — generated percussion
- `contaminated_sir_*.wav` — contaminated test recordings
- `results.json` — baseline measurements

The baseline measurements describe the contaminated input. They are not evidence that reconstruction has succeeded.

## Log/time-shift reconstruction

The planned `voice_template_reconstruct` executable uses the clean template to calculate a reference log/time-shift feature and compares it with the contaminated recording.

The reconstruction experiment should report:

- Correlation with the clean template
- SI-SDR
- Residual RMS
- Feature parameters
- Average applied mask or reconstruction statistics

The reconstruction and residual outputs should both be retained for listening tests.

**Experimental limitation:** Log/time-shift feature similarity alone cannot reliably distinguish voice from percussion. This is a deterministic DSP experiment, not a validated vocal separator.

## Build requirements

The benchmark and reconstruction experiments use C++17, libsndfile, and FFTW3.

On Debian/Ubuntu, install the development packages:

```sh
sudo apt install build-essential libsndfile1-dev libfftw3-dev
```

Build the project:

```sh
make clean
make
```

## Run the benchmark

```sh
mkdir -p benchmark

./voice_template_benchmark template.wav benchmark
```

The benchmark writes contaminated WAV files and `results.json` into the output directory.

## Run reconstruction

```sh
./voice_template_reconstruct \
    template.wav \
    benchmark/contaminated_sir_-12.wav \
    benchmark/template_reconstruct_-12
```

The intended output files are:

```text
benchmark/template_reconstruct_-12_reconstructed.wav
benchmark/template_reconstruct_-12_residual.wav
benchmark/template_reconstruct_-12_results.json
```

The output prefix's parent directory must already exist.

## Listening and evaluation

Compare the following at matched playback volume:

1. Original clean template
2. Synthetic percussion
3. Contaminated input
4. Reconstructed candidate
5. Residual

Repeat the test at each SIR. Listen for vocal harmonics and consonants that may be attenuated, and percussion that may leak into the reconstruction.

Do not judge separation quality from correlation or SI-SDR alone. Listen to both output components and compare them with the original recordings.

## Current status

- Synthetic contamination benchmark: implemented and run.
- Baseline metrics: available in the benchmark `results.json`.
- Log/time-shift reconstruction: experimental implementation in
  `voice_template_reconstruct`.
- Reconstruction output quality: not yet validated.

