# voice_extractor

Groups frequency bands by **shared amplitude modulation**.

The working hypothesis is that frequency regions produced by the same voice
move together in time: syllabic pulses, consonant bursts, and phrase-level
dynamics appear in multiple bands with similar timing. The extractor therefore
compares each band's amplitude envelope rather than comparing its carrier
frequency or pitch.

## Pipeline

1. Split 16 kHz mono audio into 16 overlapping RBJ band-pass regions.
2. Rectify and smooth each band's signal into an amplitude envelope.
3. Downsample envelopes to 2 kHz and remove their slow local level.
4. Compare every pair using normalized cross-correlation, allowing ±20 ms
   timing offset.
5. Connect bands whose modulation similarity is at least `0.48` and whose
   dominant modulation clocks are compatible.
6. Connected components are modulation groups. Sum the corresponding
   band-pass signals and write one WAV per group.

## Build / run

```sh
c++ -O2 -std=c++17 -Wall -Wextra voice_extractor.cpp -o voice_extractor
./voice_extractor input.wav
# or:
./voice_extractor input.wav ./out/voice
```

Input must be PCM16, mono, 16 kHz. Convert with:

```sh
ffmpeg -i input.wav -ar 16000 -ac 1 -c:a pcm_s16le input_16k.wav
```

Outputs are `<prefix>_group_00.wav`, etc.

## Important limitations

This is a first-pass modulation-based source grouping, not full blind source
separation. Two speakers who speak in synchrony can merge. A single speaker
whose bands have different envelope shapes can split. Reverberation, music,
strong background noise, and very short clips can reduce coherence. Threshold
`0.48`, the ±20 ms lag allowance, and the band layout are tunable parameters.

A modulation group is not automatically a voice: it means only that those
frequency bands share sufficiently similar envelope motion. Validate groups
against real recordings before treating them as isolated speakers.
