#include <sndfile.h>
#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

// Log/time-shift feature reconstruction.
// The feature is computed per STFT bin over time:
//   log(mean_k((|X[n+k]| + |X[n+k+SHIFT]|) / 2) + EPS)
//
// This is an experimental feature-guided spectral attenuation method.
// It is not a mathematically exact source separator.

constexpr int NFFT = 1024;
constexpr int HOP = 256;
constexpr int SHIFT = 4;             // shift in STFT frames
constexpr int FEATURE_WINDOW = 8;   // accumulation window in STFT frames

constexpr double EPS = 1e-10;
constexpr double LOG_EXCESS_THRESHOLD = 0.15;
constexpr double MASK_STRENGTH = 1.8;
constexpr double MIN_MASK = 0.04;

using Complex = fftw_complex;

struct Audio {
    int sr = 16000;
    vector<double> x;
};

struct Spectrogram {
    int frames = 0;
    int bins = NFFT / 2 + 1;
    vector<double> data; // interleaved real/imaginary values

    double* at(int frame, int bin) {
        return data.data() + (size_t(frame) * bins + bin) * 2;
    }

    const double* at(int frame, int bin) const {
        return data.data() + (size_t(frame) * bins + bin) * 2;
    }
};

Audio load_wav(const string& path) {
    SF_INFO info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f)
        throw runtime_error("Cannot open " + path + ": " + sf_strerror(nullptr));

    if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
        sf_close(f);
        throw runtime_error("Invalid WAV metadata: " + path);
    }

    vector<double> interleaved((size_t)info.frames * info.channels);
    sf_count_t read_frames = sf_readf_double(f, interleaved.data(), info.frames);
    sf_close(f);

    if (read_frames != info.frames)
        throw runtime_error("Short read from " + path);

    Audio a;
    a.sr = info.samplerate;
    a.x.resize((size_t)info.frames);

    for (sf_count_t i = 0; i < info.frames; ++i) {
        double sum = 0.0;
        for (int ch = 0; ch < info.channels; ++ch)
            sum += interleaved[(size_t)i * info.channels + ch];
        a.x[(size_t)i] = sum / info.channels;
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
        throw runtime_error("Cannot write " + path + ": " + sf_strerror(nullptr));

    sf_count_t written = sf_writef_double(f, a.x.data(), (sf_count_t)a.x.size());
    sf_close(f);
    if (written != (sf_count_t)a.x.size())
        throw runtime_error("Short write to " + path);
}

double correlation(const vector<double>& a, const vector<double>& b) {
    size_t n = min(a.size(), b.size());
    if (n == 0) return 0.0;

    double ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= n;
    mb /= n;

    double num = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x = a[i] - ma;
        double y = b[i] - mb;
        num += x * y;
        aa += x * x;
        bb += y * y;
    }
    return num / sqrt(aa * bb + EPS);
}

double si_sdr(const vector<double>& ref, const vector<double>& est) {
    size_t n = min(ref.size(), est.size());
    if (n == 0) return 0.0;

    double dot = 0.0, energy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        dot += ref[i] * est[i];
        energy += ref[i] * ref[i];
    }

    double alpha = dot / (energy + EPS);
    double target = 0.0, error = 0.0;
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
    s.data.resize(size_t(s.frames) * s.bins * 2);

    auto window = hann_window();
    double* in = fftw_alloc_real(NFFT);
    Complex* out = fftw_alloc_complex(s.bins);
    if (!in || !out) {
        if (in) fftw_free(in);
        if (out) fftw_free(out);
        throw runtime_error("FFTW allocation failed");
    }

    auto plan = fftw_plan_dft_r2c_1d(NFFT, in, out, FFTW_ESTIMATE);
    if (!plan) {
        fftw_free(in);
        fftw_free(out);
        throw runtime_error("FFTW STFT plan failed");
    }

    for (int frame = 0; frame < s.frames; ++frame) {
        int center = frame * HOP;
        for (int k = 0; k < NFFT; ++k) {
            int index = center + k - NFFT / 2;
            double value = (index >= 0 && index < (int)audio.x.size())
                ? audio.x[(size_t)index] : 0.0;
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
    if (!in || !out) {
        if (in) fftw_free(in);
        if (out) fftw_free(out);
        throw runtime_error("FFTW allocation failed");
    }

    auto plan = fftw_plan_dft_c2r_1d(NFFT, in, out, FFTW_ESTIMATE);
    if (!plan) {
        fftw_free(in);
        fftw_free(out);
        throw runtime_error("FFTW ISTFT plan failed");
    }

    for (int frame = 0; frame < s.frames; ++frame) {
        int center = frame * HOP;
        for (int b = 0; b < s.bins; ++b) {
            in[b][0] = s.at(frame, b)[0];
            in[b][1] = s.at(frame, b)[1];
        }

        fftw_execute(plan);
        for (int k = 0; k < NFFT; ++k) {
            int index = center + k - NFFT / 2;
            if (index < 0 || index >= (int)length)
                continue;

            double w = window[k];
            audio.x[(size_t)index] += out[k] / NFFT * w;
            weight[(size_t)index] += w * w;
        }
    }

    for (size_t i = 0; i < length; ++i)
        audio.x[i] /= max(weight[i], EPS);

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);
    return audio;
}

