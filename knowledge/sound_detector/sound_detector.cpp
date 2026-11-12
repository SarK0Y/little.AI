/* sound_detector C++ test
 *
 * No FFT. 10 biquad bandpass filters + one-pole envelope followers.
 * Extracts the syllable/beat clock from each band's envelope by
 * autocorrelation of a decimated detrended envelope.
 *
 * C-style: no smart pointers, no exceptions, no RTTI.
 * One allowed: std::variant for the Result type (Rust-style errors).
 * No dynamic allocation except for the single audio buffer.
 */

#include <cstdio>
#include <cstring>
#include <cmath>
#include <array>
#include <variant>
#include <string>

/* ------------------------------------------------------------------ */
/* Rust-style Result<T, Error> — one variant, no exceptions          */
/* ------------------------------------------------------------------ */

struct Error {
    int line;
    const char *what;
};

using Result = std::variant<std::monostate, Error>;

static Error make_err(int line, const char *what) {
    return Error{line, what};
}

#define CHECK(cond, what)                                            \
    do {                                                             \
        if (!(cond)) {                                              \
            Error e{__LINE__, what};                               \
            std::fprintf(stderr, "line %d: %s\n", e.line, e.what); \
            return e;                                               \
        }                                                            \
    } while (0)

#define ERR(what) make_err(__LINE__, what)

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

static constexpr int FS = 16000;
static constexpr int NUM_BANDS = 10;
static constexpr float CENTERS[NUM_BANDS] = {
    100.0f, 160.0f, 250.0f, 400.0f,
    630.0f, 1000.0f, 1600.0f, 2500.0f,
    4000.0f, 6300.0f
};
static constexpr float Q = 1.2f;

/* decimate factor: 16 kHz -> ~1 kHz envelope sample rate */
static constexpr int DEC = 16;

/* detrend window in ms (kill the slow envelope, keep the ride) */
static constexpr int DETREND_MS = 200;

/* ------------------------------------------------------------------ */
/* Biquad bandpass (RBJ), computed at init, run per sample           */
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

/* single sample through the biquad */
static inline float biquad_step(Biquad &b, float x) {
    const float y = b.b0 * x + b.b1 * b.x1 + b.b2 * b.x2
                  - b.a1 * b.y1 - b.a2 * b.y2;
    b.x2 = b.x1; b.x1 = x;
    b.y2 = b.y1; b.y1 = y;
    return y;
}

/* run a full biquad on a buffer, in-place replace */
static Result run_bp(const float *in, float *out, int n, Biquad &b) {
    for (int i = 0; i < n; i++)
        out[i] = biquad_step(b, in[i]);
    return {};
}

/* ------------------------------------------------------------------ */
/* Envelope: rectify + one-pole lowpass (fast attack, slow release)  */
/* ------------------------------------------------------------------ */

static inline void env_follow(float *env, int n, float a, const float *x) {
    /* y[i] = a*x[i] + (1-a)*y[i-1], a from time-constant */
    float y = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = x[i] < 0.0f ? -x[i] : x[i];   /* abs, no branch */
        y = a * v + (1.0f - a) * y;
        env[i] = y;
    }
}

/* ------------------------------------------------------------------ */
/* Autocorrelation of a detrended decimated envelope                */
/* ------------------------------------------------------------------ */

/* decimate e to dec_rate, detrend with a running-max window, return
 * number of samples in the decimated signal.  In-place reuse of buf. */
static int decimate_detrend(const float *env, int n, float *buf,
                            int *det_len) {
    int m = n / DEC;
    for (int i = 0; i < m; i++)
        buf[i] = env[i * DEC];
    for (int i = 1; i < m; i++)   /* keep the max per 80 ms window */
        buf[i] = buf[i] > buf[i - 1] ? buf[i] : buf[i - 1];
    /* detrend: subtract a moving max of DETREND_MS */
    int k = DETREND_MS * FS / 1000 / DEC;   /* detrend width in dec samples */
    if (k > m) k = m;
    float prev_max = buf[0];
    for (int i = 1; i < m; i++) {
        if (buf[i] > prev_max) prev_max = buf[i];
        if (i - k >= 0) {
            /* approximate: just subtract the current moving max */
        }
        buf[i] -= prev_max;   /* crude, but O(1) */
    }
    /* subtract mean to zero the DC */
    float mean = 0.0f;
    for (int i = 0; i < m; i++) mean += buf[i];
    mean /= m;
    for (int i = 0; i < m; i++) buf[i] -= mean;
    *det_len = m;
    return m;
}

