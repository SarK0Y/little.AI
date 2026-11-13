// voice_music_self_template.cpp
// DB-free stereo/music-structure analysis prototype.
// Requires libsndfile and FFTW3. C++17.
//
// This first-stage tool deliberately does NOT claim to isolate vocals.
// It creates Mid/Side candidates and reports repeated spectral windows
// that can later serve as self-derived musical templates.

#include <sndfile.h>
#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {
constexpr int NFFT = 2048;
constexpr int HOP = 512;
constexpr double PI = 3.14159265358979323846;

struct Audio {
    int samplerate = 0;
    int channels = 0;
    std::vector<double> interleaved;
};

bool read_audio(const std::string& path, Audio& a) {
    SF_INFO info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f) {
        std::cerr << "Cannot open " << path << ": " << sf_strerror(nullptr) << "\n";
        return false;
    }
    if (info.channels < 1 || info.frames < 1) {
        std::cerr << "Invalid/empty audio file.\n";
        sf_close(f);
        return false;
    }
    a.samplerate = info.samplerate;
    a.channels = info.channels;
    a.interleaved.resize(static_cast<size_t>(info.frames) * info.channels);
    const sf_count_t got = sf_readf_double(f, a.interleaved.data(), info.frames);
    sf_close(f);
    if (got != info.frames) {
        a.interleaved.resize(static_cast<size_t>(got) * info.channels);
        std::cerr << "Warning: short read.\n";
    }
    return !a.interleaved.empty();
}

bool write_mono(const std::string& path, const std::vector<double>& samples, int sr) {
    SF_INFO info{};
    info.samplerate = sr;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f) {
        std::cerr << "Cannot write " << path << ": " << sf_strerror(nullptr) << "\n";
        return false;
    }
    std::vector<double> clipped = samples;
    for (double& x : clipped) x = std::clamp(x, -1.0, 1.0);
    const sf_count_t written = sf_writef_double(f, clipped.data(), static_cast<sf_count_t>(clipped.size()));
    sf_close(f);
    return written == static_cast<sf_count_t>(clipped.size());
}

std::vector<double> channel(const Audio& a, int ch) {
    const size_t frames = a.interleaved.size() / static_cast<size_t>(a.channels);
    std::vector<double> out(frames);
    for (size_t i = 0; i < frames; ++i)
        out[i] = a.interleaved[i * static_cast<size_t>(a.channels) + static_cast<size_t>(ch)];
    return out;
}

void mono_from_channels(const Audio& a, std::vector<double>& l, std::vector<double>& r) {
    const size_t frames = a.interleaved.size() / static_cast<size_t>(a.channels);
    l.assign(frames, 0.0);
    r.assign(frames, 0.0);
    if (a.channels == 1) {
        l = channel(a, 0);
        r = l;
        return;
    }
    // For >2 channels, use first two channels as the stereo pair in this prototype.
    l = channel(a, 0);
    r = channel(a, 1);
}

std::vector<double> hann() {
    std::vector<double> w(NFFT);
    for (int i = 0; i < NFFT; ++i)
        w[i] = 0.5 - 0.5 * std::cos(2.0 * PI * i / (NFFT - 1));
    return w;
}

struct SpectralFrames {
    int bins = NFFT / 2 + 1;
    std::vector<std::vector<double>> logmag;
};

SpectralFrames make_spectral_frames(const std::vector<double>& x) {
    SpectralFrames result;
    const auto win = hann();
    std::vector<double> in(NFFT);
    std::vector<fftw_complex> out(result.bins);
    fftw_plan plan = fftw_plan_dft_r2c_1d(NFFT, in.data(), out.data(), FFTW_ESTIMATE);
    if (!plan) return result;

    for (size_t start = 0; start + NFFT <= x.size(); start += HOP) {
        for (int n = 0; n < NFFT; ++n)
            in[n] = x[start + static_cast<size_t>(n)] * win[n];
        fftw_execute(plan);
        std::vector<double> frame(result.bins);
        for (int k = 0; k < result.bins; ++k) {
            const double re = out[k][0], im = out[k][1];
            frame[k] = std::log1p(std::sqrt(re * re + im * im));
        }
        result.logmag.push_back(std::move(frame));
    }
    fftw_destroy_plan(plan);
    return result;
}

double cosine(const std::vector<double>& a, const std::vector<double>& b) {
    double dot = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        aa += a[i] * a[i];
        bb += b[i] * b[i];
    }
    if (aa < 1e-20 || bb < 1e-20) return 0.0;
    return dot / std::sqrt(aa * bb);
}

std::vector<double> average_descriptor(const SpectralFrames& s, size_t first, size_t count) {
    std::vector<double> d(s.bins, 0.0);
    for (size_t i = first; i < first + count; ++i)
        for (int k = 0; k < s.bins; ++k)
            d[k] += s.logmag[i][k];
    for (double& x : d) x /= static_cast<double>(count);
    return d;
}

