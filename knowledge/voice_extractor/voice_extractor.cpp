/*
 * voice_extractor.cpp
 *
 * Group frequency bands whose amplitude envelopes share the same modulation.
 * A shared modulation is treated as evidence that the bands belong to one
 * source/voice. This is a modulation-coherence extractor, not a speaker
 * recognizer: voices with identical timing can merge, and one voice with
 * strongly different band envelopes can split.
 *
 * C-style implementation: explicit buffers, no STL containers, no exceptions.
 * `new (std::nothrow)` is used for owned buffers. Errors are returned as
 * Rust-like Result values.
 *
 * Input/output: 16 kHz, mono, PCM16 WAV. Each discovered group is written as
 * group_N.wav. The output is a sum of its band-pass components.
 *
 * Build:
 *   c++ -O2 -std=c++17 -Wall -Wextra voice_extractor.cpp -o voice_extractor
 *
 * Run:
 *   ./voice_extractor input.wav [output_prefix]
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>

static const int SAMPLE_RATE = 16000;
static const int BAND_COUNT = 16;
static const int DECIMATE = 8;                 // envelope rate = 2 kHz
static const float Q = 1.35f;
static const float CENTERS[BAND_COUNT] = {
    90.0f, 130.0f, 180.0f, 250.0f, 350.0f, 480.0f, 650.0f, 850.0f,
    1100.0f, 1450.0f, 1900.0f, 2500.0f, 3300.0f, 4300.0f, 5600.0f, 7000.0f
};
static const float PI_F = 3.14159265358979323846f;

struct Error {
    int line;
    const char *message;
};

struct Result {
    bool ok;
    Error error;
    static Result success() { return {true, {0, ""}}; }
    static Result failure(int line, const char *message) {
        return {false, {line, message}};
    }
};

#define FAIL(message) return Result::failure(__LINE__, message)
#define TRY(result) do { Result _r = (result); if (!_r.ok) return _r; } while (0)

struct Audio {
    int count;
    float *samples;
};

struct Biquad {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
};

struct Band {
    Biquad filter;
    float *signal;
    float *envelope;
    float modulation_rate;
    float modulation_confidence;
    int group;
};

static Biquad make_bandpass(float center) {
    const float w = 2.0f * PI_F * center / SAMPLE_RATE;
    const float alpha = sinf(w) / (2.0f * Q);
    const float norm = 1.0f + alpha;
    Biquad b;
    b.b0 = alpha / norm;
    b.b1 = 0.0f;
    b.b2 = -alpha / norm;
    b.a1 = -2.0f * cosf(w) / norm;
    b.a2 = (1.0f - alpha) / norm;
    b.x1 = b.x2 = b.y1 = b.y2 = 0.0f;
    return b;
}

static inline float filter_sample(Biquad *b, float x) {
    const float y = b->b0*x + b->b1*b->x1 + b->b2*b->x2
                  - b->a1*b->y1 - b->a2*b->y2;
    b->x2 = b->x1; b->x1 = x;
    b->y2 = b->y1; b->y1 = y;
    return y;
}

static Result read_u16(FILE *f, unsigned short *v) {
    unsigned char b[2];
    if (fread(b, 1, 2, f) != 2) FAIL("unexpected end of WAV");
    *v = (unsigned short)(b[0] | (b[1] << 8));
    return Result::success();
}

static Result read_u32(FILE *f, unsigned int *v) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) FAIL("unexpected end of WAV");
    *v = (unsigned int)b[0] | ((unsigned int)b[1] << 8)
       | ((unsigned int)b[2] << 16) | ((unsigned int)b[3] << 24);
    return Result::success();
}

/* Handles ordinary RIFF PCM WAV files, including extra chunks before data. */
static Result load_wav(const char *path, Audio *audio) {
    FILE *f = fopen(path, "rb");
    if (!f) FAIL("cannot open input WAV");

    char id[4];
    unsigned int riff_size;
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) != 0) {
        fclose(f); FAIL("not a RIFF WAV");
    }
    TRY(read_u32(f, &riff_size));
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4) != 0) {
        fclose(f); FAIL("not a WAVE file");
    }

    unsigned short format = 0, channels = 0, bits = 0;
    unsigned int rate = 0, data_size = 0;
    bool have_fmt = false, have_data = false;

    while (!have_data) {
        if (fread(id, 1, 4, f) != 4) break;
        unsigned int chunk_size = 0;
        TRY(read_u32(f, &chunk_size));
        long chunk_start = ftell(f);

        if (memcmp(id, "fmt ", 4) == 0) {
            unsigned int byte_rate;
            unsigned short block_align;
            TRY(read_u16(f, &format));
            TRY(read_u16(f, &channels));
            TRY(read_u32(f, &rate));
            TRY(read_u32(f, &byte_rate));
            TRY(read_u16(f, &block_align));
            TRY(read_u16(f, &bits));
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            data_size = chunk_size;
            have_data = true;
            break;
        }
        if (fseek(f, chunk_start + (long)chunk_size + (chunk_size & 1u), SEEK_SET) != 0) {
            fclose(f); FAIL("invalid WAV chunk size");
        }
    }

    if (!have_fmt || !have_data) { fclose(f); FAIL("WAV is missing fmt or data chunk"); }
    if (format != 1 || channels != 1 || bits != 16 || rate != SAMPLE_RATE) {
        fclose(f); FAIL("need PCM16 mono WAV at 16000 Hz (convert with ffmpeg)");
    }

    const int count = (int)(data_size / 2);
    if (count <= 0) { fclose(f); FAIL("WAV contains no samples"); }
    float *samples = new (std::nothrow) float[count];
    if (!samples) { fclose(f); FAIL("out of memory allocating input"); }

    for (int i = 0; i < count; ++i) {
        unsigned short u;
        if (!read_u16(f, &u).ok) {
            delete[] samples; fclose(f); FAIL("truncated PCM data");
        }
        short s = (short)u;
        samples[i] = (float)s / 32768.0f;
    }
    fclose(f);
    audio->count = count;
    audio->samples = samples;
    return Result::success();
}

