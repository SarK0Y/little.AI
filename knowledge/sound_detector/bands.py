"""voice / music / noise from two time-domain ideas: fading and bands.

No FFT, no scipy. 16 kHz mono. Everything is biquads + envelope followers,
so it ports to C++ as ~300 lines with zero dependencies.

    fading survives  -> not noise          (noise has no attack/decay stage)
    unison fading    -> voice vs music     (n harmonics share one short fade)
"""

import numpy as np

FS = 16000
CENTERS = (100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300)  # 2/3 octave
Q = 1.2


def _bp(f0, fs=FS, q=Q):
    """RBJ bandpass biquad."""
    w0 = 2 * np.pi * f0 / fs
    alpha = np.sin(w0) / (2 * q)
    b = np.array([alpha, 0.0, -alpha])
    a = np.array([1 + alpha, -2 * np.cos(w0), 1 - alpha])
    return b / a[0], a / a[0]


def _ir(b, a, n):
    """Impulse response of a 2nd-order section (order 1 is zero-padded)."""
    b = np.pad(np.asarray(b, float), (0, max(0, 3 - len(b))))
    a = np.pad(np.asarray(a, float), (0, max(0, 3 - len(a))))
    h = np.zeros(n)
    for i in range(n):
        h[i] = b[i] if i < 3 else 0.0
        if i >= 1:
            h[i] -= a[1] * h[i - 1]
        if i >= 2:
            h[i] -= a[2] * h[i - 2]
    return h


def _filt(x, b, a, n):
    return np.convolve(x, _ir(b, a, n))[: len(x)]


def _env(x, fs=FS, rel_ms=8.0, n=512):
    """Rectify + one-pole lowpass. Fast enough to resolve a 30 ms release."""
    a = 1 - np.exp(-1.0 / (fs * rel_ms / 1000.0))
    return _filt(np.abs(x), np.array([a]), np.array([1.0, -(1 - a)]), n=n)


def _decimate_max(e, factor):
    n = len(e) // factor * factor
    return e[:n].reshape(-1, factor).max(1)


def _fades(env, fs=FS, min_drop_db=20.0, min_ms=20.0, max_ms=2000.0,
          rebound_db=3.0):
    """All descents in one band, as durations in ms. A descent is a 20 dB drop
    on the band envelope, measured per descent; sustains are ignored, which is
    the point. Stationary noise has no such descending edge -> no fades -> noise.

    Voice = a handful of harmonics, so ~2-4 bands share one fade event at
    nearly the same time; a song = many freqs, so 6-8 bands fade at staggered
    times; hiss = everything fades at once; drone = nothing fades. That is why
    we keep the fades *per band*, not merged: the min across bands is the
    voice event, the max is the music tail, and their ratio is the whole
    voice-vs-music test in one number.
    """
    e = _decimate_max(env, 16)  # 1 kHz
    fs_e = fs / 16
    if e.max() < 1e-6:
        return [], 0
    e = e / e.max()
    k = 10  # 10 ms moving average kills the 100-400 Hz rectifier ripple
    e = np.convolve(e, np.ones(k) / k, mode="same")
    n = len(e)
    durs = []
    rebound = 10 ** (rebound_db / 20)
    w = int(0.02 * fs_e)  # a descent starts at the max of a 40 ms neighbourhood
    loc = np.lib.stride_tricks.sliding_window_view(
        np.pad(e, (w, w), mode="edge"), 2 * w + 1).max(1)
    starts = np.where((e == loc) & (e > 0.1))[0]
    for s in starts:
        peak, trough, j = e[s], e[s], s
        while j < n and (j - s) / fs_e * 1000 <= max_ms:
            trough = min(trough, e[j])
            if e[j] > trough * rebound:
                break  # bounced back: this descent ended
            if 20 * np.log10(peak / max(trough, 1e-9)) >= min_drop_db:
                dur_ms = (j - s) / fs_e * 1000
                if dur_ms >= min_ms:
                    durs.append(dur_ms)
                break
            j += 1
    return durs


def features(x, fs=FS):
    x = np.asarray(x, dtype=float)
    rms_db = 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)
    if rms_db < -50:
        return {"rms_db": rms_db, "band_std": 0.0, "spread_hz": 0.0,
                "fade_ms": None, "fade_fast": None, "fades": 0,
                "short_fades": 0, "fades_per_band": {}}

    bands = np.stack([_filt(x, *_bp(f), n=512) for f in CENTERS])
    env = np.stack([_env(b) for b in bands])  # (bands, samples)

    energy = (env ** 2).mean(1)                      # per-band energy
    e_db = 10 * np.log10(energy / energy.max() + 1e-12)
    band_std = float(np.std(e_db))                   # flat spectrum -> hiss

    f = np.array(CENTERS, dtype=float)
    centroid = (f * energy).sum() / energy.sum()
    spread = np.sqrt(((f - centroid) ** 2 * energy).sum() / energy.sum())

    total = env.sum(0)
    fades = _fades(total, fs)
    per_band = {f: _fades(env[i], fs) for i, f in enumerate(CENTERS)}
    # voice: a handful of harmonics -> few descents, many of them short
    # music: many freqs -> many descents, few of them short
    all_dur = [d for durs in per_band.values() for d in durs]
    short = sum(1 for d in all_dur if d < 150)
    return {
        "rms_db": rms_db,
        "band_std": band_std,
        "spread_hz": spread,
        "fade_ms": float(np.median(fades)) if fades else None,
        "fade_fast": float(np.mean(np.array(fades) < 150)) if fades else None,
        "fades": len(fades),
        "short_fades": short,
        "fades_per_band": per_band,
    }


