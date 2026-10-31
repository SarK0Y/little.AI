
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
static const int DECIMATE = 8;
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
    float modulation_strength;
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
    b->x2 = b->x1;
    b->x1 = x;
    b->y2 = b->y1;
    b->y1 = y;
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
        fclose(f);
        FAIL("not a RIFF WAV");
    }

    if (!read_u32(f, &riff_size).ok) {
        fclose(f);
        FAIL("invalid RIFF header");
    }

    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4) != 0) {
        fclose(f);
        FAIL("not a WAVE file");
    }

    unsigned short format = 0, channels = 0, bits = 0;
    unsigned int rate = 0, data_size = 0;
    bool have_fmt = false, have_data = false;

    while (!have_data) {
        if (fread(id, 1, 4, f) != 4) break;

        unsigned int chunk_size = 0;
        if (!read_u32(f, &chunk_size).ok) {
            fclose(f);
            FAIL("invalid WAV chunk header");
        }

        long chunk_start = ftell(f);

        if (memcmp(id, "fmt ", 4) == 0) {
            unsigned int byte_rate;
            unsigned short block_align;

            if (!read_u16(f, &format).ok ||
                !read_u16(f, &channels).ok ||
                !read_u32(f, &rate).ok ||
                !read_u32(f, &byte_rate).ok ||
                !read_u16(f, &block_align).ok ||
                !read_u16(f, &bits).ok) {
                fclose(f);
                FAIL("invalid WAV fmt chunk");
            }
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            data_size = chunk_size;
            have_data = true;
            break;
        }

        if (fseek(f, chunk_start + (long)chunk_size +
                  (chunk_size & 1u), SEEK_SET) != 0) {
            fclose(f);
            FAIL("invalid WAV chunk size");
        }
    }

    if (!have_fmt || !have_data) {
        fclose(f);
        FAIL("WAV is missing fmt or data chunk");
    }

    if (format != 1 || channels != 1 || bits != 16 ||
        rate != SAMPLE_RATE) {
        fclose(f);
        FAIL("need PCM16 mono WAV at 16000 Hz (convert with ffmpeg)");
    }

    const int count = (int)(data_size / 2);
    if (count <= 0) {
        fclose(f);
        FAIL("WAV contains no samples");
    }

    float *samples = new (std::nothrow) float[count];
    if (!samples) {
        fclose(f);
        FAIL("out of memory allocating input");
    }

    for (int i = 0; i < count; ++i) {
        unsigned short u;
        if (!read_u16(f, &u).ok) {
            delete[] samples;
            fclose(f);
            FAIL("truncated PCM data");
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
        bands[b].modulation_strength = 0.0f;
        bands[b].group = -1;

        if (!bands[b].signal || !bands[b].envelope)
            FAIL("out of memory allocating filter bands");

        float env = 0.0f;
        int out = 0;

        for (int i = 0; i < n; ++i) {
            float y = filter_sample(&bands[b].filter, audio->samples[i]);
            bands[b].signal[i] = y;

            float a = fabsf(y);
            env += env_a * (a - env);

            if ((i % DECIMATE) == 0)
                bands[b].envelope[out++] = env;
        }
    }

    return Result::success();
}

/* Remove only the slow level contour. */
static void detrend(float *src, float *dst, int n) {
    const int radius = 250;
    double sum = 0.0;
    int left = 0, right = -1;

    for (int i = 0; i < n; ++i) {
        int want_l = i - radius;
        int want_r = i + radius;

        if (want_l < 0) want_l = 0;
        if (want_r >= n) want_r = n - 1;

        while (right < want_r) sum += src[++right];
        while (left < want_l) sum -= src[left++];

        const float mean = (float)(sum / (right - left + 1));
        dst[i] = src[i] - mean;
    }
}