struct Match {
    size_t a = 0, b = 0;
    double score = 0.0;
};

std::vector<Match> find_repeated_windows(const SpectralFrames& s, int sr,
                                         double window_seconds, double min_separation_seconds) {
    std::vector<Match> matches;
    if (s.logmag.size() < 4) return matches;
    const size_t win_frames = std::max<size_t>(3, static_cast<size_t>(window_seconds * sr / HOP));
    const size_t step = std::max<size_t>(1, win_frames / 3);
    const size_t min_sep = static_cast<size_t>(min_separation_seconds * sr / HOP);

    struct Candidate { size_t start; std::vector<double> desc; };
    std::vector<Candidate> c;
    for (size_t i = 0; i + win_frames < s.logmag.size(); i += step)
        c.push_back({i, average_descriptor(s, i, win_frames)});

    for (size_t i = 0; i < c.size(); ++i) {
        Match best{};
        for (size_t j = i + 1; j < c.size(); ++j) {
            if (c[j].start - c[i].start < min_sep) continue;
            const double sc = cosine(c[i].desc, c[j].desc);
            if (sc > best.score) best = {c[i].start, c[j].start, sc};
        }
        if (best.score > 0.90) matches.push_back(best);
    }
    std::sort(matches.begin(), matches.end(), [](const Match& x, const Match& y) {
        return x.score > y.score;
    });
    // Keep a compact list; nearby overlapping candidate windows are redundant.
    std::vector<Match> filtered;
    for (const auto& m : matches) {
        bool duplicate = false;
        for (const auto& q : filtered) {
            if (std::abs(static_cast<long long>(m.a) - static_cast<long long>(q.a)) < 4 &&
                std::abs(static_cast<long long>(m.b) - static_cast<long long>(q.b)) < 4) {
                duplicate = true; break;
            }
        }
        if (!duplicate) filtered.push_back(m);
        if (filtered.size() >= 50) break;
    }
    return filtered;
}

double rms(const std::vector<double>& x) {
    if (x.empty()) return 0.0;
    double s = 0.0;
    for (double v : x) s += v * v;
    return std::sqrt(s / static_cast<double>(x.size()));
}