static Result build_bands(const Audio *audio, Band *bands) {
    const int n = audio->count;
    const float env_a = 1.0f - expf(-1.0f / (SAMPLE_RATE * 0.008f));
    for (int b = 0; b < BAND_COUNT; ++b) {
        bands[b].filter = make_bandpass(CENTERS[b]);
        bands[b].signal = new (std::nothrow) float[n];
        bands[b].envelope = new (std::nothrow) float[n / DECIMATE + 1];
        bands[b].modulation_rate = 0.0f;
        bands[b].modulation_confidence = 0.0f;
        bands[b].group = -1;
        if (!bands[b].signal || !bands[b].envelope) FAIL("out of memory allocating filter bands");

        float env = 0.0f;
        int out = 0;
        for (int i = 0; i < n; ++i) {
            float y = filter_sample(&bands[b].filter, audio->samples[i]);
            bands[b].signal[i] = y;
            float a = fabsf(y);
            env += env_a * (a - env);
            if ((i % DECIMATE) == 0) bands[b].envelope[out++] = env;
        }
    }
    return Result::success();
}

/* Remove the slow level contour using a local moving average. */
static void detrend(float *src, float *dst, int n) {
    const int radius = 100; // 50 ms at 2 kHz
    double sum = 0.0;
    int left = 0, right = -1;
    for (int i = 0; i < n; ++i) {
        int want_l = i - radius;
        int want_r = i + radius;
        if (want_l < 0) want_l = 0;
        if (want_r >= n) want_r = n - 1;
        while (right < want_r) sum += src[++right];
        while (left < want_l) sum -= src[left++];
        float mean = (float)(sum / (right - left + 1));
        dst[i] = src[i] - mean;
    }
}

/* Pearson-like normalized correlation for two modulation envelopes.
 * A small lag search tolerates filter/envelope delay between frequency bands. */