/* Pearson correlation over overlapping envelope samples and allowed lags. */
static float modulation_similarity(const float *a, const float *b, int n) {
    if (n < 160) return 0.0f;

    float *da = new (std::nothrow) float[n];
    float *db = new (std::nothrow) float[n];

    if (!da || !db) {
        delete[] da;
        delete[] db;
        return 0.0f;
    }

    detrend((float *)a, da, n);
    detrend((float *)b, db, n);

    const int max_lag = 80;
    float best = 0.0f;

    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        const int begin = lag < 0 ? -lag : 0;
        const int end = lag > 0 ? n - lag : n;
        const int count = end - begin;

        if (count < n / 2) continue;

        double ma = 0.0, mb = 0.0;

        for (int i = begin; i < end; ++i) {
            ma += da[i];
            mb += db[i + lag];
        }

        ma /= count;
        mb /= count;

        double cross = 0.0, ea = 0.0, eb = 0.0;

        for (int i = begin; i < end; ++i) {
            const double x = da[i] - ma;
            const double y = db[i + lag] - mb;
            cross += x * y;
            ea += x * x;
            eb += y * y;
        }

        const double denom = sqrt(ea * eb);

        if (denom > 1e-18) {
            const float corr = (float)(cross / denom);
            if (corr > best) best = corr;
        }
    }

    delete[] da;
    delete[] db;
    return best;
}

/* Modulation strength is envelope variation relative to its mean. */
static void estimate_modulation(Band *band, int env_count) {
    double mean = 0.0;

    for (int i = 0; i < env_count; ++i)
        mean += band->envelope[i];

    mean /= (env_count > 0 ? env_count : 1);
    if (mean <= 1e-9) return;

    double variance = 0.0;

    for (int i = 0; i < env_count; ++i) {
        const double d = band->envelope[i] - mean;
        variance += d * d;
    }

    variance /= (env_count > 0 ? env_count : 1);

    const double cv = sqrt(variance) / mean;
    band->modulation_strength = (float)(cv / (1.0 + cv));

    float *d = new (std::nothrow) float[env_count];
    if (!d) return;

    detrend(band->envelope, d, env_count);

    const float env_rate = (float)SAMPLE_RATE / DECIMATE;
    const int min_lag = (int)(env_rate / 30.0f);
    const int max_lag = (int)(env_rate / 0.7f);

    float best = 0.0f;
    int best_lag = 0;

    for (int lag = min_lag; lag <= max_lag && lag < env_count / 2; ++lag) {
        double cross = 0.0, ea = 0.0, eb = 0.0;

        for (int i = 0; i < env_count - lag; ++i) {
            cross += (double)d[i] * d[i + lag];
            ea += (double)d[i] * d[i];
            eb += (double)d[i + lag] * d[i + lag];
        }

        const double denom = sqrt(ea * eb);
        const float corr = denom > 1e-18 ?
            (float)(cross / denom) : 0.0f;

        if (corr > best) {
            best = corr;
            best_lag = lag;
        }
    }

    if (best_lag && best > 0.12f)
        band->modulation_rate = env_rate / best_lag;

    delete[] d;
}

