/* sound_detector C++ test
 *
 * No FFT.  10 biquad bandpass filters + one-pole envelope followers.
 *
 * WIDE modulation, two forms:
 *   coherence  -- do the band envelopes vary TOGETHER?  (voice = n freqs
 *                 carrying one modulation; needs no clock, which matters
 *                 because real speech has no detectable periodicity)
 *   beat clock -- residual periodicity, only asked for when the bands do
 *                 NOT agree (stationary music vs stationary hiss)
 *
 * C-style: no smart pointers, no exceptions, no RTTI.
 * One allowed: std::variant for the Result type (Rust-style errors).
 * No dynamic allocation beyond per-window scratch buffers.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <variant>

/* ------------------------------------------------------------------ */
/* Rust-style Result<T, Error> — one variant, no exceptions            */
/* ------------------------------------------------------------------ */

struct Error {
    int line;
    const char *msg;
};

using Result = std::variant<std::monostate, Error>;

static Error make_err(int line, const char *what) {
    return Error{line, what};
}

#define CHECK(cond, what)                                               \
    do {                                                                \
        if (!(cond)) {                                                  \
            Error e{__LINE__, what};                                    \
            std::fprintf(stderr, "line %d: %s\n", e.line, e.msg);       \
            return e;                                                   \
        }                                                               \
    } while (0)

#define ERR(what) make_err(__LINE__, what)

/* ------------------------------------------------------------------ */
/* Constants (match bands.py / mod.py)                                 */
/* ------------------------------------------------------------------ */

static constexpr int FS = 16000;
static constexpr int NUM_BANDS = 10;
static constexpr float CENTERS[NUM_BANDS] = {
    100.0f, 160.0f, 250.0f, 400.0f,
    630.0f, 1000.0f, 1600.0f, 2500.0f,
    4000.0f, 6300.0f
};
static constexpr float Q = 1.2f;

static constexpr int DEC = 8;              /* 16 kHz -> 2 kHz envelope */
static constexpr float ENV_REL_MS = 8.0f;  /* bands._env release */
static constexpr float DETREND_MS = 200.0f;
/* mod.py's two-stage test: the search peak must clear mod_freq's own
 * thresh (0.2), then _mod_band re-measures confidence on a 100 ms detrend
 * and callers reject conf < 0.3 */
static constexpr float MOD_PEAK = 0.2f;
static constexpr float MOD_CONF = 0.3f;

/* scratch for mod_freq: 5 s window -> 10000 decimated samples,
 * beat lag up to 3 s -> 6000 lags */
static constexpr int MAXDEC = 5 * FS / DEC + 64;
static constexpr int MAXLAG = (int)(3.0f * FS / DEC) + 64;

static float g_dec[MAXDEC];    /* decimated envelope */
static float g_det[MAXDEC];    /* detrended */
static float g_ac[MAXLAG];     /* autocorrelation over lag range */
static float g_ac2[MAXLAG];    /* copy for the median check */

/* ------------------------------------------------------------------ */
/* Biquad bandpass (RBJ), computed at init, run per sample            */
/* ------------------------------------------------------------------ */

struct Biquad {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2, y1, y2;   /* state */
};

static Biquad make_bp(float f0) {
    Biquad b;
    const float w0 = 2.0f * (float)M_PI * f0 / (float)FS;
    const float sin_w0 = sinf(w0);
    const float cos_w0 = cosf(w0);
    const float alpha = sin_w0 / (2.0f * Q);
    const float norm = 1.0f + alpha;
    b.b0 = alpha / norm;
    b.b1 = 0.0f;
    b.b2 = -alpha / norm;
    b.a1 = -2.0f * cos_w0 / norm;
    b.a2 = (1.0f - alpha) / norm;
    b.x1 = b.x2 = b.y1 = b.y2 = 0.0f;
    return b;
}

static inline float biquad_step(Biquad &b, float x) {
    const float y = b.b0 * x + b.b1 * b.x1 + b.b2 * b.x2
                  - b.a1 * b.y1 - b.a2 * b.y2;
    b.x2 = b.x1; b.x1 = x;
    b.y2 = b.y1; b.y1 = y;
    return y;
}