static float modulation_similarity(const float *a, const float *b, int n) {
    if (n < 300) return 0.0f; // require >=150 ms at 2 kHz
    float *da = new (std::nothrow) float[n];
    float *db = new (std::nothrow) float[n];
    if (!da || !db) { delete[] da; delete[] db; return 0.0f; }
    detrend((float *)a, da, n);
    detrend((float *)b, db, n);

    double ea = 0.0, eb = 0.0;
    for (int i = 0; i < n; ++i) { ea += da[i]*da[i]; eb += db[i]*db[i]; }
    if (ea < 1e-12 || eb < 1e-12) {
        delete[] da; delete[] db; return 0.0f;
    }

    const int max_lag = 40; // +/-20 ms
    float best = 0.0f;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        double cross = 0.0, xa = 0.0, xb = 0.0;
        for (int i = max_lag; i < n - max_lag; ++i) {
            int j = i + lag;
            cross += da[i] * db[j];
            xa += da[i] * da[i];
            xb += db[j] * db[j];
        }
        float c = (float)(cross / (sqrt(xa * xb) + 1e-18));
        if (c > best) best = c;
    }
    delete[] da;
    delete[] db;
    return best;
}

/* Estimate a band's dominant modulation rate via autocorrelation. */
static void estimate_modulation(Band *band, int env_count) {
    float *d = new (std::nothrow) float[env_count];
    if (!d) return;
    detrend(band->envelope, d, env_count);
    double energy = 0.0;
    for (int i = 0; i < env_count; ++i) energy += d[i]*d[i];
    if (energy < 1e-12) { delete[] d; return; }

    const float env_rate = (float)SAMPLE_RATE / DECIMATE;
    const int min_lag = (int)(env_rate / 12.0f);
    const int max_lag = (int)(env_rate / 1.5f);
    float best = 0.0f;
    int best_lag = 0;
    for (int lag = min_lag; lag <= max_lag && lag < env_count / 2; ++lag) {
        double cross = 0.0;
        for (int i = 0; i < env_count - lag; ++i) cross += d[i] * d[i + lag];
        float c = (float)(cross / energy);
        if (c > best) { best = c; best_lag = lag; }
    }
    if (best_lag && best > 0.18f) {
        band->modulation_rate = env_rate / best_lag;
        band->modulation_confidence = best;
    }
    delete[] d;
}

/* Connected components of the coherence graph form modulation groups. */
static int group_bands(Band *bands, int env_count, float threshold) {
    bool edge[BAND_COUNT][BAND_COUNT] = {};
    for (int i = 0; i < BAND_COUNT; ++i) {
        for (int j = i + 1; j < BAND_COUNT; ++j) {
            float sim = modulation_similarity(bands[i].envelope,
                                              bands[j].envelope, env_count);
            float fi = bands[i].modulation_rate;
            float fj = bands[j].modulation_rate;
            bool clocks_match = (fi == 0.0f || fj == 0.0f)
                ? true : (fabsf(fi - fj) <= 1.0f || fabsf(fi - fj) <= 0.22f * (fi > fj ? fi : fj));
            edge[i][j] = edge[j][i] = (sim >= threshold && clocks_match);
        }
    }

    int group_count = 0;
    for (int i = 0; i < BAND_COUNT; ++i) {
        if (bands[i].group >= 0) continue;
        int queue[BAND_COUNT], head = 0, tail = 0;
        queue[tail++] = i;
        bands[i].group = group_count;
        while (head < tail) {
            int at = queue[head++];
            for (int j = 0; j < BAND_COUNT; ++j) {
                if (edge[at][j] && bands[j].group < 0) {
                    bands[j].group = group_count;
                    queue[tail++] = j;
                }
            }
        }
        ++group_count;
    }
    return group_count;
}

