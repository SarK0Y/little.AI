/*
 * Template-powered voice reconstruction.
 *
 * Source audio is used only for frame timing, pitch/vibrato estimation and
 * coarse spectral evidence. The output is synthesized from LPC/formant frames
 * learned from a clean, isolated voice template. No source waveform samples
 * are mixed into the output.
 *
 * Input: PCM16 mono 16 kHz WAV.
 */
#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

static constexpr int SR=16000, FFT=512, HOP=160, WIN=400, LPC_ORDER=16;
static constexpr int FEATURE_BANDS=18;
static constexpr float PI=3.14159265358979323846f;

struct Audio { std::vector<float> x; };
struct FrameModel {
    float feat[FEATURE_BANDS]{};
    float a[LPC_ORDER+1]{};
    float reflection[LPC_ORDER+1]{};
    float residual=0.0f, rms=0.0f, probability=0.0f;
    bool voiced=false;
};

static bool read16(FILE* f, unsigned short& v) {
    unsigned char b[2]; if(std::fread(b,1,2,f)!=2) return false;
    v=(unsigned short)(b[0]|(b[1]<<8)); return true;
}
static bool read32(FILE* f, unsigned int& v) {
    unsigned char b[4]; if(std::fread(b,1,4,f)!=4) return false;
    v=(unsigned int)b[0]|((unsigned int)b[1]<<8)|((unsigned int)b[2]<<16)|((unsigned int)b[3]<<24); return true;
}
static bool load_wav(const char* path, Audio& a) {
    FILE* f=std::fopen(path,"rb"); if(!f) { std::fprintf(stderr,"cannot open %s\n",path); return false; }
    char id[4]; unsigned int z;
    if(std::fread(id,1,4,f)!=4 || std::memcmp(id,"RIFF",4) || !read32(f,z) ||
       std::fread(id,1,4,f)!=4 || std::memcmp(id,"WAVE",4)) { std::fclose(f); std::fprintf(stderr,"invalid WAV: %s\n",path); return false; }
    unsigned short fmt=0,ch=0,bits=0; unsigned int rate=0,data_size=0; bool hf=false,hd=false;
    while(!hd) {
        if(std::fread(id,1,4,f)!=4 || !read32(f,z)) break;
        long start=std::ftell(f);
        if(!std::memcmp(id,"fmt ",4)) {
            unsigned int br; unsigned short align;
            if(!read16(f,fmt)||!read16(f,ch)||!read32(f,rate)||!read32(f,br)||!read16(f,align)||!read16(f,bits)) break;
            hf=true; std::fseek(f,start+(long)z+(z&1u),SEEK_SET);
        } else if(!std::memcmp(id,"data",4)) { data_size=z; hd=true; }
        else std::fseek(f,start+(long)z+(z&1u),SEEK_SET);
    }
    if(!hf||!hd||fmt!=1||ch!=1||bits!=16||rate!=SR||data_size<2) {
        std::fclose(f); std::fprintf(stderr,"%s must be PCM16 mono 16000 Hz WAV\n",path); return false;
    }
    a.x.resize(data_size/2);
    for(size_t i=0;i<a.x.size();++i) { unsigned short u; if(!read16(f,u)) { std::fclose(f); return false; } a.x[i]=(short)u/32768.0f; }
    std::fclose(f); return true;
}
static bool write_wav(const char* path,const std::vector<float>& x) {
    FILE* f=std::fopen(path,"wb"); if(!f) return false;
    unsigned int data=(unsigned int)(x.size()*2), riff=data+36;
    std::fwrite("RIFF",1,4,f); std::fwrite(&riff,4,1,f); std::fwrite("WAVEfmt ",1,8,f);
    unsigned int fmt_size=16,rate=SR,br=SR*2; unsigned short fmt=1,ch=1,align=2,bits=16;
    std::fwrite(&fmt_size,4,1,f); std::fwrite(&fmt,2,1,f); std::fwrite(&ch,2,1,f);
    std::fwrite(&rate,4,1,f); std::fwrite(&br,4,1,f); std::fwrite(&align,2,1,f); std::fwrite(&bits,2,1,f);
    std::fwrite("data",1,4,f); std::fwrite(&data,4,1,f);
    for(float v:x) { float c=std::max(-0.999969f,std::min(0.999969f,v)); short q=(short)std::lrint(c*32767.0f); std::fwrite(&q,2,1,f); }
    std::fclose(f); return true;
}
static void fft(float* re,float* im) {
    for(int i=1,j=0;i<FFT;i++) { int bit=FFT>>1; for(;j&bit;bit>>=1)j^=bit; j^=bit; if(i<j){std::swap(re[i],re[j]);std::swap(im[i],im[j]);} }
    for(int len=2;len<=FFT;len<<=1) {
        float ang=-2.0f*PI/len, wr0=std::cos(ang),wi0=std::sin(ang);
        for(int i=0;i<FFT;i+=len) { float wr=1,wi=0; for(int j=0;j<len/2;j++) {
            int u=i+j,v=u+len/2; float tr=wr*re[v]-wi*im[v],ti=wr*im[v]+wi*re[v];
            re[v]=re[u]-tr; im[v]=im[u]-ti; re[u]+=tr; im[u]+=ti;
            float nw=wr*wr0-wi*wi0; wi=wr*wi0+wi*wr0; wr=nw;
        }}
    }
}
static void spectrum_features(const float* x,float* feat) {
    float re[FFT]={},im[FFT]={};
    for(int n=0;n<WIN;n++) { float w=0.54f-0.46f*std::cos(2*PI*n/(WIN-1)); re[n]=(x[n]-0.97f*(n?x[n-1]:0))*w; }
    fft(re,im);
    const float edges[FEATURE_BANDS+1]={80,120,170,230,300,390,500,630,780,960,1170,1420,1710,2050,2450,2920,3500,4300,7000};
    for(int b=0;b<FEATURE_BANDS;b++) {
        int lo=std::max(1,(int)std::floor(edges[b]*FFT/SR)),hi=std::min(FFT/2,(int)std::ceil(edges[b+1]*FFT/SR));
        double peak=1e-12;
        for(int k=lo;k<=hi;k++) {
            double power=(double)re[k]*re[k]+(double)im[k]*im[k];
            if(power>peak) peak=power;
        }
        // The strongest local peak is the band's seed-frequency evidence.
        feat[b]=(float)std::log(peak);
    }
    float mean=0; for(float v: std::vector<float>(feat,feat+FEATURE_BANDS)) mean+=v;
    mean/=FEATURE_BANDS; for(int b=0;b<FEATURE_BANDS;b++) feat[b]-=mean;
}
static float lpc_analyze(const float* x,float* a,float* reflection) {
    // Analyse the pre-emphasized signal. Reflection coefficients are retained
    // separately so neighbouring template frames can be interpolated without
    // destabilising the all-pole synthesis filter.
    float y[WIN];
    for(int n=0;n<WIN;n++) y[n]=x[n]-0.97f*(n?x[n-1]:0.0f);
    double r[LPC_ORDER+1]={};
    for(int lag=0;lag<=LPC_ORDER;lag++)
        for(int n=lag;n<WIN;n++) r[lag]+=(double)y[n]*y[n-lag];

    float prev[LPC_ORDER+1]={};
    a[0]=1.0f;
    prev[0]=1.0f;
    double err=std::max(r[0],1e-12);
    for(int i=1;i<=LPC_ORDER;i++) {
        double acc=r[i];
        for(int j=1;j<i;j++) acc+=(double)prev[j]*r[i-j];
        double k=-acc/err;
        // A little margin from |k|=1 prevents extremely sharp, fragile poles.
        k=std::max(-0.88,std::min(0.88,k));
        reflection[i]=(float)k;
        float next[LPC_ORDER+1]={};
        next[0]=1.0f;
        for(int j=1;j<i;j++) next[j]=prev[j]+(float)k*prev[i-j];
        next[i]=(float)k;
        for(int j=0;j<=i;j++) prev[j]=next[j];
        err*=1.0-k*k;
        err=std::max(err,1e-12);
    }
    for(int i=0;i<=LPC_ORDER;i++) a[i]=prev[i];
    return (float)std::sqrt(err/WIN);
}

