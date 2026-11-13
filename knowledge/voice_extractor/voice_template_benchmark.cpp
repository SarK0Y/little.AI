#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace std;

constexpr double PI  = 3.14159265358979323846;
constexpr double EPS = 1e-10;

struct Audio {
    int sample_rate = 16000;
    vector<double> samples;
};

struct Metrics {
    double sir_db = 0.0;
    double correlation = 0.0;
    double si_sdr_db = 0.0;
    double feature_error = 0.0;
};

Audio load_wav(const string& path) {
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);

    if (!file)
        throw runtime_error("Cannot open WAV: " + path);

    vector<double> interleaved(info.frames * info.channels);

    sf_readf_double(file, interleaved.data(), info.frames);
    sf_close(file);

    Audio audio;
    audio.sample_rate = info.samplerate;
    audio.samples.resize(info.frames);

    for (sf_count_t i = 0; i < info.frames; ++i) {
        double sum = 0.0;

        for (int ch = 0; ch < info.channels; ++ch)
            sum += interleaved[i * info.channels + ch];

        audio.samples[i] = sum / info.channels;
    }

    return audio;
}

void save_wav(const string& path, const Audio& audio) {
    SF_INFO info{};
    info.samplerate = audio.sample_rate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* file = sf_open(path.c_str(), SFM_WRITE, &info);

    if (!file)
        throw runtime_error("Cannot write WAV: " + path);

    sf_writef_double(
        file,
        audio.samples.data(),
        static_cast<sf_count_t>(audio.samples.size())
    );

    sf_close(file);
}

double rms(const vector<double>& x) {
    double sum = 0.0;

    for (double v : x)
        sum += v * v;

    return sqrt(sum / max<size_t>(1, x.size()) + EPS);
}

void normalize_rms(vector<double>& x, double target) {
    double current = rms(x);

    if (current < EPS)
        return;

    double gain = target / current;

    for (double& v : x)
        v *= gain;
}

double correlation(
    const vector<double>& a,
    const vector<double>& b
) {
    size_t n = min(a.size(), b.size());

    double ma = 0.0, mb = 0.0;

    for (size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }

    ma /= n;
    mb /= n;

    double num = 0.0, da = 0.0, db = 0.0;

    for (size_t i = 0; i < n; ++i) {
        double x = a[i] - ma;
        double y = b[i] - mb;

        num += x * y;
        da += x * x;
        db += y * y;
    }

    return num / sqrt(da * db + EPS);
}

double si_sdr(
    const vector<double>& reference,
    const vector<double>& estimate
) {
    size_t n = min(reference.size(), estimate.size());

    double dot = 0.0, ref_energy = 0.0;

    for (size_t i = 0; i < n; ++i) {
        dot += reference[i] * estimate[i];
        ref_energy += reference[i] * reference[i];
    }

    double alpha = dot / (ref_energy + EPS);

    double target_energy = 0.0;
    double error_energy = 0.0;

    for (size_t i = 0; i < n; ++i) {
        double target = alpha * reference[i];
        double error = estimate[i] - target;

        target_energy += target * target;
        error_energy += error * error;
    }

    return 10.0 * log10(
        (target_energy + EPS) / (error_energy + EPS)
    );
}

// ------------------------------------------------------------
// Synthetic broad-band synth percussion
// ------------------------------------------------------------

vector<double> synth_percussion(
    size_t length,
    int sample_rate,
    uint32_t seed
) {
    mt19937 rng(seed);

    uniform_real_distribution<double> uniform(0.0, 1.0);
    normal_distribution<double> normal(0.0, 1.0);

    vector<double> noise(length);

    for (double& x : noise)
        x = normal(rng);

    vector<double> result(length, 0.0);

    // Broad-band noise bed.
    for (size_t i = 0; i < length; ++i) {
        double t = double(i) / sample_rate;

        double envelope =
            0.15
            + 0.10 * sin(2.0 * PI * 0.7 * t)
            + 0.08 * sin(2.0 * PI * 1.3 * t);

        result[i] = noise[i] * envelope;
    }

    // Irregular percussion hits.
    double hit_time = 0.15;

    while (hit_time < double(length) / sample_rate) {
        size_t start = size_t(hit_time * sample_rate);

        double duration = 0.04 + uniform(rng) * 0.26;
        size_t count = size_t(duration * sample_rate);

        double f0 = 80.0 + uniform(rng) * 820.0;
        double sweep = 300.0 + uniform(rng) * 3700.0;

        double phase_offset = uniform(rng) * 2.0 * PI;

        for (size_t j = 0; j < count && start + j < length; ++j) {
            double t = double(j) / sample_rate;

            double decay = exp(-t * (8.0 + uniform(rng) * 27.0));

            double phase =
                2.0 * PI * (f0 * t - 0.5 * sweep * t * t)
                + phase_offset;

            double tonal = 0.0;

            for (int h = 1; h <= 8; ++h)
                tonal += sin(h * phase) / h;

            double metallic =
                sin(2.0 * PI * (1200.0 + uniform(rng) * 5800.0) * t)
                + 0.5 * sin(
                    2.0 * PI * (2000.0 + uniform(rng) * 7000.0) * t
                );

            double burst = normal(rng);

            result[start + j] += decay *
                (0.5 * tonal + 0.25 * metallic + 0.4 * burst);
        }

        hit_time += 0.12 + uniform(rng) * 0.43;
    }

    normalize_rms(result, 1.0);

    return result;
}