void write_report(const std::string& path, int sr, size_t frames,
                  const std::vector<Match>& matches) {
    std::ofstream f(path);
    f << "{\n  \"sample_rate\": " << sr
      << ",\n  \"audio_frames\": " << frames
      << ",\n  \"method\": \"coarse log-magnitude window cosine similarity; candidates require score > 0.90\",\n"
      << "  \"warning\": \"Similarity is not proof of identical accompaniment; vocals and arrangement can also repeat.\",\n"
      << "  \"repeated_windows\": [\n";
    for (size_t i = 0; i < matches.size(); ++i) {
        const auto& m = matches[i];
        const double ta = static_cast<double>(m.a * HOP) / sr;
        const double tb = static_cast<double>(m.b * HOP) / sr;
        f << "    {\"start_a_sec\": " << std::fixed << std::setprecision(3) << ta
          << ", \"start_b_sec\": " << tb
          << ", \"window_sec\":  2.0"
          << ", \"similarity\": " << std::setprecision(5) << m.score << "}";
        if (i + 1 != matches.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
}


// --- Reference-band experiment ------------------------------------------------
// A low conditional entropy rate is a *candidate* property, not proof of music:
// sustained vocals and stationary noise can also be predictable. We therefore
// report it separately from beat-periodicity and cross-band temporal coherence.
struct Biquad { double b0,b1,b2,a1,a2,x1=0,x2=0,y1=0,y2=0; };
Biquad make_bandpass(double fc, double sr, double q) {
    const double w=2.0*PI*fc/sr, alpha=std::sin(w)/(2.0*q), c=std::cos(w);
    const double a0=1.0+alpha;
    return {alpha/a0,0.0,-alpha/a0,-2.0*c/a0,(1.0-alpha)/a0};
}
double tick(Biquad& b,double x) {
    const double y=b.b0*x+b.b1*b.x1+b.b2*b.x2-b.a1*b.y1-b.a2*b.y2;
    b.x2=b.x1;b.x1=x;b.y2=b.y1;b.y1=y;return y;
}
double pearson(const std::vector<double>& a,const std::vector<double>& b) {
    const size_t n=std::min(a.size(),b.size()); if(n<8)return 0.0;
    double ma=0,mb=0;for(size_t i=0;i<n;i++){ma+=a[i];mb+=b[i];}ma/=n;mb/=n;
    double xy=0,xx=0,yy=0;for(size_t i=0;i<n;i++){double x=a[i]-ma,y=b[i]-mb;xy+=x*y;xx+=x*x;yy+=y*y;}
    return (xx>1e-18&&yy>1e-18)?xy/std::sqrt(xx*yy):0.0;
}
// Shannon entropy of the transition distribution between quantized envelope
// states, normalized to [0,1]. This is an entropy-rate proxy, not source entropy.
double transition_entropy(const std::vector<double>& env) {
    constexpr int Q=8; if(env.size()<16)return 1.0;
    double lo=*std::min_element(env.begin(),env.end()),hi=*std::max_element(env.begin(),env.end());
    if(hi-lo<1e-12)return 0.0;
    int counts[Q][Q]{};int row[Q]{};int total=0;
    auto quant=[&](double x){return std::clamp((int)((x-lo)/(hi-lo)*Q),0,Q-1);};
    for(size_t i=1;i<env.size();i++){int a=quant(env[i-1]),b=quant(env[i]);counts[a][b]++;row[a]++;total++;}
    double h=0;
    for(int a=0;a<Q;a++) if(row[a]) {
        double pa=(double)row[a]/total, conditional=0;
        for(int b=0;b<Q;b++)if(counts[a][b]){double p=(double)counts[a][b]/row[a];conditional-=p*std::log2(p);}
        h+=pa*conditional;
    }
    return std::clamp(h/std::log2((double)Q),0.0,1.0);
}
// Beat-like periodicity: strongest normalized envelope autocorrelation in
// roughly 60-240 BPM. It is intentionally not a beat detector.
double beat_periodicity(const std::vector<double>& env,double env_rate) {
    if(env.size()<32)return 0.0;
    double mean=std::accumulate(env.begin(),env.end(),0.0)/env.size();
    double energy=0;for(double x:env)energy+=(x-mean)*(x-mean);if(energy<1e-18)return 0;
    int minlag=std::max(1,(int)(env_rate*0.25)), maxlag=std::min((int)env.size()/2,(int)(env_rate*1.0));
    double best=0;
    for(int lag=minlag;lag<=maxlag;lag++) {double c=0;for(size_t i=lag;i<env.size();i++)c+=(env[i]-mean)*(env[i-lag]-mean);best=std::max(best,c/energy);}
    return std::clamp(best,0.0,1.0);
}
struct RefBand {
    double fc=0, entropy=1, beat=0, coherence=0, score=0, rms=0;
    std::vector<double> env;
};

// Envelope metrics used by the experimental music-mask acceptance test.
// Fading length is measured in envelope frames: from each local peak to the
// first later frame below 10% of that peak (within a bounded search window).
double fading_length(const std::vector<double>& env) {
    if (env.size() < 3) return 0.0;
    double sum = 0.0;
    size_t count = 0;
    const double global_peak = *std::max_element(env.begin(), env.end());
    if (global_peak <= 1e-12) return 0.0;
    const size_t max_decay = std::max<size_t>(2, (size_t)std::round(1.0 * 44100.0 / HOP));
    for (size_t i = 1; i + 1 < env.size(); ++i) {
        if (env[i] < env[i-1] || env[i] <= env[i+1] || env[i] < 0.05 * global_peak)
            continue;
        const double threshold = 0.10 * env[i];
        const size_t end = std::min(env.size()-1, i + max_decay);
        size_t j = i + 1;
        while (j <= end && env[j] > threshold) ++j;
        if (j <= end) { sum += static_cast<double>(j-i); ++count; }
    }
    return count ? sum / count : 0.0;
}

// Convert audio to a smoothed RMS envelope at ENV_HOP spacing.
std::vector<double> make_envelope(const std::vector<double>& x, int hop) {
    std::vector<double> env;
    env.reserve((x.size()+static_cast<size_t>(hop)-1)/static_cast<size_t>(hop));
    for (size_t start=0; start<x.size(); start+=static_cast<size_t>(hop)) {
        const size_t end=std::min(x.size(),start+static_cast<size_t>(hop));
        double e=0.0;
        for(size_t i=start;i<end;i++) e+=x[i]*x[i];
        env.push_back(std::sqrt(e/std::max<size_t>(1,end-start)));
    }
    return env;
}

// Music-model mask: map reference-band envelope to a smooth gain estimate.
// The mask is intentionally applied to the target carrier, not subtracted as
// a phase-unrelated waveform. This is an exploratory suppression heuristic.
std::vector<double> suppress_with_reference_envelope(
        const std::vector<double>& target,
        const std::vector<double>& reference_env,
        int hop, double suppression) {
    std::vector<double> out(target.size());
    if(target.empty() || reference_env.empty()) return target;
    const double peak=*std::max_element(reference_env.begin(),reference_env.end());
    for(size_t i=0;i<target.size();i++) {
        const double pos=(double)i/hop;
        const size_t k=std::min((size_t)pos,reference_env.size()-1);
        const size_t k2=std::min(k+1,reference_env.size()-1);
        const double frac=pos-k;
        const double e=(1.0-frac)*reference_env[k]+frac*reference_env[k2];
        const double music=peak>1e-12?std::clamp(e/peak,0.0,1.0):0.0;
        // Keep a floor so the experiment does not hard-gate transient content.
        const double gain=std::clamp(1.0-suppression*music,0.15,1.0);
        out[i]=target[i]*gain;
    }
    return out;
}

struct MusicOptions {
    double max_band_hz = 10000.0; // -band-bar
    double max_entropy = 0.45;    // -entropy
    double silence_low_hz = -1.0; // -silence-band low-high; excluded range
    double silence_high_hz = -1.0;
    int max_candidates = 12;
    double too_quiet_db = -24.0; // relative to whole-source RMS
    bool no_bursts_enabled = false;
    double no_bursts_ms = 80.0; // peak-centered output window duration
    double no_bursts_db = -45.0; // minimum peak frame level relative to source RMS
};

// Stage 2: apply -no-bursts directly to Mid before any reference-band,
// entropy, gRef, or music-removal analysis. A burst is a contiguous run of
// frames above the DB-relative Mid RMS threshold; keep an MS-length window
// centered on that run's maximum frame.
std::vector<double> apply_no_bursts_to_mid(const std::vector<double>& mid,
                                           int sr, double ms, double db) {
    std::vector<double> out(mid.size(), 0.0);
    if (mid.empty()) return out;
    constexpr size_t HOP = 512;
    const size_t frames = (mid.size() + HOP - 1) / HOP;
    std::vector<double> frame_rms(frames, 0.0);
    for (size_t f = 0; f < frames; ++f) {
        const size_t a = f * HOP, b = std::min(mid.size(), a + HOP);
        double e = 0.0;
        for (size_t i = a; i < b; ++i) e += mid[i] * mid[i];
        frame_rms[f] = b > a ? std::sqrt(e / static_cast<double>(b-a)) : 0.0;
    }
    const double threshold = rms(mid) * std::pow(10.0, -std::abs(db) / 20.0);
    const size_t half = static_cast<size_t>(std::llround(std::max(0.0, ms) * sr / 2000.0));
    size_t f = 0;
    while (f < frames) {
        if (frame_rms[f] < threshold) { ++f; continue; }
        const size_t run_begin = f;
        while (f < frames && frame_rms[f] >= threshold) ++f;
        const size_t run_end = f;
        size_t peak = run_begin;
        for (size_t j = run_begin + 1; j < run_end; ++j)
            if (frame_rms[j] > frame_rms[peak]) peak = j;
        const size_t peak_sample = std::min(mid.size()-1, peak * HOP + HOP/2);
        const size_t a = peak_sample > half ? peak_sample - half : 0;
        const size_t b = std::min(mid.size(), peak_sample + half + 1);
        for (size_t i = a; i < b; ++i) out[i] = mid[i];
    }
    return out;
}

void analyze_reference_bands(const std::vector<double>& mid,const std::vector<double>& side,
                             int sr,const std::string& prefix,const MusicOptions& opts) {
    constexpr int BANDS=48, ENV_HOP=512, TOP=12;
    const double minHz=80.0,maxHz=std::min(10000.0,sr*0.45);
    const double q=8.65; // approx 1/6-octave bandwidth
    const double envRate=(double)sr/ENV_HOP;
    std::vector<RefBand> bands(BANDS);
    std::vector<std::vector<double>> filtered(BANDS);
    std::vector<std::vector<double>> filtered_mid(BANDS);
    for(int b=0;b<BANDS;b++) {
        double t=(double)b/(BANDS-1); bands[b].fc=minHz*std::pow(maxHz/minHz,t);
        filtered[b].resize(mid.size());
        filtered_mid[b].resize(mid.size());
        Biquad bm=make_bandpass(bands[b].fc,sr,q), bs=make_bandpass(bands[b].fc,sr,q);
        Biquad bm_only=make_bandpass(bands[b].fc,sr,q);
        std::vector<double> env; env.reserve((mid.size()+ENV_HOP-1)/ENV_HOP);
        double e=0;for(size_t i=0;i<mid.size();i++) {
            const double mid_band = tick(bm_only, mid[i]);
            double y=0.5*(tick(bm,mid[i])+tick(bs,side[i]));
            filtered[b][i]=y;
            filtered_mid[b][i]=mid_band;
            e+=mid_band*mid_band; // reference envelope is from the Mid reference band
            if((i+1)%ENV_HOP==0) { env.push_back(std::sqrt(e/ENV_HOP)); e=0; }
        }
        if(mid.size()%ENV_HOP && !mid.empty())
            env.push_back(std::sqrt(e/(mid.size()%ENV_HOP)));
        bands[b].env=std::move(env);
        bands[b].rms=std::sqrt(std::inner_product(filtered_mid[b].begin(),filtered_mid[b].end(),
                                                   filtered_mid[b].begin(),0.0)/std::max<size_t>(1,mid.size()));
        bands[b].entropy=transition_entropy(bands[b].env); bands[b].beat=beat_periodicity(bands[b].env,envRate);
    }
    // Coherence with the remaining filterbank: use best non-neighbor envelope
    // correlation, to avoid simply rewarding adjacent overlapping filters.
    for(int b=0;b<BANDS;b++) {
        double best=0;for(int j=0;j<BANDS;j++)if(std::abs(j-b)>=2)best=std::max(best,std::abs(pearson(bands[b].env,bands[j].env)));
        bands[b].coherence=best;
        // Separate metrics; score is only a ranking heuristic, not a probability.
        bands[b].score=(1.0-bands[b].entropy)*(0.50+0.25*bands[b].beat+0.25*bands[b].coherence);
    }
    std::vector<int> order(BANDS);std::iota(order.begin(),order.end(),0);
    std::sort(order.begin(),order.end(),[&](int a,int b){return bands[a].score>bands[b].score;});
    std::ofstream csv(prefix+"_reference_band_scores.csv");
    csv<<"rank,band,center_hz,rms,transition_entropy,beat_periodicity,cross_band_coherence,heuristic_score\n";
    for(int rank=0;rank<BANDS;rank++){int b=order[rank];auto& z=bands[b];csv<<rank+1<<","<<b<<","<<z.fc<<","<<z.rms<<","<<z.entropy<<","<<z.beat<<","<<z.coherence<<","<<z.score<<"\n";}
    // Select music candidates with the same heuristic score used by
    // voice_music_self_template, instead of entropy alone.
    std::vector<int> music_candidates;
    for (int b : order) {
        const double fc = bands[b].fc;
        if (fc > opts.max_band_hz) continue;
        if (opts.silence_low_hz >= 0.0 && fc >= opts.silence_low_hz && fc <= opts.silence_high_hz) continue;
        if (bands[b].entropy > opts.max_entropy) continue;
        if (bands[b].rms < 1e-7) continue; // avoid effectively silent filters
        music_candidates.push_back(b);
        if ((int)music_candidates.size() >= opts.max_candidates) break;
    }

    // Reconstruct the music component from the enveloped reference bands: each
    // selected band is modulated by its own RMS-normalized envelope, so the
    // average level is unchanged but the rhythm the envelope files expose is
    // what gets subtracted. Normalize by sqrt(N) to avoid level inflation.
    std::vector<double> music(mid.size(), 0.0);
    for (int b : music_candidates) {
        const auto& env = bands[b].env;
        if (env.empty()) continue;
        double env_sq = 0.0;
        for (double e : env) env_sq += e * e;
        const double env_rms = std::sqrt(env_sq / std::max<size_t>(1, env.size()));
        for (size_t i = 0; i < music.size(); ++i) {
            const size_t k = std::min(i / ENV_HOP, env.size() - 1);
            const double gain = env_rms > 1e-12 ? env[k] / env_rms : 0.0;
            music[i] += filtered_mid[b][i] * gain;
        }
    }
    if (!music_candidates.empty()) {
        const double norm = 1.0 / std::sqrt((double)music_candidates.size());
        for (double& x : music) x *= norm;
    }

    // gRef is the summed envelope of the selected reference bands. It is the
    // single time-aligned activity mask used for both filler and voice zones.
    const size_t zone_frames = (mid.size() + ENV_HOP - 1) / ENV_HOP;
    std::vector<double> gRef(zone_frames, 0.0);
    if (!music_candidates.empty()) {
        for (int b : music_candidates) {
            const auto& env = bands[b].env; // envelope of this Mid reference band
            for (size_t k = 0; k < std::min(zone_frames, env.size()); ++k)
                gRef[k] += env[k];
        }
        const double scale = 1.0 / std::sqrt(static_cast<double>(music_candidates.size()));
        for (double& v : gRef) v *= scale;
    }

    // -too-quiet is the ONLY quiet/non-silent decision. It is relative to
    // source RMS; all region boundaries are exactly the gRef envelope frames.
    double source_energy = 0.0;
    for (double x : mid) source_energy += x * x;
    const double source_rms = std::sqrt(source_energy / std::max<size_t>(1, mid.size()));
    const double quiet_threshold = source_rms * std::pow(10.0, opts.too_quiet_db / 20.0);

    std::vector<int> zone_class(zone_frames, 0); // 0=voice candidate, 1=music filler
    std::vector<double> voice_gate(zone_frames, 1.0);
    for (size_t k = 0; k < zone_frames; ++k) {
        const bool silent_ref = gRef[k] <= quiet_threshold;
        zone_class[k] = silent_ref ? 0 : 1;
        voice_gate[k] = silent_ref ? 1.0 : 0.0;
    }

    // Remove the enveloped-reference music estimate only in non-silent gRef
    // regions (music fillers). In silent gRef regions, leave the Mid signal
    // untouched so candidate width follows the silent reference timecodes.
    constexpr size_t ADAPT_HOP = 2048;
    constexpr double ALPHA_MAX = 1.25;
    std::vector<double> residual(mid.size()), alpha_track;
    double alpha_prev = 0.0;
    for (size_t start = 0; start < mid.size(); start += ADAPT_HOP) {
        const size_t end = std::min(mid.size(), start + ADAPT_HOP);
        double dot = 0.0, energy = 0.0;
        for (size_t i = start; i < end; ++i) {
            const size_t frame = std::min(i / ENV_HOP, zone_frames - 1);
            if (zone_class[frame] != 1) continue; // fit music only in filler
            dot += mid[i] * music[i];
            energy += music[i] * music[i];
        }
        double alpha = energy > 1e-12 ? std::clamp(dot / energy, 0.0, ALPHA_MAX) : 0.0;
        alpha = 0.65 * alpha_prev + 0.35 * alpha; // temporal smoothing
        alpha_prev = alpha; alpha_track.push_back(alpha);
        for (size_t i = start; i < end; ++i) {
            const size_t frame = std::min(i / ENV_HOP, zone_frames - 1);
            residual[i] = zone_class[frame] == 1 ? mid[i] - alpha * music[i] : mid[i];
        }
    }

    // Build a compact voice-only output: retain only samples belonging to
    // gRef-quiet voice-candidate frames AND above the -too-quiet amplitude
    // threshold. Concatenate active runs; do not synthesize silence or preserve
    // original timeline gaps. This stage receives the already burst-gated Mid.
    std::vector<double> voice_zone_samples;
    voice_zone_samples.reserve(mid.size());
    const double sample_gate = quiet_threshold;
    for (size_t i = 0; i < mid.size(); ++i) {
        const size_t frame = std::min(i / ENV_HOP, zone_frames - 1);
        if (zone_class[frame] == 0 && std::abs(residual[i]) > sample_gate)
            voice_zone_samples.push_back(residual[i]);
    }

    write_mono(prefix + "_music_reconstructed.wav", music, sr);
    write_mono(prefix + "_music_cut_residual.wav", residual, sr);
    write_mono(prefix + "_voice_detected_zones.wav", voice_zone_samples, sr);
    std::ofstream zones(prefix + "_zones.csv");
    zones << "frame,start_ms,end_ms,gRef,zone,voice_gate\n";
    for (size_t k = 0; k < zone_frames; ++k) {
        const double start_ms = 1000.0 * k * ENV_HOP / sr;
        const double end_ms = 1000.0 * std::min(mid.size(), (k + 1) * ENV_HOP) / sr;
        const char* label = zone_class[k] == 0 ? "voice_candidate" : "music_filler";
        zones << k << "," << start_ms << "," << end_ms << "," << gRef[k]
              << "," << label << "," << voice_gate[k] << "\n";
    }
    // Export gRef as a normalized sample-aligned envelope WAV for inspection.
    std::vector<double> gRef_audio(mid.size(), 0.0);
    const double gRef_peak = gRef.empty() ? 0.0 : *std::max_element(gRef.begin(), gRef.end());
    for (size_t i = 0; i < gRef_audio.size(); ++i) {
        const size_t frame = std::min(i / ENV_HOP, gRef.size() - 1);
        gRef_audio[i] = gRef_peak > 1e-12 ? gRef[frame] / gRef_peak : 0.0;
    }
    write_mono(prefix + "_gRef_envelope.wav", gRef_audio, sr);

    std::ofstream selected(prefix + "_music_candidates.csv");
    selected << "rank,band,center_hz,entropy,beat_periodicity,cross_band_coherence,heuristic_score,rms,adaptive_alpha\n";
    for (size_t k = 0; k < music_candidates.size(); ++k) {
        const int b = music_candidates[k];
        const double alpha = alpha_track.empty() ? 0.0 :
            std::accumulate(alpha_track.begin(), alpha_track.end(), 0.0) / alpha_track.size();
        selected << k+1 << "," << b << "," << bands[b].fc << "," << bands[b].entropy
                << "," << bands[b].beat << "," << bands[b].coherence << "," << bands[b].score
                << "," << bands[b].rms << "," << alpha << "\n";
    }
    std::cout << "Music candidates selected: " << music_candidates.size() << "\n"
              << "Music reconstruction: " << prefix << "_music_reconstructed.wav\n"
              << "Adaptive residual: " << prefix << "_music_cut_residual.wav\n"
              << "Compact voice-only zones: " << prefix << "_voice_detected_zones.wav\n"
              << "Zone report: " << prefix << "_zones.csv\n";
    // Export top candidate bands as filtered audio and an audible envelope track.
    // Envelope WAV is amplitude-modulated 220 Hz, useful for hearing its rhythm.
    for(int rank=0;rank<std::min(TOP,BANDS);rank++) {
        int b=order[rank];const std::string stem=prefix+"_ref_"+std::to_string(rank+1)+"_"+std::to_string((int)std::round(bands[b].fc))+"Hz";
        write_mono(stem+".wav",filtered[b],sr);
        std::vector<double> pulse(mid.size(),0.0);const auto& env=bands[b].env;
        double env_peak=env.empty()?0.0:*std::max_element(env.begin(),env.end());
        for(size_t i=0;i<pulse.size();i++){double pos=(double)i/ENV_HOP;size_t k=env.empty()?0:std::min((size_t)pos,env.size()-1);double v=(env_peak>1e-12&&!env.empty())?env[k]/env_peak:0.0;pulse[i]=0.25*std::sin(2.0*PI*220.0*i/sr)*v;}
        write_mono(stem+"_envelope.wav",pulse,sr);
    }
    // Experimental acceptance check: V = original target band, Y = masked
    // output candidate, G = reference-driven generated music activity.
    // The reference's envelope is applied to the target carrier as a gain
    // mask; no phase-unrelated waveform subtraction is performed.
    const int ref_index=order.front();
    const auto& ref_env=bands[ref_index].env;
    std::ofstream eval(prefix+"_music_mask_acceptance.csv");
    eval<<"reference_band,target_band,center_hz,H_V,H_Y,F_V_frames,F_Y_frames,"
           "corr_G_V,corr_G_Y,accepted\n";
    for(int target=0;target<BANDS;target++) {
        const auto& V=filtered[target];
        auto Y=suppress_with_reference_envelope(V,ref_env,ENV_HOP,0.70);
        const auto envV=make_envelope(V,ENV_HOP);
        const auto envY=make_envelope(Y,ENV_HOP);
        std::vector<double> G(envV.size(),0.0);
        if(!ref_env.empty()) {
            const double peak=*std::max_element(ref_env.begin(),ref_env.end());
            for(size_t k=0;k<G.size();k++) {
                const size_t rk=std::min(k,ref_env.size()-1);
                G[k]=(peak>1e-12?ref_env[rk]/peak:0.0)*envV[k];
            }
        }
        const double HV=transition_entropy(envV), HY=transition_entropy(envY);
        const double FV=fading_length(envV), FY=fading_length(envY);
        const double corrGV=std::abs(pearson(G,envV));
        const double corrGY=std::abs(pearson(G,envY));
        const bool accepted=(HY>=HV)&&(FY>=FV)&&(corrGV>corrGY);
        eval<<ref_index<<","<<target<<","<<bands[target].fc<<","
            <<HV<<","<<HY<<","<<FV<<","<<FY<<","
            <<corrGV<<","<<corrGY<<","<<(accepted?1:0)<<"\n";
        const std::string stem=prefix+"_music_mask_ref_"+std::to_string(ref_index)
            +"_target_"+std::to_string(target);
        write_mono(stem+"_Y.wav",Y,sr);
    }
    std::cout<<"Reference-band ranking: "<<prefix<<"_reference_band_scores.csv\n"
             <<"Exported top "<<std::min(TOP,BANDS)<<" filtered candidate bands + envelope WAVs.\n"
             <<"Music-mask acceptance: "<<prefix<<"_music_mask_acceptance.csv\n"
             <<"Scores and acceptance are experimental, NOT proof of source separation.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " input_stereo.wav [output_prefix] [report.json]"
                  << " [-band-bar HZ] [-entropy MAX] [-silence-band LOW-HIGH]"
                  << " [-too-quiet DB] [-no-bursts MS DB]\n";
        return 2;
    }
    const std::string input = argv[1];
    std::string prefix = "stereo", report;
    int argi = 2;
    if (argi < argc && argv[argi][0] != '-') prefix = argv[argi++];
    report = prefix + "_repeats.json";
    if (argi < argc && argv[argi][0] != '-') report = argv[argi++];
    MusicOptions opts;
    for (; argi < argc; ++argi) {
        const std::string flag = argv[argi];
        if (flag == "-band-bar" && argi + 1 < argc) opts.max_band_hz = std::stod(argv[++argi]);
        else if (flag == "-entropy" && argi + 1 < argc) opts.max_entropy = std::stod(argv[++argi]);
        else if (flag == "-too-quiet" && argi + 1 < argc) opts.too_quiet_db = std::stod(argv[++argi]);
        else if (flag == "-no-bursts" && argi + 2 < argc) {
            opts.no_bursts_enabled = true;
            opts.no_bursts_ms = std::stod(argv[++argi]);
            opts.no_bursts_db = std::stod(argv[++argi]);
        }
        else if (flag == "-silence-band" && argi + 1 < argc) {
            std::string range = argv[++argi];
            auto sep = range.find('-'); if (sep == std::string::npos) sep = range.find(':');
            if (sep == std::string::npos) { std::cerr << "-silence-band expects LOW-HIGH (Hz)\n"; return 2; }
            opts.silence_low_hz = std::stod(range.substr(0, sep));
            opts.silence_high_hz = std::stod(range.substr(sep + 1));
            if (opts.silence_low_hz > opts.silence_high_hz) std::swap(opts.silence_low_hz, opts.silence_high_hz);
        } else { std::cerr << "Unknown/incomplete option: " << flag << "\n"; return 2; }
    }
    if (opts.max_band_hz <= 0 || opts.max_entropy < 0 || opts.max_entropy > 1 ||
        opts.too_quiet_db > 0 || !std::isfinite(opts.no_bursts_ms) ||
        opts.no_bursts_ms < 0 || !std::isfinite(opts.no_bursts_db)) {
        std::cerr << "-band-bar must be >0, -entropy in [0,1], -too-quiet must be <=0 dB, "
                     "and -no-bursts requires finite MS >=0 and finite DB (any sign).\n";
        return 2;
    }

    Audio a;
    if (!read_audio(input, a)) return 1;
    if (a.channels < 2) {
        std::cerr << "Input has " << a.channels << " channel(s). Stereo analysis needs at least 2.\n";
        return 1;
    }

    std::vector<double> l, r;
    mono_from_channels(a, l, r);
    const size_t n = std::min(l.size(), r.size());
    l.resize(n); r.resize(n);

    std::vector<double> mid(n), side(n);
    for (size_t i = 0; i < n; ++i) {
        mid[i] = 0.5 * (l[i] + r[i]);
        side[i] = 0.5 * (l[i] - r[i]);
    }

    if (!write_mono(prefix + "_mid.wav", mid, a.samplerate) ||
        !write_mono(prefix + "_side.wav", side, a.samplerate)) {
        std::cerr << "Failed writing Mid/Side files.\n";
        return 1;
    }

    // Stage 1 creates Mid/Side. Stage 2 is optional: when -no-bursts is
    // present, gate Mid and apply the same retained sample mask to Side.
    // Without the option, pass the original Mid and Side through unchanged.
    const std::vector<double> mid_stage2 = opts.no_bursts_enabled
        ? apply_no_bursts_to_mid(mid, a.samplerate, opts.no_bursts_ms, opts.no_bursts_db)
        : mid;
    std::vector<double> side_stage2 = side;
    if (opts.no_bursts_enabled) {
        for (size_t i = 0; i < side.size(); ++i)
            if (mid_stage2[i] == 0.0) side_stage2[i] = 0.0;
        if (!write_mono(prefix + "_mid_no_bursts.wav", mid_stage2, a.samplerate) ||
            !write_mono(prefix + "_side_no_bursts.wav", side_stage2, a.samplerate)) {
            std::cerr << "Failed writing stage-2 Mid/Side (-no-bursts).\n";
            return 1;
        }
    }
    analyze_reference_bands(mid_stage2, side_stage2, a.samplerate, prefix, opts);

    // A first-pass repeat scan on stage-2 Mid; Side is often more useful for detecting
    // panned musical elements. We scan both and combine their reports.
    const auto mid_spec = make_spectral_frames(mid_stage2);
    const auto side_spec = make_spectral_frames(side_stage2);
    const auto mid_matches = find_repeated_windows(mid_spec, a.samplerate, 2.0, 4.0);
    const auto side_matches = find_repeated_windows(side_spec, a.samplerate, 2.0, 4.0);

    std::vector<Match> combined = mid_matches;
    combined.insert(combined.end(), side_matches.begin(), side_matches.end());
    std::sort(combined.begin(), combined.end(), [](const Match& x, const Match& y) {
        return x.score > y.score;
    });
    if (combined.size() > 100) combined.resize(100);
    write_report(report, a.samplerate, n, combined);

    std::cout << "Input: " << input << "\n"
              << "Sample rate: " << a.samplerate << " Hz\n"
              << "Duration: " << static_cast<double>(n) / a.samplerate << " s\n"
              << "L RMS: " << rms(l) << " | R RMS: " << rms(r) << "\n"
              << "Mid RMS: " << rms(mid)
              << (opts.no_bursts_enabled ? " | Stage-2 Mid RMS: " : " | Stage-2 bypassed; using original Mid")
              << (opts.no_bursts_enabled ? std::to_string(rms(mid_stage2)) : std::string())
              << " | Side RMS: " << rms(side) << "\n"
              << "Wrote: " << prefix << "_mid.wav, " << prefix << "_side.wav"
              << (opts.no_bursts_enabled ? ", " + prefix + "_mid_no_bursts.wav, " + prefix + "_side_no_bursts.wav" : std::string())
              << "\nRepeated-window report: " << report << "\n"
              << "Note: repetition matches are coarse candidates, not source-separated stems.\n";
    return 0;
}