static void lpc_from_reflection(const float* reflection,float* a) {
    float prev[LPC_ORDER+1]={};
    prev[0]=1.0f;
    for(int i=1;i<=LPC_ORDER;i++) {
        float k=std::max(-0.88f,std::min(0.88f,reflection[i]));
        float next[LPC_ORDER+1]={};
        next[0]=1.0f;
        for(int j=1;j<i;j++) next[j]=prev[j]+k*prev[i-j];
        next[i]=k;
        for(int j=0;j<=i;j++) prev[j]=next[j];
    }
    for(int i=0;i<=LPC_ORDER;i++) a[i]=prev[i];
}
static float estimate_pitch(const float* x,float& confidence) {
    // Autocorrelation pitch tracker over the singer range (75-500 Hz).
    double energy=0; for(int n=0;n<WIN;n++) energy+=(double)x[n]*x[n];
    if(energy<1e-7) { confidence=0; return 0; }
    int minlag=SR/500,maxlag=SR/75,best=minlag; double bestc=-1;
    for(int lag=minlag;lag<=maxlag;lag++) {
        double c=0,e1=0,e2=0;
        for(int n=lag;n<WIN;n++) { double u=x[n],v=x[n-lag]; c+=u*v;e1+=u*u;e2+=v*v; }
        double d=std::sqrt(e1*e2)+1e-15; c/=d;
        if(c>bestc) {bestc=c;best=lag;}
    }
    confidence=(float)std::max(0.0,std::min(1.0,bestc));
    if(confidence<0.22f) return 0;
    return (float)SR/best;
}
static void get_frame(const Audio& a,int center,float* frame) {
    int start=center-WIN/2;
    for(int n=0;n<WIN;n++) { int i=start+n; frame[n]=(i>=0&&i<(int)a.x.size())?a.x[i]:0.0f; }
}
static float distance(const float* a,const float* b) {
    double d=0; for(int i=0;i<FEATURE_BANDS;i++){double v=a[i]-b[i];d+=v*v;}
    return (float)(d/FEATURE_BANDS);
}
static bool build_template(const Audio& t,std::vector<FrameModel>& alphabet) {
    float frame[WIN],feat[FEATURE_BANDS]; int voiced=0;
    for(int center=WIN/2;center<(int)t.x.size();center+=HOP) {
        get_frame(t,center,frame); FrameModel m;
        spectrum_features(frame,feat); std::memcpy(m.feat,feat,sizeof(feat));
        m.residual=lpc_analyze(frame,m.a,m.reflection);
        double rms=0;for(float v:frame)rms+=(double)v*v;m.rms=(float)std::sqrt(rms/WIN);
        float conf=0; m.voiced=estimate_pitch(frame,conf)>0 && conf>0.30f;
        if(m.rms>0.003f) { alphabet.push_back(m); if(m.voiced)voiced++; }
    }
    if(alphabet.size()<4) {std::fprintf(stderr,"template contains too little usable voice\n");return false;}
    // Empirical occurrence probability of each coarse spectral/formant
    // signature. Quantization groups near-identical vowel/consonant frames.
    std::map<std::array<int,FEATURE_BANDS>,int> histogram;
    for(const auto& m:alphabet) {
        std::array<int,FEATURE_BANDS> key{};
        for(int b=0;b<FEATURE_BANDS;b++) key[b]=(int)std::lrint(m.feat[b]/0.85f);
        histogram[key]++;
    }
    for(auto& m:alphabet) {
        std::array<int,FEATURE_BANDS> key{};
        for(int b=0;b<FEATURE_BANDS;b++) key[b]=(int)std::lrint(m.feat[b]/0.85f);
        m.probability=(float)histogram[key]/(float)alphabet.size();
    }
    std::printf("template alphabet: %zu frames, %d voiced frames, %d LPC/formant coefficients per frame\n",alphabet.size(),voiced,LPC_ORDER);
    return true;
}
static bool reconstruct(const Audio& source,const std::vector<FrameModel>& alphabet,const char* output) {
    const int frames=((int)source.x.size()+HOP-1)/HOP;
    std::vector<float> pitch(frames),conf(frames),rms(frames),features((size_t)frames*FEATURE_BANDS);
    float frame[WIN];
    for(int f=0;f<frames;f++) {
        get_frame(source,f*HOP,frame);
        pitch[f]=estimate_pitch(frame,conf[f]);
        double e=0.0;
        for(float v:frame)e+=(double)v*v;
        rms[f]=(float)std::sqrt(e/WIN);
        spectrum_features(frame,&features[(size_t)f*FEATURE_BANDS]);
    }

    // Bridge only one-frame pitch gaps; do not smear note changes or vibrato.
    for(int f=1;f<frames-1;f++)
        if(pitch[f]==0.0f && pitch[f-1]>0.0f && pitch[f+1]>0.0f)
            pitch[f]=0.5f*(pitch[f-1]+pitch[f+1]);

    std::vector<int> chosen(frames,0);
    int previous=-1;
    for(int f=0;f<frames;f++) {
        float best=1e30f;
        int bi=0;
        for(int j=0;j<(int)alphabet.size();j++) {
            float d=distance(&features[(size_t)f*FEATURE_BANDS],alphabet[j].feat);
            float cost=d+0.10f*(-std::log(alphabet[j].probability+1e-9f));
            if(previous>=0 && j!=previous) cost+=0.025f;
            if(cost<best){best=cost;bi=j;}
        }
        chosen[f]=bi;
        previous=bi;
    }

    std::vector<float> out(source.x.size(),0.0f);
    float filter_state[LPC_ORDER+1]={};
    float deemphasis_state=0.0f;
    float phase=0.0f;
    unsigned int rng=0x6d2b79f5u;
    double source_rms_sum=0.0;
    for(float v:rms) source_rms_sum+=v;
    const float source_rms_mean=(float)(source_rms_sum/std::max(1,frames));
    double template_rms_sum=0.0;
    for(const auto& m:alphabet) template_rms_sum+=m.rms;
    const float template_rms=(float)(template_rms_sum/alphabet.size());

    // Stable, frame-wise LPC interpolation: interpolate reflection coefficients,
    // then rebuild a stable predictor. Never interpolate raw LPC poles directly.
    for(int f=0;f<frames;f++) {
        const int next=std::min(frames-1,f+1);
        const FrameModel& m=alphabet[chosen[f]];
        const FrameModel& mn=alphabet[chosen[next]];
        float reflection[LPC_ORDER+1]={};
        for(int k=1;k<=LPC_ORDER;k++)
            reflection[k]=0.5f*(m.reflection[k]+mn.reflection[k]);
        float a[LPC_ORDER+1]={};
        lpc_from_reflection(reflection,a);
        const float residual=std::max(1e-5f,0.5f*(m.residual+mn.residual));

        const int begin=f*HOP;
        const int finish=std::min((int)out.size(),begin+HOP);
        for(int i=begin;i<finish;i++) {
            const float blend=(float)(i-begin)/(float)HOP;
            const float f0=pitch[f]*(1.0f-blend)+pitch[next]*blend;
            const float cf=conf[f]*(1.0f-blend)+conf[next]*blend;
            const float source_level=rms[f]*(1.0f-blend)+rms[next]*blend;
            float level=source_level/(source_rms_mean+1e-6f);
            level=std::max(0.30f,std::min(1.8f,level));

            float excitation=0.0f;
            if(f0>0.0f && cf>0.20f) {
                // A smooth harmonic glottal-flow pulse, not a one-sample impulse.
                phase+=std::max(60.0f,std::min(500.0f,f0))/(float)SR;
                phase-=std::floor(phase);
                for(int h=1;h<=10;h++)
                    excitation+=std::cos(2.0f*PI*(float)h*phase)/(float)(h*h);
                excitation*=1.55f; // approximately unit-ish excitation RMS
            } else {
                rng^=rng<<13; rng^=rng>>17; rng^=rng<<5;
                excitation=((int)(rng>>8)-8388608)/8388608.0f;
                excitation*=0.30f;
            }

            float y=excitation*residual;
            for(int k=1;k<=LPC_ORDER;k++) y-=a[k]*filter_state[k];
            if(!std::isfinite(y)) y=0.0f;
            // Bound the recursive state so malformed template frames cannot run away.
            y=std::max(-2.0f,std::min(2.0f,y));
            for(int k=LPC_ORDER;k>1;k--) filter_state[k]=filter_state[k-1];
            filter_state[1]=y;
            float sample=y+0.97f*deemphasis_state;
            if(!std::isfinite(sample)) sample=0.0f;
            sample=std::max(-2.0f,std::min(2.0f,sample));
            deemphasis_state=sample;
            out[i]=sample*level;
        }
    }

    // Loudness is set by RMS, not by a single isolated peak. This avoids the
    // previous failure where a nearly silent file was amplified to a 0.90 peak.
    double energy=0.0;
    for(float v:out) {
        if(!std::isfinite(v)) return false;
        energy+=(double)v*v;
    }
    const float measured_rms=(float)std::sqrt(energy/std::max<size_t>(1,out.size()));
    if(!(measured_rms>1e-6f) || !std::isfinite(measured_rms)) {
        std::fprintf(stderr,"reconstruction failed: synthesis produced no usable signal (RMS %.8g)\n",measured_rms);
        return false;
    }
    const float target_rms=std::max(0.025f,std::min(0.075f,template_rms*1.8f));
    const float gain=std::max(0.1f,std::min(20.0f,target_rms/measured_rms));
    float peak=0.0f;
    for(float& v:out) {
        v*=gain;
        peak=std::max(peak,std::fabs(v));
    }
    if(peak>0.95f) {
        const float limiter=0.95f/peak;
        for(float& v:out) v=std::tanh(v*limiter*1.35f)/std::tanh(1.35f);
    }
    if(!write_wav(output,out)) {
        std::fprintf(stderr,"cannot write %s\n",output);
        return false;
    }
    double final_energy=0.0;
    int active=0;
    float final_peak=0.0f;
    for(float v:out) {
        final_energy+=(double)v*v;
        final_peak=std::max(final_peak,std::fabs(v));
        if(std::fabs(v)>0.01f) active++;
    }
    const float final_rms=(float)std::sqrt(final_energy/std::max<size_t>(1,out.size()));
    std::printf("source analysis: %d frames; pitch contour retained where tracking is confident\n",frames);
    std::printf("stable LPC synthesis: reflection-coefficient interpolation; harmonic voiced excitation; noise-shaped unvoiced excitation\n");
    std::printf("template RMS %.5f; output RMS %.5f; peak %.5f; active samples %.1f%%\n",
                template_rms,final_rms,final_peak,100.0f*(float)active/std::max<size_t>(1,out.size()));
    std::printf("source waveform is not mixed into output\n");
    return true;
}

int main(int argc,char** argv) {
    if(argc<4){std::fprintf(stderr,"usage: %s source.wav clean_template.wav output.wav\n",argv[0]);return 2;}
    Audio source,templ;
    if(!load_wav(argv[1],source)||!load_wav(argv[2],templ))return 1;
    if(source.x.empty()||templ.x.empty())return 1;
    std::vector<FrameModel> alphabet;
    if(!build_template(templ,alphabet))return 1;
    return reconstruct(source,alphabet,argv[3])?0:1;
}