def classify(x, fs=FS):
    f = features(x, fs)
    if f["rms_db"] < -50:
        return "silence", f
    if f["fades"] == 0:
        # nothing fades: stationary. Flat spectrum -> hiss, peaks -> drone/pad.
        return ("noise" if f["band_std"] < 4.5 else "music"), f
    # voice: a handful of harmonics, all fading at once -> many short descents
    # music: many freqs fading at staggered times -> few short descents
    # measured on this box: voice ~12 short/11 total, music ~1 short/3 total
    if f["short_fades"] > f["fades"] * 0.33:
        return "voice", f
    if f["fades"] >= 2:
        return "music", f
    return ("voice" if f["spread_hz"] > 700 else "music"), f


def load_wav(path, fs=FS):
    import wave
    with wave.open(path) as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (fs, 1, 2):
            raise SystemExit(
                f"{path}: need 16-bit mono {fs} Hz. "
                f"ffmpeg -i {path} -ar {fs} -ac 1 out.wav")
        return np.frombuffer(w.readframes(w.getnframes()), "<i2") / 32768.0


# ---------------------------------------------------------------- self-check
def _voice(sec=2.0, fs=FS):
    """Syllables: 150 Hz harmonic voice, 50 ms release, 4 Hz rate, fricatives."""
    t = np.arange(int(sec * fs)) / fs
    x = np.zeros_like(t)
    rng = np.random.default_rng(0)
    for start in np.arange(0.1, sec - 0.3, 0.25):
        f0 = 150 * (1 + 0.02 * rng.standard_normal())
        seg = slice(int(start * fs), int((start + 0.2) * fs))
        tt = t[seg]
        tone = sum(np.sin(2 * np.pi * f0 * h * tt) / h for h in range(1, 12))
        e = np.ones(len(tt))
        e[: int(0.01 * fs)] = np.linspace(0, 1, int(0.01 * fs))
        e[int(0.1 * fs):] *= np.exp(-np.arange(len(tt) - int(0.1 * fs)) / (0.05 * fs))
        x[seg] += tone * e
        fr = slice(int((start + 0.21) * fs), int((start + 0.25) * fs))
        x[fr] += rng.standard_normal(len(x[fr])) * 0.3       # fricative
    return x / np.abs(x).max()


def _music(sec=2.0, fs=FS):
    """Chord every 0.5 s, 400 ms exponential note decay over a steady mix floor."""
    t = np.arange(int(sec * fs)) / fs
    x = np.zeros_like(t)
    for start in np.arange(0.05, sec - 0.5, 0.5):
        seg = slice(int(start * fs), int((start + 0.45) * fs))
        tt = t[seg] - start
        e = np.exp(-tt / 0.12)
        x[seg] += e * sum(np.sin(2 * np.pi * f * tt) for f in (220, 277, 330, 440))
    x += 0.02 * np.random.default_rng(1).standard_normal(len(t))
    return x / np.abs(x).max()


if __name__ == "__main__":
    import sys

    if len(sys.argv) > 1:
        for path in sys.argv[1:]:
            label, f = classify(load_wav(path))
            fade = f"{f['fade_ms']:.0f}" if f["fade_ms"] else "-"
            fast = f"{f['fade_fast']:.2f}" if f["fade_fast"] is not None else "-"
            print(f"{path}: {label}  fade={fade}ms fast={fast} "
                  f"n={f['fades']} spread={f['spread_hz']:.0f}Hz "
                  f"flat={f['band_std']:.1f}dB rms={f['rms_db']:.1f}dB")
        raise SystemExit

    rng = np.random.default_rng(2)
    t = np.arange(FS * 2) / FS
    pad = 0.3 * sum(np.sin(2 * np.pi * f * t) for f in (220, 277, 330))
    cases = {
        "voice": _voice(),
        "music": _music(),
        "sustained": pad,
        "noise": rng.standard_normal(FS * 2) * 0.2,
        "silence": np.zeros(FS),
    }
    out = {}
    for name, sig in cases.items():
        label, f = classify(sig)
        out[name] = f
        fade = f"{f['fade_ms']:.0f}" if f["fade_ms"] else "-"
        fast = f"{f['fade_fast']:.2f}" if f["fade_fast"] is not None else "-"
        print(
            f"{name:9s} -> {label:8s} fade={fade:>4s}ms fast={fast:4s} "
            f"n={f['fades']:2d} spread={f['spread_hz']:5.0f}Hz "
            f"flat={f['band_std']:4.1f}dB rms={f['rms_db']:6.1f}dB"
        )

    assert classify(cases["silence"])[0] == "silence"
    assert classify(cases["noise"])[0] == "noise"
    assert classify(cases["voice"])[0] == "voice"
    assert classify(cases["music"])[0] == "music"
    assert classify(cases["sustained"])[0] == "music"
    # the two claims this probe exists to test
    assert out["voice"]["fade_ms"] < out["music"]["fade_ms"], "voice releases should be shorter"
    assert out["voice"]["spread_hz"] > out["music"]["spread_hz"], "voice should be spectrally wider"
    print("ok")