double magnitude(const double* z) {
    return hypot(z[0], z[1]);
}

// Log/time-shift feature: time accumulation in STFT frames,
// not a frequency-bin shift.
vector<double> log_time_shift_feature(const Spectrogram& s, int bin) {
    vector<double> feature(s.frames, 0.0);

    for (int n = 0; n < s.frames; ++n) {
        double sum = 0.0;
        int count = 0;

        for (int k = 0; k < FEATURE_WINDOW; ++k) {
            int a = n + k;
            int b = n + k + SHIFT;
            if (b >= s.frames)
                break;

            sum += 0.5 * (magnitude(s.at(a, bin)) +
                           magnitude(s.at(b, bin)));
            ++count;
        }

        feature[n] = log(sum / max(1, count) + EPS);
    }
    return feature;
}

void reconstruct(const Audio& clean,
                const Audio& contaminated,
                const string& prefix) {
    Spectrogram ref = stft(clean);
    Spectrogram mix = stft(contaminated);

    if (ref.bins != mix.bins)
        throw runtime_error("STFT bin count mismatch");

    const int frames = min(ref.frames, mix.frames);
    const int bins = ref.bins;

    double feature_delta_sum = 0.0;
    double mask_sum = 0.0;
    size_t count = 0;

    // The clean template acts as a frame-by-frame feature reference.
    // Positive log-feature excess is treated as possible added energy.
    for (int b = 0; b < bins; ++b) {
        auto clean_feature = log_time_shift_feature(ref, b);
        auto mix_feature = log_time_shift_feature(mix, b);

        for (int n = 0; n < frames; ++n) {
            double delta = mix_feature[n] - clean_feature[n];
            feature_delta_sum += abs(delta);

            double excess = max(0.0, delta - LOG_EXCESS_THRESHOLD);
            double mask = exp(-MASK_STRENGTH * excess * excess);
            mask = clamp(mask, MIN_MASK, 1.0);

            mix.at(n, b)[0] *= mask;
            mix.at(n, b)[1] *= mask;

            mask_sum += mask;
            ++count;
        }
    }

    Audio recovered = istft(mix, contaminated.sr, contaminated.x.size());

    Audio residual = contaminated;
    for (size_t i = 0; i < residual.x.size(); ++i)
        residual.x[i] -= recovered.x[i];

    save_wav(prefix + "_reconstructed.wav", recovered);
    save_wav(prefix + "_residual.wav", residual);

    const double corr = correlation(clean.x, recovered.x);
    const double sdr = si_sdr(clean.x, recovered.x);
    const double avg_mask = mask_sum / max<size_t>(1, count);
    const double avg_delta = feature_delta_sum / max<size_t>(1, count);

    cout << prefix
         << " correlation=" << corr
         << " SI-SDR=" << sdr
         << " avg_mask=" << avg_mask
         << " log_feature_delta=" << avg_delta << '\n';

    ofstream json(prefix + "_reconstruct.json");
    if (!json)
        throw runtime_error("Cannot write JSON: " + prefix + "_reconstruct.json");

    json << fixed << setprecision(8)
         << "{\n"
         << "  \"method\": \"log_time_shift_feature_reference_mask\",\n"
         << "  \"nfft\": " << NFFT << ",\n"
         << "  \"hop\": " << HOP << ",\n"
         << "  \"shift_frames\": " << SHIFT << ",\n"
         << "  \"feature_window_frames\": " << FEATURE_WINDOW << ",\n"
         << "  \"log_excess_threshold\": " << LOG_EXCESS_THRESHOLD << ",\n"
         << "  \"mask_strength\": " << MASK_STRENGTH << ",\n"
         << "  \"minimum_mask\": " << MIN_MASK << ",\n"
         << "  \"correlation\": " << corr << ",\n"
         << "  \"si_sdr_db\": " << sdr << ",\n"
         << "  \"average_mask\": " << avg_mask << ",\n"
         << "  \"mean_absolute_log_feature_delta\": " << avg_delta << "\n"
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
        if (clean.x.empty() || contaminated.x.empty())
            throw runtime_error("Input WAV is empty");

        string prefix = argc > 3 ? argv[3] : "template_reconstruct";
        reconstruct(clean, contaminated, prefix);
    } catch (const exception& e) {
        cerr << "Error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}