/* Connected components of the coherence graph form modulation groups. */
static int group_bands(Band *bands, int env_count, float threshold) {
    bool edge[BAND_COUNT][BAND_COUNT] = {};
    float similarity[BAND_COUNT][BAND_COUNT] = {};

    for (int i = 0; i < BAND_COUNT; ++i) {
        for (int j = i + 1; j < BAND_COUNT; ++j) {
            const float sim = modulation_similarity(
                bands[i].envelope, bands[j].envelope, env_count);

            similarity[i][j] = similarity[j][i] = sim;
            edge[i][j] = edge[j][i] = (sim >= threshold);
        }
    }

    printf("\nmodulation similarity (upper triangle):\n    ");

    for (int i = 0; i < BAND_COUNT; ++i)
        printf("%2d ", i);

    printf("\n");

    for (int i = 0; i < BAND_COUNT; ++i) {
        printf("%2d: ", i);

        for (int j = 0; j < BAND_COUNT; ++j) {
            if (j <= i) printf(" -- ");
            else printf("%0.2f ", similarity[i][j]);
        }

        printf("\n");
    }

    int group_count = 0;

    for (int i = 0; i < BAND_COUNT; ++i) {
        if (bands[i].group >= 0) continue;

        int queue[BAND_COUNT], head = 0, tail = 0;
        queue[tail++] = i;
        bands[i].group = group_count;

        while (head < tail) {
            const int at = queue[head++];

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

/*
 * Write a standard 44-byte PCM16 RIFF/WAVE header.
 *
 * RIFF layout:
 *   "RIFF" + RIFF chunk size
 *   "WAVE"
 *   "fmt " + PCM format data
 *   "data" + PCM data size
 */
static Result write_wav(const char *path, const float *samples, int count) {
    if (!samples || count <= 0)
        FAIL("no samples to write");

    if ((unsigned long long)count > (0xFFFFFFFFull - 36ull) / 2ull)
        FAIL("output WAV is too large for RIFF");

    FILE *f = fopen(path, "wb");
    if (!f)
        FAIL("cannot create output WAV");

    const unsigned int data_bytes = (unsigned int)count * 2u;
    const unsigned int riff_size = 36u + data_bytes;

    unsigned char header[44] = {
        'R','I','F','F',
        0,0,0,0,                 /* RIFF chunk size */
        'W','A','V','E',
        'f','m','t',' ',
        16,0,0,0,                /* PCM fmt chunk size */
        1,0,                     /* PCM */
        1,0,                     /* mono */
        0x80,0x3e,0,0,           /* 16000 Hz */
        0x00,0x7d,0,0,           /* 32000 bytes/sec */
        2,0,                     /* block align */
        16,0,                    /* 16 bits/sample */
        'd','a','t','a',
        0,0,0,0                  /* data chunk size */
    };

    /* RIFF size is the file size minus the first 8 bytes. */
    header[4] = (unsigned char)(riff_size & 0xffu);
    header[5] = (unsigned char)((riff_size >> 8) & 0xffu);
    header[6] = (unsigned char)((riff_size >> 16) & 0xffu);
    header[7] = (unsigned char)((riff_size >> 24) & 0xffu);

    header[40] = (unsigned char)(data_bytes & 0xffu);
    header[41] = (unsigned char)((data_bytes >> 8) & 0xffu);
    header[42] = (unsigned char)((data_bytes >> 16) & 0xffu);
    header[43] = (unsigned char)((data_bytes >> 24) & 0xffu);

    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        FAIL("failed while writing WAV header");
    }

    for (int i = 0; i < count; ++i) {
        float x = samples[i];

        if (x > 1.0f) x = 1.0f;
        if (x < -1.0f) x = -1.0f;

        const short sample = (short)lrintf(x * 32767.0f);
        const unsigned short u = (unsigned short)sample;

        const unsigned char bytes[2] = {
            (unsigned char)(u & 0xffu),
            (unsigned char)((u >> 8) & 0xffu)
        };

        if (fwrite(bytes, 1, 2, f) != 2) {
            fclose(f);
            FAIL("failed while writing output WAV");
        }
    }

    if (fclose(f) != 0)
        FAIL("failed while closing output WAV");

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
        fprintf(stderr, "error at line %d: %s\n",
                r.error.line, r.error.message);
        return 1;
    }

    Band bands[BAND_COUNT] = {};

    r = build_bands(&audio, bands);

    if (!r.ok) {
        fprintf(stderr, "error at line %d: %s\n",
                r.error.line, r.error.message);

        for (int i = 0; i < BAND_COUNT; ++i) {
            delete[] bands[i].signal;
            delete[] bands[i].envelope;
        }

        delete[] audio.samples;
        return 1;
    }

    for (int i = 0; i < BAND_COUNT; ++i) {
        if (!bands[i].signal || !bands[i].envelope) {
            fprintf(stderr, "error: out of memory allocating filter bands\n");

            for (int j = 0; j < BAND_COUNT; ++j) {
                delete[] bands[j].signal;
                delete[] bands[j].envelope;
            }

            delete[] audio.samples;
            return 1;
        }
    }

    const int env_count = (audio.count - 1) / DECIMATE + 1;

    for (int i = 0; i < BAND_COUNT; ++i)
        estimate_modulation(&bands[i], env_count);

    const int groups = group_bands(bands, env_count, 0.32f);

    printf("voice extractor: %d samples, %.2f s, %d modulation groups\n",
           audio.count, (double)audio.count / SAMPLE_RATE, groups);

    printf("band      range-centre    mod-Hz  strength    group\n");

    for (int i = 0; i < BAND_COUNT; ++i) {
        printf("%2d        %7.1f Hz     %6.2f     %5.2f       %d\n",
               i, CENTERS[i], bands[i].modulation_rate,
               bands[i].modulation_strength, bands[i].group);
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

                for (int s = 0; s < audio.count; ++s)
                    mix[s] += bands[b].signal[s];
            }

            char path[512];
            snprintf(path, sizeof(path), "%s_group_%02d.wav", prefix, g);

            r = write_wav(path, mix, audio.count);

            if (!r.ok) {
                fprintf(stderr, "error at line %d: %s\n",
                        r.error.line, r.error.message);
            } else {
                printf("wrote %s (%d bands)\n", path, members);
            }
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