// ------------------------------------------------------------
// Time-shifted logarithmic feature
// ------------------------------------------------------------

vector<double> shifted_log_feature(
    const vector<double>& signal,
    int window,
    int shift
) {
    vector<double> feature(signal.size(), 0.0);

    for (size_t i = 0; i < signal.size(); ++i) {
        double sum = 0.0;
        int count = 0;

        for (int k = 0; k < window; ++k) {
            size_t a = i + k;
            size_t b = i + k + shift;

            if (b >= signal.size())
                break;

            sum += 0.5 * (
                abs(signal[a]) + abs(signal[b])
            );

            ++count;
        }

        if (count > 0)
            feature[i] = log(sum / count + EPS);
    }

    return feature;
}

double feature_error(
    const vector<double>& clean,
    const vector<double>& contaminated
) {
    auto a = shifted_log_feature(clean, 8, 4);
    auto b = shifted_log_feature(contaminated, 8, 4);

    double error = 0.0;

    for (size_t i = 0; i < a.size(); ++i)
        error += abs(a[i] - b[i]);

    return error / a.size();
}

// ------------------------------------------------------------
// Benchmark
// ------------------------------------------------------------

int main(int argc, char** argv) {
    try {
        string input = argc > 1 ? argv[1] : "template.wav";
        string output_dir = argc > 2 ? argv[2] : "benchmark";

        system(("mkdir -p \"" + output_dir + "\"").c_str());

        Audio clean = load_wav(input);

        // Resampling intentionally left out:
        // input should already be 16 kHz mono for this first version.
        if (clean.sample_rate != 16000) {
            cerr << "Please provide a 16 kHz template WAV.\n";
            return 1;
        }

        save_wav(output_dir + "/clean.wav", clean);

        auto percussion = synth_percussion(
            clean.samples.size(),
            clean.sample_rate,
            20261102
        );

        save_wav(
            output_dir + "/synth_percussion.wav",
            Audio{clean.sample_rate, percussion}
        );

        vector<double> sir_levels = {12, 6, 0, -6, -12};

        ofstream json(output_dir + "/results.json");

        json << fixed << setprecision(6);

        json << "{\n";
        json << "  \"input\": \"" << input << "\",\n";
        json << "  \"sample_rate\": " << clean.sample_rate << ",\n";
        json << "  \"samples\": " << clean.samples.size() << ",\n";
        json << "  \"noise_type\": \"synthetic broadband harmonic percussion\",\n";
        json << "  \"results\": [\n";

        bool first = true;

        for (double sir_db : sir_levels) {
            double desired_noise_rms =
                rms(clean.samples) / pow(10.0, sir_db / 20.0);

            vector<double> noise = percussion;

            normalize_rms(noise, desired_noise_rms);

            vector<double> mixture(clean.samples.size());

            for (size_t i = 0; i < mixture.size(); ++i)
                mixture[i] = clean.samples[i] + noise[i];

            Audio contaminated{clean.sample_rate, mixture};

            string filename =
                output_dir + "/contaminated_sir_" +
                to_string(int(sir_db)) + ".wav";

            save_wav(filename, contaminated);

            Metrics m;

            m.sir_db = sir_db;
            m.correlation = correlation(clean.samples, mixture);
            m.si_sdr_db = si_sdr(clean.samples, mixture);
            m.feature_error = feature_error(clean.samples, mixture);

            if (!first)
                json << ",\n";

            first = false;

            json << "    {\n";
            json << "      \"sir_db\": " << m.sir_db << ",\n";
            json << "      \"correlation\": " << m.correlation << ",\n";
            json << "      \"si_sdr_db\": " << m.si_sdr_db << ",\n";
            json << "      \"log_feature_mae\": " << m.feature_error << ",\n";
            json << "      \"contaminated_wav\": \"" << filename << "\"\n";
            json << "    }";
        }

        json << "\n  ],\n";
        json << "  \"note\": \"Contamination metrics only. No source recovery is claimed.\"\n";
        json << "}\n";

        cout << "Benchmark complete.\n";
        cout << "Results: " << output_dir << "/results.json\n";

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
