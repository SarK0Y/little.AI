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

# Voice Template Profile Transfer

Experimental C++ tool for locating vocal-like fragments in a target recording
using a template WAV, estimating a local duration/speed ratio, and applying a
template-derived amplitude envelope to the matched target fragments.

## What it does

The program has two modes:

1. **`profile`** — scans `template.wav` for active regions, searches the target
   for spectrally similar regions, and writes fragment boundaries, landmark
   times, a speed estimate, and a cosine-similarity score to JSON.
2. **`transfer`** — reads the JSON profile, transfers a template-derived
   amplitude envelope onto the matched target fragments, and writes a WAV.
   Samples outside matched fragments are zero (silence).

This is an experimental DSP prototype, not a reliable vocal separator.

## Important limitations

- The current descriptor is a coarse log-spectral-envelope proxy. It is **not**
  proper LPC formant tracking and cannot reliably identify a singer, word, or
  phrase by itself.
- A high similarity score is not proof that the match is correct. Listen to the
  target and inspect the JSON before running transfer.
- The output preserves the target waveform inside accepted fragments and
  changes its amplitude envelope. It does not synthesize a new voice or perform
  pitch-preserving time stretching.
- The reported speed ratio is an average interval ratio:
  `template_interval_duration / target_interval_duration`.
  Values above 1 mean the target interval is shorter/faster than the template.
- Sample rates must match. Resample either WAV first if necessary.
- Input is converted to mono by averaging channels.

## Requirements

Linux (or another platform with compatible development packages), a C++17
compiler, `libsndfile`, and FFTW3.

On Debian/Ubuntu:

```bash
sudo apt install build-essential libsndfile1-dev libfftw3-dev
```

## Build

```bash
make
```

This builds:

```text
voice_template_profile_transfer
```

## Usage

### 1. Create the fragment profile

```bash
./voice_template_profile_transfer profile \
    template.wav target.wav voice_profile.json
```

The output JSON includes each accepted fragment's:

- template start/end time
- target start/end time
- template and target landmark A/B
- `speed_ratio_template_over_target`
- `time_scale_target_over_template`
- `confidence_cosine`

The matcher uses a conservative similarity cutoff in this prototype. If the
JSON contains no fragments, the matcher did not accept any candidates.

### 2. Apply the envelope profile

```bash
./voice_template_profile_transfer transfer \
    template.wav target.wav voice_profile.json target_vocal_profile.wav
```

Only the accepted target fragments are written; the rest of the output is
silence.

## Make variables

The Makefile provides convenience targets:

```bash
make profile
make run-profile TEMPLATE=template.wav TARGET=target.wav PROFILE_JSON=voice_profile.json

make run-transfer TEMPLATE=template.wav TARGET=target.wav \
    PROFILE_JSON=voice_profile.json OUTPUT=target_vocal_profile.wav
```

Defaults:

| Variable | Default |
|---|---|
| `TEMPLATE` | `template.wav` |
| `TARGET` | `target.wav` |
| `PROFILE_JSON` | `voice_profile.json` |
| `OUTPUT` | `target_vocal_profile.wav` |

## Suggested workflow

1. Start with a short, clean template phrase.
2. Use a target containing the same or acoustically similar vocal material.
3. Generate `voice_profile.json`.
4. Check every fragment boundary and confidence score manually.
5. Run transfer only after the matches look reasonable.
6. Listen to the output and compare it with the original target.

For different lyrics or substantially different articulation, formant/spectral
matching may fail. The next development step should be robust formant tracking
and multi-landmark alignment, with a user-reviewable match threshold.

# Music-Dominant Reference-Band Experiment

Experimental C++17 DSP prototype for ranking narrow frequency bands that may
serve as **musical-temporal references** in a mixed recording.

The working hypothesis is that some bands have relatively low temporal
conditional entropy and preserve useful beat/music timing even when the full
mix contains vocals. These bands may later help predict musical structure in
vocal-contaminated regions.

**This tool does not separate vocals and music.** It ranks candidate bands and
exports audio for listening and further experiments.

## What it does

The program:

1. Reads an audio file with libsndfile.
2. Creates a Mid/Side representation from the first two channels.
3. Analyzes 48 logarithmically spaced narrow bands, approximately 1/6-octave
   bandwidth, from 80 Hz up to `min(10,000 Hz, 0.45 × sample rate)`.
4. Computes separate temporal measurements for each band:
   - **Transition entropy:** normalized conditional entropy of transitions
     between 8 quantized envelope levels. Lower values mean more predictable
     envelope transitions.
   - **Beat-like periodicity:** strongest normalized envelope autocorrelation
     over lag values corresponding approximately to 60–240 BPM.
   - **Cross-band coherence:** strongest absolute Pearson envelope correlation
     with a non-neighboring band (at least two filterbank indices away).
   - **RMS:** band signal level.
5. Ranks bands with a heuristic score.
6. Exports the top 12 bands and audible envelope tracks.
7. Keeps the earlier coarse repeated-window scan on Mid and Side.

## Ranking score

The current heuristic is:

```text
score = (1 - transition_entropy)
        * (0.50 + 0.25 * beat_periodicity
                 + 0.25 * cross_band_coherence)
```

Each normalized metric is in `[0, 1]`.

Entropy is the primary gate in this ranking: beat periodicity and cross-band
coherence refine the score. The score is **not a probability** and is not
evidence of a music-only band by itself.

### Important interpretation

Low entropy means the band’s envelope transitions are more predictable. It
does **not** establish that the band contains music rather than voice or noise:
a sustained vocal note, stationary noise, or other regular signal may also
have low entropy.

The beat metric is only an autocorrelation-based periodicity indicator, not a
beat tracker. Cross-band coherence is correlation, not proof that two bands
belong to the same source.

The intended next validation is listening and testing whether reference-band
timing predicts musical events in other bands. The presumed music-to-vocal
energy ratio (for example, music dominance above 0.9) is not directly measured
from a single mixed recording by this prototype.

## Requirements

- C++17 compiler
- `make`
- libsndfile development package
- FFTW3 development package

On Debian/Ubuntu:

```bash
sudo apt install build-essential libsndfile1-dev libfftw3-dev
```

## Build

Build just this experiment:

```bash
make voice_music_self_template_entropy
```