/* autocorrelation at a specific lag, returns normalised value */
static float ac_norm(const float *e, int n, int lag) {
    if (lag <= 0 || lag >= n) return 0.0f;
    float num = 0.0f, den = 0.0f;
    for (int i = 0; i < n - lag; i++) {
        num += e[i] * e[i + lag];
        den += e[i] * e[i];
    }
    return den > 1e-9f ? num / den : 0.0f;
}

/* find the dominant clock in the envelope.
 * Search lags from min_hz to max_hz, return the lag with highest
 * autocorrelation.  Returns 0.0f if nothing exceeds thresh. */
static float mod_freq(const float *env, int n, float min_hz, float max_hz,
                      float thresh) {
    float buf[2048];   /* max 32 s of audio at 1 kHz */
    int m;
    int mm = decimate_detrend(env, n, buf, &m);
    if (m < 400) return 0.0f;

    float best = 0.0f, best_lag = 0.0f;
    /* fs_e = FS / DEC */
    int min_lag = (int)(DEC / min_hz);
    int max_lag = (int)(DEC / max_hz);
    if (max_lag > m - 1) max_lag = m - 1;
    for (int lag = min_lag; lag <= max_lag; lag++) {
        float c = ac_norm(buf, m, lag);
        if (c > best) { best = c; best_lag = (float)lag; }
    }
    if (best < thresh) return 0.0f;
    /* the lag is in decimated samples; convert to Hz */
    float fs_e = (float)FS / (float)DEC;
    return best_lag > 0.0f ? fs_e / best_lag : 0.0f;
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

    /* RIFF / WAVE / fmt check */
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
    CHECK(fread(raw, 2, n, f) == (unsigned long)n, "read failed");

    for (int i = 0; i < n; i++)
        w.data[i] = (float)raw[i] * (1.0f / 32768.0f);

    free(raw);
    fclose(f);
    w.n = n;
    return {};
}

/* ------------------------------------------------------------------ */
/* classify                                                             */
/* ------------------------------------------------------------------ */

struct FeatureSet {
    float fade_ms;          /* median release, 0 = none */
    int  n_fades;
    int  short_fades;
    float band_std;
    float spread_hz;
    float syll_hz[NUM_BANDS];   /* 0 = no clock found */
    float beat_hz[NUM_BANDS];
};