static Result run_bp(const float *in, float *out, int n, Biquad &b) {
    for (int i = 0; i < n; i++)
        out[i] = biquad_step(b, in[i]);
    return {};
}

/* ------------------------------------------------------------------ */
/* Envelope: rectify + one-pole lowpass (8 ms release, bands._env)    */
/* ------------------------------------------------------------------ */

static inline void env_follow(float *env, int n, float a, const float *x) {
    const float b = 1.0f - a;
    float y = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = x[i] < 0.0f ? -x[i] : x[i];
        y = a * v + b * y;
        env[i] = y;
    }
}

/* ------------------------------------------------------------------ */
/* Modulation clock (ports mod.mod_freq)                               */
/* ------------------------------------------------------------------ */

static int cmp_float(const void *a, const void *b) {
    const float fa = *(const float *)a;
    const float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

/* Strongest clock of one band envelope, in Hz (0.0 if none).
 * lag_lo_s / lag_hi_s: search range in seconds of the period.
 *   beat: (0.4, 3.0)   -- the syllable search is gone: real speech has no
 *   periodic clock to find (see notes.md), so only the beat vote remains. */
static float mod_freq(const float *env, int n,
                      float lag_lo_s, float lag_hi_s) {
    const float fs_e = (float)FS / (float)DEC;
    int m = n / DEC;
    if (m > MAXDEC) m = MAXDEC;
    if (m < (int)fs_e) return 0.0f;             /* need >= 1 s */

    for (int i = 0; i < m; i++)
        g_dec[i] = env[i * DEC];

    /* detrend: subtract the max of the next k samples (200 ms window) */
    int k = (int)(DETREND_MS * 1e-3f * fs_e);
    if (k < 1) k = 1;
    if (k > m) k = m;
    const int nd = m - k + 1;
    if (nd < (int)fs_e) return 0.0f;
    for (int i = 0; i < nd; i++) {
        float mx = g_dec[i];
        for (int j = i + 1; j < i + k; j++)
            if (g_dec[j] > mx) mx = g_dec[j];
        g_det[i] = g_dec[i] - mx;
    }

    float mean = 0.0f;
    for (int i = 0; i < nd; i++) mean += g_det[i];
    mean /= (float)nd;
    float norm2 = 0.0f;
    for (int i = 0; i < nd; i++) {
        g_det[i] -= mean;
        norm2 += g_det[i] * g_det[i];
    }
    if (norm2 < 1e-12f) return 0.0f;

    int lo = (int)(lag_lo_s * fs_e);
    int hi = (int)(lag_hi_s * fs_e);
    if (hi > nd - 1) hi = nd - 1;
    if (lo < 1) lo = 1;
    if (lo >= hi) return 0.0f;
    const int nl = hi - lo + 1;

    for (int lag = lo; lag <= hi; lag++) {
        float s = 0.0f;
        const int mm = nd - lag;
        for (int i = 0; i < mm; i++)
            s += g_det[i] * g_det[i + lag];
        g_ac[lag - lo] = s / norm2;
    }

    int bi = 0;
    for (int i = 1; i < nl; i++)
        if (g_ac[i] > g_ac[bi]) bi = i;
    if (bi == 0) return 0.0f;                  /* monotone decay = noise */
    if (g_ac[bi] < MOD_PEAK) return 0.0f;

    /* a real clock spikes above the surrounding lags; a broad plateau
     * (median comparable to the peak) is rejected */
    memcpy(g_ac2, g_ac, (size_t)nl * sizeof(float));
    qsort(g_ac2, (size_t)nl, sizeof(float), cmp_float);
    const float med = g_ac2[nl / 2];
    if (med > 0.0f && g_ac[bi] < 2.0f * med) return 0.0f;

    /* confidence, measured the way mod._mod_band measures it: re-detrend
     * with a 100 ms window (the search used 200 ms) and read the peak lag.
     * That window keeps fast ripple, so this value is systematically lower
     * -- it is what makes white noise's false peaks fail. */
    const int k2 = (int)(0.1f * fs_e);
    const int target = lo + bi;
    const int nd2 = m - k2 + 1;
    if (k2 < 1 || nd2 <= target) return 0.0f;
    for (int i = 0; i < nd2; i++) {
        float mx = g_dec[i];
        for (int j = i + 1; j < i + k2; j++)
            if (g_dec[j] > mx) mx = g_dec[j];
        g_det[i] = g_dec[i] - mx;
    }
    mean = 0.0f;
    for (int i = 0; i < nd2; i++) mean += g_det[i];
    mean /= (float)nd2;
    norm2 = 0.0f;
    for (int i = 0; i < nd2; i++) {
        g_det[i] -= mean;
        norm2 += g_det[i] * g_det[i];
    }
    if (norm2 < 1e-12f) return 0.0f;
    float conf = 0.0f;
    for (int i = 0; i < nd2 - target; i++)
        conf += g_det[i] * g_det[i + target];
    conf /= norm2;
    if (conf < MOD_CONF) return 0.0f;

    return fs_e / (float)target;
}

/* ------------------------------------------------------------------ */
/* WAV I/O (PCM 16-bit, mono)                                          */
/* ------------------------------------------------------------------ */

struct Wave {
    int n;
    float *data;    /* caller must free */
};

static Result read_wav(const char *path, Wave &w) {
    FILE *f = fopen(path, "rb");
    if (!f) return ERR("cannot open file");

    unsigned char hdr[44];
    CHECK(fread(hdr, 1, 44, f) == 44, "short header");

    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return ERR("not a WAV");

    unsigned short channels, bits;
    unsigned int sample_rate;
    memcpy(&channels,    hdr + 22, 2);
    memcpy(&sample_rate, hdr + 24, 4);
    memcpy(&bits,        hdr + 34, 2);

    CHECK(channels == 1, "need mono");
    CHECK(bits == 16, "need 16-bit PCM");
    CHECK(sample_rate == (unsigned)FS, "need 16000 Hz");

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    int n = (int)(sz - 44) / 2;
    if (n <= 0) { fclose(f); return ERR("no audio data"); }

    w.data = (float *)malloc(sizeof(float) * n);
    if (!w.data) { fclose(f); return ERR("out of memory"); }

    short *raw = (short *)malloc(sizeof(short) * n);
    if (!raw) { free(w.data); fclose(f); return ERR("out of memory"); }
    CHECK(fread(raw, 2, n, f) == (size_t)n, "read failed");

    for (int i = 0; i < n; i++)
        w.data[i] = (float)raw[i] * (1.0f / 32768.0f);

    free(raw);
    fclose(f);
    w.n = n;
    return {};
}

/* ------------------------------------------------------------------ */
/* features + vote                                                     */
/* ------------------------------------------------------------------ */

struct FeatureSet {
    float rms_db;
    float band_std;
    float spread_hz;
    float coh;            /* wide modulation: bands varying together */
    int   fades;          /* 20 dB descents on the summed envelope */
    int   short_fades;    /* per-band descents < 150 ms */
    float beat_hz[NUM_BANDS];
};

static constexpr int FADE_MAX = 5 * FS / 16 + 64;   /* 5 s window at 1 kHz */
static float g_fd1[FADE_MAX], g_fd2[FADE_MAX];

/* Count 20 dB descents in one envelope (ports bands._fades).  The envelope
 * is max-pooled to 1 kHz, so an index difference IS milliseconds.
 * *n_short accumulates descents under 150 ms; pass null to skip. */
static int fades_in(const float *env, int n, int *n_short) {
    const int m = n / 16;
    if (m < 32 || m > FADE_MAX) return 0;

    float peak = 0.0f;
    for (int i = 0; i < m; i++) {
        float v = env[i * 16];
        for (int j = 1; j < 16; j++) {
            const float w = env[i * 16 + j];
            if (w > v) v = w;
        }
        g_fd1[i] = v;
        if (v > peak) peak = v;
    }
    if (peak < 1e-6f) return 0;
    for (int i = 0; i < m; i++) g_fd1[i] /= peak;

    /* 10 ms boxcar kills the rectifier ripple; window [i-5, i+4] matches
     * np.convolve(..., mode="same") for an even kernel */
    for (int i = 0; i < m; i++) {
        float acc = 0.0f;
        for (int j = i - 5; j <= i + 4; j++)
            if (j >= 0 && j < m) acc += g_fd1[j];
        g_fd2[i] = acc / 10.0f;
    }

    const int w = 20;                     /* 40 ms neighbourhood at 1 kHz */
    int count = 0;
    for (int i = 0; i < m; i++) {
        if (g_fd2[i] <= 0.1f) continue;   /* a descent starts loud */
        int lo = i - w, hi = i + w;
        if (lo < 0) lo = 0;
        if (hi > m - 1) hi = m - 1;
        bool is_max = true;
        for (int j = lo; j <= hi; j++)
            if (g_fd2[j] > g_fd2[i]) { is_max = false; break; }
        if (!is_max) continue;

        float pk = g_fd2[i], tr = g_fd2[i];
        int j = i;
        while (j < m && j - i <= 2000) {   /* max_ms = 2000 */
            if (g_fd2[j] < tr) tr = g_fd2[j];
            if (g_fd2[j] > tr * 1.41253f) break;        /* rebound 3 dB */
            const float drop = 20.0f * log10f(pk / (tr > 1e-9f ? tr : 1e-9f));
            if (drop >= 20.0f) {
                const int dur = j - i;
                if (dur >= 20) {
                    count++;
                    if (n_short && dur < 150) (*n_short)++;
                }
                break;
            }
            j++;
        }
    }
    return count;
}

/* WIDE modulation: median pairwise correlation of 1 s high-passed,
 * z-normalized band envelopes (ports bands.coherence).  No clock anywhere.
 * scratch must hold NUM_BANDS * n floats; envs is left untouched. */
static float coherence(const float *envs, int n, float *scratch) {
    int k = FS;                       /* 1 s */
    if (k > n) k = n;                 /* short file: detrend by its own mean */

    for (int b = 0; b < NUM_BANDS; b++) {
        const float *e = envs + (size_t)b * n;
        float *d = scratch + (size_t)b * n;
        float win = 0.0f;
        for (int i = 0; i < k; i++) win += e[i];
        const float first = win / (float)k;
        for (int i = 0; i < n; i++) {
            /* the first k-1 samples take the first full window (matches
             * bands._movmean), then the window slides */
            d[i] = e[i] - ((i < k - 1) ? first : win / (float)k);
            if (i >= k - 1 && i + 1 < n)
                win += e[i + 1] - e[i + 1 - k];
        }
        double ss = 0.0;
        for (int i = 0; i < n; i++) ss += (double)d[i] * d[i];
        const float sd = (float)sqrt(ss / (double)n) + 1e-12f;
        for (int i = 0; i < n; i++) d[i] /= sd;
    }

    float pairs[NUM_BANDS * (NUM_BANDS - 1) / 2];
    int np = 0;
    for (int a = 0; a < NUM_BANDS; a++)
        for (int b = a + 1; b < NUM_BANDS; b++) {
            const float *da = scratch + (size_t)a * n;
            const float *db = scratch + (size_t)b * n;
            double s = 0.0;
            for (int i = 0; i < n; i++) s += (double)da[i] * db[i];
            pairs[np++] = (float)(s / (double)n);
        }
    qsort(pairs, (size_t)np, sizeof(float), cmp_float);
    return pairs[np / 2];
}

static Result features(const float *x, int n, FeatureSet &f) {
    double acc = 0.0;
    for (int i = 0; i < n; i++) acc += (double)x[i] * x[i];
    f.rms_db = 20.0f * log10f((float)sqrt(acc / (double)n) + 1e-12f);
    f.band_std = f.spread_hz = f.coh = 0.0f;
    f.fades = f.short_fades = 0;
    memset(f.beat_hz, 0, sizeof f.beat_hz);
    if (f.rms_db < -50.0f)
        return {};                    /* silence: nothing to measure */

    float *bp_out = (float *)malloc(sizeof(float) * n);
    float *envs   = (float *)malloc(sizeof(float) * n * NUM_BANDS);
    if (!bp_out || !envs) {
        free(bp_out); free(envs);
        return ERR("out of memory");
    }

    Biquad bq[NUM_BANDS];
    for (int i = 0; i < NUM_BANDS; i++) bq[i] = make_bp(CENTERS[i]);

    const float a = 1.0f - expf(-1.0f / (FS * ENV_REL_MS / 1000.0f));

    for (int i = 0; i < NUM_BANDS; i++) {
        Result r = run_bp(x, bp_out, n, bq[i]);
        if (std::holds_alternative<Error>(r)) {
            free(bp_out); free(envs);
            return r;
        }
        float *env = envs + (size_t)i * n;
        env_follow(env, n, a, bp_out);
    }

    /* band energy + flatness (dB deviation from the loudest band) */
    float energy[NUM_BANDS];
    float max_energy = 0.0f;
    for (int i = 0; i < NUM_BANDS; i++) {
        float *env = envs + (size_t)i * n;
        float e = 0.0f;
        for (int j = 0; j < n; j++) e += env[j] * env[j];
        e /= (float)n;
        energy[i] = e;
        if (e > max_energy) max_energy = e;
    }
    float log_max = 10.0f * log10f(max_energy + 1e-12f);
    /* std of the dB profile about ITS MEAN (np.std, not the RMS about the
     * loudest band -- those differ by the mean offset, 4.1 vs 6.5 here) */
    float sum_dev = 0.0f, sum2_dev = 0.0f;
    for (int i = 0; i < NUM_BANDS; i++) {
        float db = 10.0f * log10f(energy[i] + 1e-12f);
        float dev = db - log_max;
        sum_dev += dev;
        sum2_dev += dev * dev;
    }
    const float mean_dev = sum_dev / NUM_BANDS;
    f.band_std = sqrtf(sum2_dev / NUM_BANDS - mean_dev * mean_dev);

    /* spectral spread (Hz), normalized by the total energy as in bands.py */
    float tot_energy = 0.0f;
    for (int i = 0; i < NUM_BANDS; i++) tot_energy += energy[i];
    float cent = 0.0f, spread = 0.0f;
    for (int i = 0; i < NUM_BANDS; i++)
        cent += CENTERS[i] * energy[i];
    cent /= (tot_energy + 1e-12f);
    for (int i = 0; i < NUM_BANDS; i++) {
        float d = CENTERS[i] - cent;
        spread += d * d * energy[i];
    }
    f.spread_hz = sqrtf(spread / (tot_energy + 1e-12f));

    /* fades: total on the summed envelope, shorts counted per band */
    float *sum = (float *)malloc(sizeof(float) * n);
    float *scratch = (float *)malloc(sizeof(float) * n * NUM_BANDS);
    if (!sum || !scratch) {
        free(sum); free(scratch); free(bp_out); free(envs);
        return ERR("out of memory");
    }
    for (int j = 0; j < n; j++) {
        float s = 0.0f;
        for (int i = 0; i < NUM_BANDS; i++) s += envs[(size_t)i * n + j];
        sum[j] = s;
    }
    f.fades = fades_in(sum, n, nullptr);
    for (int i = 0; i < NUM_BANDS; i++)
        fades_in(envs + (size_t)i * n, n, &f.short_fades);

    f.coh = coherence(envs, n, scratch);
    free(sum);
    free(scratch);

    /* the tree only asks for the beat clock in one branch */
    if (f.coh < 0.7f && f.fades == 0) {
        for (int i = 0; i < NUM_BANDS; i++)
            f.beat_hz[i] = mod_freq(envs + (size_t)i * n, n,
                                    0.40f, 3.00f);
    }

    free(bp_out);
    free(envs);
    return {};
}

/* ------------------------------------------------------------------ */
/* vote + self-check                                                    */
/* ------------------------------------------------------------------ */

/* Wide-modulation tree (mirrors bands.classify).
 * A voice is n freqs carrying ONE modulation; that no longer needs a clock,
 * just agreement between the bands. */
static const char *vote(const FeatureSet &f) {
    if (f.rms_db < -50.0f) return "silence";
    if (f.coh >= 0.7f) {
        /* shared modulation: unison releases -> one source (voice),
         * each note decaying on its own -> music (chords, one instrument) */
        return (f.short_fades > f.fades * 0.33f) ? "voice" : "music";
    }
    /* No shared modulation -> cannot be voice, whatever the fades say.
     * (The old tree called dense pop "voice" here: its per-band ripples
     * passed the unison test.  Bands that don't vary together aren't one
     * source, so the fade ratio means nothing in this branch.) */
    if (f.fades == 0) {
        for (int i = 0; i < NUM_BANDS; i++)
            if (f.beat_hz[i] > 0.0f) return "music";   /* pulse, no edges */
        return (f.band_std < 4.5f) ? "noise" : "music";
    }
    return "music";
}

/* approx gaussian, sum of 12 uniforms - 6 (LCG) */
static float frand(unsigned int *seed) {
    float v = 0.0f;
    for (int j = 0; j < 12; j++) {
        *seed = *seed * 1664525u + 1013904223u;
        v += (float)(*seed >> 8) / 16777216.0f;
    }
    return v - 6.0f;
}

/* bands._voice: jittered f0, 4 Hz rate, 10 ms ramp, 50 ms release, fricatives */
static float *synth_voice() {
    static float x[2 * FS];
    unsigned int seed = 7u;
    for (int k = 0; 0.1f + 0.25f * k < 2.0f - 0.3f; k++) {
        const int st = (int)((0.1f + 0.25f * k) * FS);
        const float f0 = 150.0f * (1.0f + 0.02f * frand(&seed));
        const int len = (int)(0.2f * FS);
        for (int i = 0; i < len; i++) {
            const float tt = (float)i / FS;
            float tone = 0.0f;
            for (int h = 1; h <= 11; h++)
                tone += sinf(2.0f * (float)M_PI * f0 * h * tt) / (float)h;
            float e = 1.0f;
            if (i < (int)(0.01f * FS))
                e = (float)i / (0.01f * FS);
            if (i >= (int)(0.1f * FS))
                e *= expf(-(float)(i - (int)(0.1f * FS)) / (0.05f * FS));
            x[st + i] += tone * e;
        }
        const int fr = st + (int)(0.21f * FS);
        const int fl = (int)(0.04f * FS);
        for (int i = 0; i < fl && fr + i < 2 * FS; i++)
            x[fr + i] += frand(&seed) * 0.3f;
    }
    float mx = 0.0f;
    for (int i = 0; i < 2 * FS; i++) { const float v = fabsf(x[i]); if (v > mx) mx = v; }
    if (mx > 0) for (int i = 0; i < 2 * FS; i++) x[i] /= mx;
    return x;
}

/* bands._music: chord every 500 ms, 120 ms exponential decay, steady floor */
static float *synth_music() {
    static float x[2 * FS];
    const float notes[4] = {220.0f, 277.0f, 330.0f, 440.0f};
    for (int k = 0; 0.05f + 0.5f * k < 1.5f; k++) {
        const int st = (int)((0.05f + 0.5f * k) * FS);
        const int len = (int)(0.45f * FS);
        if (st + len > 2 * FS) break;
        for (int i = 0; i < len; i++) {
            const float tt = (float)i / FS;
            float v = 0.0f;
            for (int j = 0; j < 4; j++)
                v += sinf(2.0f * (float)M_PI * notes[j] * tt);
            x[st + i] += expf(-tt / 0.12f) * v;
        }
    }
    unsigned int seed = 99u;
    float mx = 0.0f;
    for (int i = 0; i < 2 * FS; i++) {
        x[i] += 0.02f * frand(&seed);
        const float v = fabsf(x[i]); if (v > mx) mx = v;
    }
    if (mx > 0) for (int i = 0; i < 2 * FS; i++) x[i] /= mx;
    return x;
}

static float *synth_pad() {
    static float x[2 * FS];
    const float notes[3] = {220.0f, 277.0f, 330.0f};
    for (int i = 0; i < 2 * FS; i++) {
        const float t = (float)i / FS;
        float v = 0.0f;
        for (int j = 0; j < 3; j++)
            v += sinf(2.0f * (float)M_PI * notes[j] * t);
        x[i] = 0.3f * v;
    }
    return x;
}

static float *synth_noise() {
    static float x[2 * FS];
    unsigned int seed = 12345u;
    for (int i = 0; i < 2 * FS; i++)
        x[i] = frand(&seed) * 0.2f;
    return x;
}

static float *synth_silence() {
    static float x[FS];
    memset(x, 0, sizeof x);
    return x;
}

/* the five cases bands.py asserts, plus the wide-modulation claims */
static int selftest() {
    struct Case { const char *name; float *sig; int n; const char *want;
                  bool check_coh; float coh_min; };
    const Case cases[] = {
        {"voice",      synth_voice(),  2 * FS, "voice",   true,  0.7f},
        {"music",      synth_music(),  2 * FS, "music",   false, 0.0f},
        {"sustained",  synth_pad(),    2 * FS, "music",   false, 0.0f},
        {"noise",      synth_noise(),  2 * FS, "noise",   true,  -1.0f},
        {"silence",    synth_silence(),   FS, "silence",  false, 0.0f},
    };
    FeatureSet f;
    int failed = 0;
    for (const Case &c : cases) {
        Result r = features(c.sig, c.n, f);
        if (std::holds_alternative<Error>(r)) {
            std::printf("selftest ERROR: %s\n", c.name);
            failed++;
            continue;
        }
        const char *got = vote(f);
        bool ok = strcmp(got, c.want) == 0;
        if (c.check_coh && c.coh_min >= 0.0f && f.coh < c.coh_min) ok = false;
        if (c.check_coh && c.coh_min < 0.0f && f.coh >= 0.7f) ok = false;
        if (!ok) failed++;
        std::printf(
            "%-10s -> %-7s (want %-7s)  coh=%.2f fades=%2d short=%2d "
            "flat=%.1fdB spread=%.0fHz %s\n",
            c.name, got, c.want, f.coh, f.fades, f.short_fades,
            f.band_std, f.spread_hz, ok ? "ok" : "FAIL");
    }
    std::printf("selftest: %s\n", failed == 0 ? "all passed" : "FAILED");
    return failed == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s --selftest | <file.wav>\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "--selftest") == 0)
        return selftest();

    Wave w;
    w.data = nullptr;
    Result r = read_wav(argv[1], w);
    if (std::holds_alternative<Error>(r))
        return 1;

    std::printf("reading %s: %d samples (%.1f s)\n",
                argv[1], w.n, (float)w.n / FS);

    /* 5 s windows; the tail of a short file becomes one window */
    int W = 5 * FS;
    if (w.n < W) W = w.n;
    if (W < FS) {
        std::fprintf(stderr, "audio too short (%d samples, need >= %d)\n",
                     w.n, FS);
        free(w.data);
        return 1;
    }
    const int n_windows = w.n / W;

    FeatureSet f;
    int voice_votes = 0, music_votes = 0;

    for (int wIdx = 0; wIdx < n_windows; wIdx++) {
        const float *seg = w.data + wIdx * W;
        r = features(seg, W, f);
        if (std::holds_alternative<Error>(r))
            break;

        int beat_bands = 0;
        for (int i = 0; i < NUM_BANDS; i++)
            if (f.beat_hz[i] > 0.0f) beat_bands++;

        const char *label = vote(f);
        if (strcmp(label, "voice") == 0) voice_votes++;
        else if (strcmp(label, "music") == 0) music_votes++;

        std::printf(
            "  %4d  %-7s  coh=%.2f  fades=%2d short=%2d  "
            "beat=%d bands  flat=%.1fdB  spread=%.0fHz\n",
            wIdx * W / FS, label,
            f.coh, f.fades, f.short_fades,
            beat_bands, f.band_std, f.spread_hz
        );
        std::fflush(stdout);
    }

    std::printf("\nvote: voice=%d  music=%d\n", voice_votes, music_votes);
    free(w.data);
    return 0;
}