`make` also builds it as part of the default `all` target. It requires
libsndfile and FFTW3 (see [Requirements](#requirements)).

Compiling directly:

```bash
c++ -O2 -std=c++17 -Wall -Wextra -Wpedantic \
    voice_music_self_template_entropy.cpp \
    -lsndfile -lfftw3 -lm \
    -o voice_music_self_template_entropy
```

## Usage

```bash
./voice_music_self_template_entropy input_stereo.wav [output_prefix] [report.json]
```

Example:

```bash
mkdir -p out
./voice_music_self_template_entropy song.wav out/song out/song_repeats.json
```

Arguments:

| Argument | Meaning | Default |
|---|---|---|
| `input_stereo.wav` | Input audio file; required | — |
| `output_prefix` | Prefix for generated files | `stereo` |
| `report.json` | Repeated-window report path | `<prefix>_repeats.json` |

The input must have at least two channels. For files with more than two
channels, the program uses only the first two channels. The input sample rate
is retained; the program does not resample.

## Outputs

For prefix `out/song`:

| Output | Description |
|---|---|
| `out/song_mid.wav` | Mid signal: `(L + R) / 2` |
| `out/song_side.wav` | Side signal: `(L - R) / 2` |
| `out/song_reference_band_scores.csv` | All 48 bands ranked by heuristic score |
| `out/song_ref_1_<frequency>Hz.wav` … | Filtered audio for top 12 candidate bands |
| `out/song_ref_1_<frequency>Hz_envelope.wav` … | 220 Hz tone amplitude-modulated by each band’s envelope |
| `out/song_repeats.json` | Coarse repeated spectral-window candidates |

The CSV columns are:

```text
rank, band, center_hz, rms, transition_entropy,
beat_periodicity, cross_band_coherence, heuristic_score
```

The envelope WAVs are **audible envelope monitors**, not reconstructed music.
They make it easier to compare the rhythm and modulation of candidate bands.


## Experimental music-mask acceptance test

The current experimental branch additionally uses the top-ranked reference
band's envelope to create a **reference-driven gain mask** for each target
band. It preserves the target band's carrier/phase and attenuates it according
to the reference envelope; it does **not** subtract a phase-unrelated music
waveform.

Definitions used by this experiment:

- `V[j]`: original filtered target-band signal.
- `Y[j]`: target-band output after applying the reference-envelope gain mask.
- `G[j]`: generated music-activity envelope, formed by multiplying the
  normalized reference-band envelope by the original target-band envelope.
- `H`: normalized transition entropy of the envelope (same 8-level conditional
  entropy proxy described above).
- `F`: average fade length in envelope frames. For each detected local peak
  above 5% of the envelope's global peak, count frames until the envelope
  falls below 10% of that local peak. The search is capped at about 100 frames
  (roughly one second at the program's 512-sample envelope hop and common
  audio sample rates). Peaks that never cross the threshold in the window
  are omitted.

A target is marked `accepted=1` only when **all three** user-defined conditions
hold:

```text
H(Y[j]) >= H(V[j])
F(Y[j]) >= F(V[j])
abs(Corr(G[j], V[j])) > abs(Corr(G[j], Y[j]))
```

The CSV `*_music_mask_acceptance.csv` contains these metrics. The program also
writes one `*_music_mask_ref_<ref>_target_<band>_Y.wav` output for each target
band, whether or not the three tests accept it. The CSV is a diagnostic
acceptance flag, not a quality guarantee.

**Important:** this is a heuristic experiment, not a validated vocal remover.
The gain mask may suppress vocal energy that coincides with the music envelope,
and its entropy/fading/correlation checks do not prove that vocals were retained
or that music was removed. Listen to the candidate WAVs and compare them with
the original.

### Build and run the new experiment

```bash
make voice_music_self_template_entropy
make run-music-mask-experiment INPUT=song.wav PREFIX=out/song
```

`run-music-mask-experiment` passes `INPUT`, `PREFIX`, and `REPORT` to the
program. Defaults are `INPUT=template.wav`, `PREFIX=stereo_analysis`, and
`REPORT=$(PREFIX)_repeats.json`, so normally you will override at least
`INPUT`. Create the output prefix's parent directory first.

The Makefile also provides `music-mask-experiment` (build only) and
`voice_music_self_template` / `run-music-self-template` for the older scanner.

## Suggested listening / validation workflow

1. Run the program on a representative stereo mix.
2. Open `*_reference_band_scores.csv` and inspect the top candidates.
3. Listen to each top `*_ref_*Hz.wav` at a matched volume.
4. Compare its corresponding `*_envelope.wav` with the original recording.
5. Include contrasting material:
   - instrumental music without vocals;
   - isolated speech;
   - speech over music;
   - singing over music;
   - sustained vocal notes and sustained/noisy instruments.
6. Check whether low-entropy candidates actually preserve musical beat timing
   and whether that timing predicts activity in other frequency regions.
7. Do not label a band “music-only” based on its score or on listening to a
   single short excerpt.

## Current limitations

- This is an exploratory heuristic, not a validated separator.
- Entropy is calculated from a quantized band-envelope transition sequence;
  it is not the entropy of the original audio source.
- The bandpass filterbank uses fixed-Q biquad filters and overlapping bands.
- Beat periodicity searches a limited lag range; unusual tempos, syncopation,
  or non-periodic music may score poorly.
- Cross-band coherence can be high for vocals, shared noise, or unrelated
  sources with correlated dynamics.
- The score does not estimate the music/vocal energy ratio.
- The filtered reference-band exports are diagnostic signals, not isolated
  instrumental stems.
- Repeated-window spectral similarity can match recurring vocals or similar
  arrangements; it is not proof of accompaniment identity.
- No neural network, database, or external source template is used.

Keep the original recording and evaluate all outputs by listening. A ranking
score alone cannot establish separation quality.
