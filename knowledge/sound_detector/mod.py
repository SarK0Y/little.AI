"""Extract the envelope's clock per band.

A band's envelope rides on one clock:
    voice -> 3-8 Hz syllables    music -> 0.5-2.5 Hz beat    noise -> none
The clock is the strongest autocorrelation peak of a detrended envelope
over a chosen lag range. One number, no FFT, no spectrum to read.

The lag range is a parameter:
    (0.5, 16)   -> beat / long-period music
    (0.05, 0.5) -> syllable clock (3-20 Hz)
"""

import numpy as np

FS = 16000
CENTERS = (100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300)
DEC = 8    # decimate to 200 Hz


def mod_freq(env, fs=FS, lag_s=(0.5, 16.0), detrend_ms=200.0, thresh=0.2):
    """Strongest clock of one band envelope, in Hz (0.0 if none).

    Decimate, detrend with a running-max subtraction (removes the slow
    envelope, keeps the ride), then autocorrelate.  The highest peak in
    correlation over the lag range gives the clock period; its inverse
    is the frequency.

    A clock estimate is only trustworthy with >= ~1 s of signal: shorter
    windows return 0.0 because the autocorrelation has too little data.
    The lag range is the caller's:
        (0.05, 0.35)  syllable clock, 3-20 Hz
        (0.4, 3.0)    beat clock, 0.33-2.5 Hz
    """
    e = env[::DEC]
    fs_e = fs / DEC
    if len(e) < fs_e:   # need at least 1 s of decimated signal
        return 0.0
    k = max(1, int(detrend_ms * 1e-3 * fs_e))
    sw = np.lib.stride_tricks.sliding_window_view(e, k)
    det = e[: sw.shape[0]] - sw.max(1)
    det = det - det.mean()
    n = len(det)
    lo, hi = int(lag_s[0] * fs_e), int(lag_s[1] * fs_e)
    hi = min(hi, n - 1)
    if lo >= hi:
        return 0.0
    norm = np.sqrt((det ** 2).sum())
    if norm < 1e-9:
        return 0.0
    lags = np.arange(lo, hi + 1)
    ac = np.empty(len(lags))
    for i, L in enumerate(lags):
        ac[i] = float((det[:n - L] * det[L:]).sum()) / (norm * norm)
    i = int(np.argmax(ac))
    if ac[i] < thresh:
        return 0.0
    # White noise produces monotonic autocorrelation decay: the "peak"
    # is just the first lag in the range.  A real clock makes a spike
    # that stands above the surrounding lags.  A broad plateau (median
    # comparable to the peak) is also rejected.
    if i == 0:
        return 0.0
    if np.median(ac) > 0 and ac[i] < 2.0 * np.median(ac):
        return 0.0
    return fs_e / lags[i]


def syllables(env, fs=FS):
    """Syllable clock, 3-20 Hz. Returns (freq, confidence)."""
    return _mod_band(env, fs, lag_s=(0.05, 0.35), thresh=0.2)


def beat(env, fs=FS):
    """Beat clock, 0.3-2.5 Hz. Returns (freq, confidence)."""
    return _mod_band(env, fs, lag_s=(0.4, 3.0), thresh=0.2)


def _mod_band(env, fs, lag_s, thresh):
    """Return (freq, confidence). confidence is the autocorrelation value
    at the detected peak, so callers can reject weak hits with conf < 0.3."""
    freq = mod_freq(env, fs=fs, lag_s=lag_s, thresh=thresh)
    if freq == 0.0:
        return 0.0, 0.0
    # Recompute the autocorrelation at the target lag with the same
    # 100 ms detrend as mod_freq uses, so the confidence matches.
    e = env[::DEC]
    fs_e = fs / DEC
    n = len(e)
    k = max(1, int(0.1 * fs_e))
    sw = np.lib.stride_tricks.sliding_window_view(e, k)
    det = e[: sw.shape[0]] - sw.max(1)
    det = det - det.mean()
    nn = len(det)
    target = int(fs_e / freq)
    lo, hi = int(lag_s[0] * fs_e), min(int(lag_s[1] * fs_e), nn - 1)
    if target < lo or target > hi or nn - target <= 0:
        return freq, 0.0
    norm = np.sqrt((det ** 2).sum())
    if norm < 1e-9:
        return freq, 0.0
    m = nn - target
    conf = float((det[: m] * det[target: target + m]).sum()) / (norm * norm)
    return freq, conf


if __name__ == "__main__":
    import sys
    import bands

    if len(sys.argv) > 1:
        for path in sys.argv[1:]:
            x = bands.load_wav(path)
            syls, beats = [], []
            for c in CENTERS:
                env = bands._env(bands._filt(x, *bands._bp(c), n=512))
                f, conf = syllables(env)
                syls.append((f, conf))
                f, conf = beat(env)
                beats.append((f, conf))
            label = bands.classify(x)[0]
            print(f"{path.split('/')[-1]} [{label}]")
            print("  syllable Hz:", " ".join(f"{f:5.2f}" for f, c in syls))
            print("  syll conf :", " ".join(f"{c:5.2f}" for f, c in syls))
            print("  beat     Hz:", " ".join(f"{f:5.2f}" for f, c in beats))
            print("  beat  conf :", " ".join(f"{c:5.2f}" for f, c in beats))
        raise SystemExit

    rng = np.random.default_rng(0)

    def voice():
        t = np.arange(FS * 2) / FS
        x = np.zeros_like(t)
        for start in np.arange(0.1, 1.8, 0.25):
            seg = slice(int(start * FS), int((start + 0.2) * FS))
            tt = t[seg]
            tone = sum(np.sin(2 * np.pi * 150 * h * tt) / h for h in range(1, 12))
            e = np.exp(-np.arange(len(tt)) / (0.05 * FS))
            e[: int(0.01 * FS)] = np.linspace(0, 1, int(0.01 * FS))
            x[seg] += tone * e
        x *= (0.5 + 0.5 * np.sin(2 * np.pi * 4 * t))
        return x / np.abs(x).max()

    def music():
        t = np.arange(FS * 2) / FS
        x = np.zeros_like(t)
        for start in np.arange(0.05, 1.5, 0.5):
            seg = slice(int(start * FS), int((start + 0.45) * FS))
            tt = t[seg] - start
            e = np.exp(-tt / 0.12)
            x[seg] += e * sum(np.sin(2 * np.pi * f * tt)
                              for f in (220, 277, 330, 440))
        x *= (0.5 + 0.5 * np.sin(2 * np.pi * 1 * t))
        return x / np.abs(x).max()

    for name, sig in (("voice @4 Hz", voice()),
                      ("music @1 Hz", music()),
                      ("white noise", rng.standard_normal(FS * 2) * 0.2)):
        syls = [syllables(bands._env(bands._filt(sig, *bands._bp(c), n=512)))
                for c in CENTERS]
        bts = [beat(bands._env(bands._filt(sig, *bands._bp(c), n=512)))
               for c in CENTERS]
        print(f"{name:12s} syl:", " ".join(f"{f:5.2f}" for f, c in syls))
        print(" " * 12 + "  c:", " ".join(f"{c:5.2f}" for f, c in syls))
        print(" " * 12 + " bt :", " ".join(f"{f:5.2f}" for f, c in bts))
        print(" " * 12 + "  c:", " ".join(f"{c:5.2f}" for f, c in bts))