static Result write_wav(const char *path, const float *samples, int count) {
    FILE *f = fopen(path, "wb");
    if (!f) FAIL("cannot create output WAV");
    const unsigned int data_bytes = (unsigned int)(count * 2);
    fwrite("RIFF", 1, 4, f);
    unsigned char u32[4];
    #define PUT32(v) do { unsigned int _v=(unsigned int)(v); u32[0]=_v&255; u32[1]=(_v>>8)&255; u32[2]=(_v>>16)&255; u32[3]=(_v>>24)&255; fwrite(u32,1,4,f); } while(0)
    unsigned char u16[2];
    #define PUT16(v) do { unsigned short _v=(unsigned short)(v); u16[0]=_v&255; u16[1]=(_v>>8)&255; fwrite(u16,1,2,f); } while(0)
    fwrite("WAVEfmt ", 1, 8, f);
    PUT32(16); PUT16(1); PUT16(1); PUT32(SAMPLE_RATE);
    PUT32(SAMPLE_RATE * 2); PUT16(2); PUT16(16);
    fwrite("data", 1, 4, f); PUT32(data_bytes);
    for (int i = 0; i < count; ++i) {
        float x = samples[i];
        if (x > 1.0f) x = 1.0f;
        if (x < -1.0f) x = -1.0f;
        short s = (short)lrintf(x * 32767.0f);
        PUT16(s);
    }
    bool good = !ferror(f);
    fclose(f);
    if (!good) FAIL("failed while writing output WAV");
    return Result::success();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s input.wav [output_prefix]\n", argv[0]);
        return 2;
    }
    const char *prefix = argc > 2 ? argv[2] : "voice";
    Audio audio = {0, NULL};
    Result r = load_wav(argv[1], &audio);
    if (!r.ok) {
        fprintf(stderr, "error at line %d: %s\n", r.error.line, r.error.message);
        return 1;
    }

    Band bands[BAND_COUNT] = {};
    build_bands(&audio, bands);
    for (int i = 0; i < BAND_COUNT; ++i) {
        if (!bands[i].signal || !bands[i].envelope) {
            fprintf(stderr, "error: out of memory allocating filter bands\n");
            for (int j = 0; j < BAND_COUNT; ++j) {
                delete[] bands[j].signal; delete[] bands[j].envelope;
            }
            delete[] audio.samples;
            return 1;
        }
    }

    const int env_count = (audio.count - 1) / DECIMATE + 1;
    for (int i = 0; i < BAND_COUNT; ++i) estimate_modulation(&bands[i], env_count);
    const int groups = group_bands(bands, env_count, 0.48f);

    printf("voice extractor: %d samples, %.2f s, %d modulation groups\n",
           audio.count, (double)audio.count / SAMPLE_RATE, groups);
    printf("band      range-centre    mod-Hz  confidence  group\n");
    for (int i = 0; i < BAND_COUNT; ++i) {
        printf("%2d        %7.1f Hz     %6.2f     %5.2f       %d\n",
               i, CENTERS[i], bands[i].modulation_rate,
               bands[i].modulation_confidence, bands[i].group);
    }

    float *mix = new (std::nothrow) float[audio.count];
    if (!mix) {
        fprintf(stderr, "error: out of memory allocating output mix\n");
    } else {
        for (int g = 0; g < groups; ++g) {
            memset(mix, 0, sizeof(float) * audio.count);
            int members = 0;
            for (int b = 0; b < BAND_COUNT; ++b) {
                if (bands[b].group != g) continue;
                ++members;
                for (int s = 0; s < audio.count; ++s) mix[s] += bands[b].signal[s];
            }
            /* Keep singleton bands too: they may contain a voice component
             * whose modulation is too weak to correlate with its neighbours. */
            char path[512];
            snprintf(path, sizeof(path), "%s_group_%02d.wav", prefix, g);
            r = write_wav(path, mix, audio.count);
            if (!r.ok) fprintf(stderr, "error at line %d: %s\n", r.error.line, r.error.message);
            else printf("wrote %s (%d bands)\n", path, members);
        }
        delete[] mix;
    }

    for (int i = 0; i < BAND_COUNT; ++i) {
        delete[] bands[i].signal;
        delete[] bands[i].envelope;
    }
    delete[] audio.samples;
    return 0;
}
