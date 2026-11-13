"""voice / music / noise from two time-domain ideas: fading and bands.

No FFT, no scipy. 16 kHz mono. Everything is biquads + envelope followers,
so it ports to C++ as ~300 lines with zero dependencies.

    fading survives  -> not noise          (noise has no attack/decay stage)
    unison fading    -> voice vs music     (n harmonics share one short fade)
"""

import numpy as np

try:
    import mod
except ImportError:
    mod = None

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


def _movmean(x, k, axis=-1):
    """Moving average of width k along axis, edge-padded at the start."""
    x = np.moveaxis(x, axis, -1)
    k = max(1, min(int(k), x.shape[-1]))
    c = np.cumsum(x, -1, dtype=float)
    c = np.concatenate([np.zeros(c.shape[:-1] + (1,)), c], -1)
    s = (c[..., k:] - c[..., :-k]) / k
    out = np.empty(x.shape[:-1] + (x.shape[-1],), dtype=float)
    out[..., :k - 1] = s[..., :1]
    out[..., k - 1:] = s
    return np.moveaxis(out, -1, axis)


def coherence(env, fs=FS, hp_s=1.0):
    """WIDE modulation: how much the band envelopes vary *together*.

    Not a clock.  Real speech has no detectable periodicity in one window
    (verified: 20/50/100/200 ms detrend, per band and on the shared
    component, all zero hits), but it does have the property the core idea
    always claimed: n frequencies carrying ONE modulation.  So measure the
    common variation directly -- high-pass each envelope (1 s, keeps >=1 Hz),
    z-normalize, median pairwise correlation.

        voice / one source     0.85-0.99    dense mix / hiss   0.26-0.56
    """
    k = hp_s * fs
    d = env - _movmean(env, k, axis=-1)
    d = d / (d.std(axis=-1, keepdims=True) + 1e-12)
    c = d @ d.T / d.shape[-1]
    iu = np.triu_indices(len(c), 1)
    return float(np.median(c[iu]))


def features(x, fs=FS):
    x = np.asarray(x, dtype=float)
    rms_db = 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)
    if rms_db < -50:
        return {"rms_db": rms_db, "band_std": 0.0, "spread_hz": 0.0,
                "coh": 0.0, "fade_ms": None, "fade_fast": None, "fades": 0,
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
        "coh": coherence(env, fs),
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
    # WIDE modulation first: voice is n freqs carrying ONE modulation, and
    # that needs no clock.  coh >= 0.7 means the bands share their variation
    # (voice 0.85-0.99, dense mix 0.33-0.56, hiss 0.26-0.43).
    if f["coh"] >= 0.7:
        # shared modulation: is it one source fading in unison (voice) or a
        # chord / one instrument where each note decays on its own?
        if f["short_fades"] > f["fades"] * 0.33:
            return "voice", f
        return "music", f
    # No shared modulation -> cannot be voice, whatever the fades say.
    # (This branch used to call dense pop "voice": its short per-band ripples
    # passed the unison test.  A mix whose bands don't vary together isn't
    # one source, so the fade ratio is meaningless there.)
    if f["fades"] == 0:
        # no fades of >= 20 dB.  A dense mix's decays are often shallow,
        # so the beat clock is the vote that separates "stationary music"
        # from "stationary hiss".
        if mod is not None:
            for c in CENTERS:
                freq, conf = mod.beat(_env(_filt(x, *_bp(c), n=512)))
                if conf > 0.3 and freq > 0:
                    return "music", f
        return ("noise" if f["band_std"] < 4.5 else "music"), f
    return "music", f


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
            print(f"{path}: {label}  coh={f['coh']:.2f} fade={fade}ms "
                  f"fast={fast} n={f['fades']} spread={f['spread_hz']:.0f}Hz "
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
            f"{name:9s} -> {label:8s} coh={f['coh']:.2f} "
            f"fade={fade:>4s}ms fast={fast:4s} "
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
    # wide modulation: the bands of a voice vary together, noise doesn't
    assert out["voice"]["coh"] >= 0.7, "voice should share one modulation"
    assert out["noise"]["coh"] < 0.7, "independent bands shouldn't correlate"
    print("ok")
