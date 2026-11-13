# voice_extractor — notes on the obtained results

Read of what the artifacts in this directory actually show. Numbers come from the
files in `tst0/`, `tst_files/`, and the root JSONs.

## What was run

- `tst0/*` is one run of `voice_music_self_template_entropy` on
  `tst_files/0.wav` (227.5 s, 44.1 kHz stereo): Mid/Side, 48-band scores,
  12 music candidates, adaptive subtraction, zones, mask acceptance.
- No `*_no_bursts.wav` exists, so `-no-bursts` was not used.
- `self_template.json` and `self_template_entropy.json` are byte-identical, and
  `tst0/self_template_entropy_repeats.json` is the same file again: three copies
  of one repeat scan, not three results.
- `out.wav_reconstruct.json` and `template_reconstruct_reconstruct.json` are
  byte-identical (same run, two prefixes). No reconstructed/residual WAVs were
  kept, so these numbers cannot be checked now.

## Measured

| Item | Value |
|---|---|
| Mid RMS / peak | 0.210 / 1.0 |
| Selected music bands | 12 of 48, centers 80–304 Hz |
| Music estimate vs Mid | corr 0.267, energy −3.6 dB |
| Adaptive alpha | 0.0346 (constant) |
| Residual identical to Mid | 95.1 % of samples |
| Removed energy | −19.5 dB |
| corr(Mid, residual) | 0.994 |
| Zones | 18 640 `voice_candidate`, 953 `music_filler` (95 % / 5 %) |
| Inferred `-too-quiet` | ≈ −1 dB (default is −24 dB) |
| Mask acceptance | 15 / 48 accepted, all contiguous 80–337 Hz |
| Repeated windows | 100 at sim 0.996–0.998, offset ≈ 59–60 s |
| Log/time-shift reconstruct | corr 0.989, SI-SDR 16.48 dB, avg mask 0.335 |

## What this establishes

1. **The "music reconstruction" is bass only.** The score
   `(1−entropy)·(0.5+0.25·beat+0.25·coherence)` ranks low bands highest, the
   candidate list is capped at 12, and all 12 selected centers are ≤ 304 Hz.
   Nothing above ~300 Hz is modelled, so the residual cannot be vocal-only.
2. **Adaptive subtraction is effectively off.** `music[i]` is the bandpass
   waveform multiplied by its own RMS-normalized envelope (envelope squared), so
   a single scalar fit yields alpha 0.035. 95 % of the residual is bit-identical
   to Mid; the removed part is −19.5 dB. `music_cut_residual.wav` ≈ Mid.
3. **The zone gate has no usable default scale.** `gRef` is a sum of narrow-band
   envelopes; the whole-mix RMS is not its yardstick. At the documented
   `-too-quiet -24` the threshold is 0.013 while median `gRef` is 0.034, so most
   frames would be `music_filler`. The actual run used ≈ −1 dB to get a 95/5
   split — an arbitrary cut, not a detected structure.
4. **The mask-acceptance test is self-fulfilling.** The 15 accepted targets are
   exactly the bands adjacent to the reference (80–337 Hz), where filterbank
   overlap makes the envelopes correlated by construction.
5. **The strongest finding is untapped: the track is a ~59.5 s loop.** 100
   windows match at 0.996–0.998 with a ~59–60 s offset. The repeats report is
   produced but never used for subtraction or alignment.
6. **The reconstruct metrics are not evidence of separation.** Correlation and
   SI-SDR are computed against the clean reference, but no contaminated-input
   baseline is reported (`si_sdr_db` alone says nothing about improvement), and
   the output audio is gone.
7. **`voice_profile.json` is corrupt; use `voice_profile.orig.json`.** One
   fragment has `target_start 10.848 / target_end 50.960` while its landmarks are
   still `26.848 / 26.960`. The program always writes the same value to both, so
   this was hand-edited. Transfer would copy a 40 s span from a 0.128 s template;
   `.orig` has 98 consistent fragments.

## Lessons for the next run

- Rank by music-likelihood, but select per octave (or all bands), not top-12.
  Low-frequency coherence wins the current score every time.
- Fit the band waveform with alpha near 1, or subtract `filtered_mid[b] * env`
  once. Do not multiply a waveform by its own envelope and then fit one scalar.
- Threshold `gRef` against its own reference-band scale (e.g. a quantile), not
  the full-mix RMS.
- Add a >3× duration-ratio check to profile output; high cosine does not catch
  degenerate matches.
- Report a baseline (contaminated vs clean) next to every SI-SDR, and keep the
  WAVs beside the JSON.
- Try the loop: align two ~59.5 s periods and subtract the accompaniment where
  it repeats. That is the one structure this material actually provides.
