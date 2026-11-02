```cpp
#include <sndfile.h>
#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

constexpr int NFFT = 1024;
constexpr int HOP = 256;
constexpr int SHIFT = 4;
constexpr int FEATURE_WINDOW = 8;

constexpr double EPS = 1e-10;
constexpr double Z_THRESHOLD = 1.5;
constexpr double MASK_STRENGTH = 0.35;

using Complex = fftw_complex;

struct Audio {
    int sr = 16000;
    vector<double> x;
};

struct Spectrogram {
    int frames = 0;
    int bins = NFFT / 2 + 1;
    vector<Complex> data;

    Complex* at(int frame, int bin) {
        return data.data() + frame * bins + bin;
    }

    const Complex* at(int frame, int bin) const {
        return data.data() + frame * bins + bin;
    }
};

Audio load_wav(const string& path) {
    SF_INFO info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);

    if (!f)
        throw runtime_error("Cannot open " + path);

    vector<double> interleaved(info.frames * info.channels);
    sf_readf_double(f, interleaved.data(), info.frames);
    sf_close(f);

    Audio a;
    a.sr = info.samplerate;
    a.x.resize(info.frames);

    for (sf_count_t i = 0; i < info.frames; ++i) {
        for (int ch = 0; ch < info.channels; ++ch)
            a.x[i] += interleaved[i * info.channels + ch];

        a.x[i] /= info.channels;
    }

    return a;
}

void save_wav(const string& path, const Audio& a) {
    SF_INFO info{};
    info.samplerate = a.sr;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);

    if (!f)
        throw runtime_error("Cannot write " + path);

    sf_writef_double(f, a.x.data(), a.x.size());
    sf_close(f);
}

double rms(const vector<double>& x) {
    double sum = 0;

    for (double v : x)
        sum += v * v;

    return sqrt(sum / max<size_t>(1, x.size()) + EPS);
}

double correlation(const vector<double>& a,
                   const vector<double>& b) {
    size_t n = min(a.size(), b.size());

    double ma = 0, mb = 0;

    for (size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }

    ma /= n;
    mb /= n;

    double num = 0, aa = 0, bb = 0;

    for (size_t i = 0; i < n; ++i) {
        double x = a[i] - ma;
        double y = b[i] - mb;

        num += x * y;
        aa += x * x;
        bb += y * y;
    }

    return num / sqrt(aa * bb + EPS);
}

double si_sdr(const vector<double>& ref,
              const vector<double>& est) {
    size_t n = min(ref.size(), est.size());

    double dot = 0, energy = 0;

    for (size_t i = 0; i < n; ++i) {
        dot += ref[i] * est[i];
        energy += ref[i] * ref[i];
    }

    double alpha = dot / (energy + EPS);
    double target = 0, error = 0;

    for (size_t i = 0; i < n; ++i) {
        double t = alpha * ref[i];
        double e = est[i] - t;

        target += t * t;
        error += e * e;
    }

    return 10.0 * log10((target + EPS) / (error + EPS));
}

vector<double> hann_window() {
    vector<double> w(NFFT);

    for (int i = 0; i < NFFT; ++i)
        w[i] = 0.5 - 0.5 * cos(2.0 * M_PI * i / (NFFT - 1));

    return w;
}

Spectrogram stft(const Audio& audio) {
    Spectrogram s;
    s.frames = int(audio.x.size() / HOP) + 1;
    s.data.resize(s.frames * s.bins);

    auto window = hann_window();

    double* in = fftw_alloc_real(NFFT);
    Complex* out = fftw_alloc_complex(s.bins);

    auto plan = fftw_plan_dft_r2c_1d(
        NFFT, in, out, FFTW_ESTIMATE
    );

    for (int frame = 0; frame < s.frames; ++frame) {
        int center = frame * HOP;

        for (int k = 0; k < NFFT; ++k) {
            int index = center + k - NFFT / 2;

            double value = 0;

            if (index >= 0 && index < int(audio.x.size()))
                value = audio.x[index];

            in[k] = value * window[k];
        }

        fftw_execute(plan);

        for (int b = 0; b < s.bins; ++b) {
            s.at(frame, b)[0] = out[b][0];
            s.at(frame, b)[1] = out[b][1];
        }
    }

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);

    return s;
}

Audio istft(const Spectrogram& s, int sr, size_t length) {
    Audio audio;
    audio.sr = sr;
    audio.x.assign(length, 0.0);

    vector<double> weight(length, 0.0);
    auto window = hann_window();

    Complex* in = fftw_alloc_complex(s.bins);
    double* out = fftw_alloc_real(NFFT);

    auto plan = fftw_plan_dft_c2r_1d(
        NFFT, in, out, FFTW_ESTIMATE
    );

    for (int frame = 0; frame < s.frames; ++frame) {
        int center = frame * HOP;

        for (int b = 0; b < s.bins; ++b) {
            in[b][0] = s.at(frame, b)[0];
            in[b][1] = s.at(frame, b)[1];
        }

        fftw_execute(plan);

        for (int k = 0; k < NFFT; ++k) {
            int index = center + k - NFFT / 2;

            if (index < 0 || index >= int(length))
                continue;

            double w = window[k];

            audio.x[index] += out[k] / NFFT * w;
            weight[index] += w * w;
        }
    }

    for (size_t i = 0; i < length; ++i)
        audio.x[i] /= max(weight[i], EPS);

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);

    return audio;
}

double magnitude(const Complex* z) {
    return hypot(z[0], z[1]);
}

// Feature: log of time-shifted averaged magnitude.
vector<double> log_feature(const Spectrogram& s, int bin) {
    vector<double> feature(s.frames, 0.0);

    for (int n = 0; n < s.frames; ++n) {
        double sum = 0;
        int count = 0;

        for (int k = 0; k < FEATURE_WINDOW; ++k) {
            int a = n + k;
            int b = n + k + SHIFT;

            if (b >= s.frames)
                break;

            sum += 0.5 * (
                magnitude(s.at(a, bin)) +
                magnitude(s.at(b, bin))
            );

            ++count;
        }

        feature[n] = log(sum / max(1, count) + EPS);
    }

    return feature;
}

void recover(const Audio& clean,
             const Audio& contaminated,
             const string& prefix) {
    Spectrogram ref = stft(clean);
    Spectrogram mix = stft(contaminated);

    int frames = min(ref.frames, mix.frames);
    int bins = ref.bins;

    double feature_mae = 0;
    double mask_sum = 0;
    size_t mask_count = 0;

    for (int b = 0; b < bins; ++b) {
        auto ref_feature = log_feature(ref, b);
        auto mix_feature = log_feature(mix, b);

        double mean = 0, variance = 0;

        for (int n = 0; n < frames; ++n)
            mean += ref_feature[n];

        mean /= frames;

        for (int n = 0; n < frames; ++n) {
            double d = ref_feature[n] - mean;
            variance += d * d;
        }

        double sd = sqrt(variance / frames + EPS);

        for (int n = 0; n < frames; ++n) {
            double z = (mix_feature[n] - mean) / sd;

            // Additive percussion tends to increase magnitude.
            // Suppress only unusually high feature deviations.
            double excess = max(0.0, z - Z_THRESHOLD);

            double mask = exp(
                -MASK_STRENGTH * excess * excess
            );

            mask = clamp(mask, 0.05, 1.0);

            mix.at(n, b)[0] *= mask;
            mix.at(n, b)[1] *= mask;

            feature_mae += abs(mix_feature[n] - ref_feature[n]);
            mask_sum += mask;
            ++mask_count;
        }
    }

    Audio recovered = istft(
        mix, contaminated.sr, contaminated.x.size()
    );

    Audio residual = contaminated;

    for (size_t i = 0; i < residual.x.size(); ++i)
        residual.x[i] -= recovered.x[i];

    save_wav(prefix + "_recovered.wav", recovered);
    save_wav(prefix + "_residual.wav", residual);

    cout << prefix
         << " correlation=" << correlation(clean.x, recovered.x)
         << " SI-SDR=" << si_sdr(clean.x, recovered.x)
         << " avg_mask=" << mask_sum / max<size_t>(1, mask_count)
         << "\n";

    ofstream json(prefix + "_recovery.json");

    json << "{\n"
         << "  \"method\": \"template log-feature soft mask\",\n"
         << "  \"correlation\": " << correlation(clean.x, recovered.x) << ",\n"
         << "  \"si_sdr_db\": " << si_sdr(clean.x, recovered.x) << ",\n"
         << "  \"average_mask\": " << mask_sum / max<size_t>(1, mask_count) << ",\n"
         << "  \"feature_mae\": " << feature_mae / max<size_t>(1, mask_count) << ",\n"
         << "  \"mask_strength\": " << MASK_STRENGTH << ",\n"
         << "  \"z_threshold\": " << Z_THRESHOLD << "\n"
         << "}\n";
}

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            cerr << "Usage: " << argv[0]
                 << " clean.wav contaminated.wav [output_prefix]\n";
            return 1;
        }

        Audio clean = load_wav(argv[1]);
        Audio contaminated = load_wav(argv[2]);

        if (clean.sr != contaminated.sr)
            throw runtime_error("Sample rates must match");

        string prefix = argc > 3 ? argv[3] : "recovery";

        recover(clean, contaminated, prefix);

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
```