static Result features(const float *x, int n, FeatureSet &f) {
    /* per-band filters */
    float *bp_out = (float *)malloc(sizeof(float) * n);
    float *envs = (float *)malloc(sizeof(float) * n * NUM_BANDS);
    if (!bp_out || !envs) {
        free(bp_out); free(envs);
        return ERR("out of memory");
    }

    Biquad bq[NUM_BANDS];
    for (int i = 0; i < NUM_BANDS; i++) bq[i] = make_bp(CENTERS[i]);

    /* run all bands, build envelopes */
    for (int i = 0; i < NUM_BANDS; i++) {
        Result r = run_bp(x, bp_out, n, bq[i]);
        if (std::holds_alternative<Error>(r)) {
            free(bp_out); free(envs);
            return r;
        }
        /* one-pole, ~10 ms release (a = 1 - exp(-1/tau)) */
        const float a = 0.05f;   /* ~10 ms at 16 kHz */
        float *env = envs + i * n;
        env_follow(env, n, a, bp_out);
    }

    /* band energy + flatness */
    float energy[NUM_BANDS];
    float max_energy = 0.0f;
    for (int i = 0; i < NUM_BANDS; i++) {
        float *env = envs + i * n;
        float e = 0.0f;
        for (int j = 0; j < n; j++) e += env[j] * env[j];
        e /= (float)n;
        energy[i] = e;
        if (e > max_energy) max_energy = e;
    }
    float log_max = 10.0f * log10f(max_energy + 1e-12f);
    float sum_db = 0.0f;
    float sum_dev = 0.0f;
    for (int i = 0; i < NUM_BANDS; i++) {
        float db = 10.0f * log10f(energy[i] + 1e-12f);
        float dev = db - log_max;
        sum_db += dev;
        sum_dev += dev * dev;
    }
    f.band_std = sqrtf(sum_dev / NUM_BANDS);

    /* spread (Hz) */
    float cent = 0.0f, spread = 0.0f;
    for (int i = 0; i < NUM_BANDS; i++) {
        cent += CENTERS[i] * energy[i];
    }
    cent /= (max_energy + 1e-12f);
    for (int i = 0; i < NUM_BANDS; i++) {
        float d = CENTERS[i] - cent;
        spread += d * d * energy[i];
    }
    spread = sqrtf(spread / (max_energy + 1e-12f));
    f.spread_hz = spread;

    /* syllable clock: 3-8 Hz range -> 250-333 ms period
     * beat clock: 0.3-2.5 Hz -> 400-3333 ms period
     * Search over 0.3-10 Hz in one call, then pick. */
    for (int i = 0; i < NUM_BANDS; i++) {
        float *env = envs + i * n;
        /* syllable: 3-8 Hz */
        f.syll_hz[i] = mod_freq(env, n, 3.0f, 8.0f, 0.25f);
        /* beat: 0.3-2.5 Hz */
        f.beat_hz[i] = mod_freq(env, n, 0.3f, 2.5f, 0.25f);
    }

    free(bp_out);
    free(envs);
    return {};
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.wav>\n", argv[0]);
        return 1;
    }

    Wave w;
    w.data = nullptr;
    Result r = read_wav(argv[1], w);
    if (std::holds_alternative<Error>(r))
        return 1;

    std::printf("reading %s: %d samples (%.1f s)\n",
                argv[1], w.n, (float)w.n / FS);

    /* 5-second windows */
    const int W = 5 * FS;
    int n_windows = w.n / W;
    if (n_windows == 0) {
        std::fprintf(stderr, "audio too short (%d samples, need %d)\n",
                     w.n, W);
        free(w.data);
        return 1;
    }

    FeatureSet f;
    int voice_votes = 0, music_votes = 0;

    for (int wIdx = 0; wIdx < n_windows; wIdx++) {
        const float *seg = w.data + wIdx * W;
        r = features(seg, W, f);
        if (std::holds_alternative<Error>(r))
            break;

        /* vote: syllable clock present in any band -> voice
         * beat clock present -> music
         * count which clock is stronger */
        float max_syll = 0.0f, max_beat = 0.0f;
        for (int i = 0; i < NUM_BANDS; i++) {
            if (f.syll_hz[i] > max_syll) max_syll = f.syll_hz[i];
            if (f.beat_hz[i] > max_beat) max_beat = f.beat_hz[i];
        }

        /* a band only "votes" if its clock was actually found (> 0) */
        int syll_bands = 0, beat_bands = 0;
        for (int i = 0; i < NUM_BANDS; i++) {
            if (f.syll_hz[i] > 0.0f) syll_bands++;
            if (f.beat_hz[i] > 0.0f) beat_bands++;
        }

        char label[16];
        if (syll_bands >= 2 && syll_bands > beat_bands) {
            std::snprintf(label, sizeof(label), "voice");
            voice_votes++;
        } else if (beat_bands >= 2) {
            std::snprintf(label, sizeof(label), "music");
            music_votes++;
        } else {
            std::snprintf(label, sizeof(label), "?");
        }

        std::printf(
            "  %4d  %s  "
            "syll=%3d bands  beat=%3d bands  "
            "best_syll=%.1fHz  best_beat=%.1fHz  "
            "spread=%.0fHz  flat=%.1fdB\n",
            wIdx * W / FS, label,
            syll_bands, beat_bands,
            max_syll, max_beat,
            f.spread_hz, f.band_std
        );
    }

    std::printf("\nvote: voice=%d  music=%d\n", voice_votes, music_votes);
    free(w.data);
    return 0;
}
