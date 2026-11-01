/*
 * Experimental time-varying voice-band extractor.
 *
 * Input: PCM16 mono 16 kHz RIFF/WAVE.
 * Analysis: 48 logarithmically spaced, narrow (Q=6) band-pass filters.
 * Grouping: local envelope correlation in 200 ms windows, updated every
 * 40 ms. Groups are tracked between adjacent windows by frequency-band
 * overlap. Output masks therefore change over time instead of assigning
 * each frequency band to one permanent group.
 *
 * This is an experiment in modulation-coherence separation, not a trained
 * speaker separator. It can still merge speakers or split one speaker.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <new>

static const int SAMPLE_RATE = 16000;
static const int BAND_COUNT = 48;
static const int DECIMATE = 8;
static const int ENV_RATE = SAMPLE_RATE / DECIMATE;
static const int FRAME = 2400;      // 1.2 s at 2 kHz
static const int HOP = 200;         // 100 ms
static const int MAX_GROUPS = BAND_COUNT;
static const float Q = 6.0f;
static const float MIN_HZ = 70.0f;
static const float MAX_HZ = 7200.0f;
static const float PI_F = 3.14159265358979323846f;
static const float CORR_THRESHOLD = 0.55f;

struct Error { int line; const char *message; };
struct Result {
    bool ok; Error error;
    static Result success() { return {true, {0, ""}}; }
    static Result failure(int line, const char *message) { return {false, {line, message}}; }
};
#define FAIL(m) return Result::failure(__LINE__, m)
struct Audio { int count; float *samples; };
struct Biquad { float b0,b1,b2,a1,a2,x1,x2,y1,y2; };
struct Band { Biquad filter; float *signal; float *envelope; float mean_energy; };

static float center_hz(int band) {
    const float ratio = MAX_HZ / MIN_HZ;
    return MIN_HZ * powf(ratio, (float)band / (BAND_COUNT - 1));
}
static Biquad make_bandpass(float center) {
    const float w = 2.0f * PI_F * center / SAMPLE_RATE;
    const float alpha = sinf(w) / (2.0f * Q);
    const float norm = 1.0f + alpha;
    Biquad b = {alpha/norm, 0.0f, -alpha/norm,
                -2.0f*cosf(w)/norm, (1.0f-alpha)/norm,
                0,0,0,0};
    return b;
}
static inline float filter_sample(Biquad *b, float x) {
    const float y = b->b0*x + b->b1*b->x1 + b->b2*b->x2
                  - b->a1*b->y1 - b->a2*b->y2;
    b->x2=b->x1; b->x1=x; b->y2=b->y1; b->y1=y;
    return y;
}
static bool read_u16(FILE *f, unsigned short *v) {
    unsigned char b[2]; if (fread(b,1,2,f)!=2) return false;
    *v=(unsigned short)(b[0] | (b[1]<<8)); return true;
}
static bool read_u32(FILE *f, unsigned int *v) {
    unsigned char b[4]; if (fread(b,1,4,f)!=4) return false;
    *v=(unsigned int)b[0] | ((unsigned int)b[1]<<8) |
       ((unsigned int)b[2]<<16) | ((unsigned int)b[3]<<24); return true;
}
static Result load_wav(const char *path, Audio *audio) {
    FILE *f=fopen(path,"rb"); if(!f) FAIL("cannot open input WAV");
    char id[4]; unsigned int riff_size=0;
    if(fread(id,1,4,f)!=4 || memcmp(id,"RIFF",4)) { fclose(f); FAIL("not a RIFF WAV"); }
    if(!read_u32(f,&riff_size) || fread(id,1,4,f)!=4 || memcmp(id,"WAVE",4)) { fclose(f); FAIL("invalid RIFF/WAVE header"); }
    unsigned short fmt=0,ch=0,bits=0; unsigned int rate=0,data_size=0;
    bool have_fmt=false,have_data=false;
    while(!have_data) {
        if(fread(id,1,4,f)!=4) break;
        unsigned int size=0; if(!read_u32(f,&size)) { fclose(f); FAIL("truncated WAV chunk"); }
        long start=ftell(f);
        if(!memcmp(id,"fmt ",4)) {
            unsigned int byte_rate; unsigned short align;
            if(!read_u16(f,&fmt)||!read_u16(f,&ch)||!read_u32(f,&rate)||
               !read_u32(f,&byte_rate)||!read_u16(f,&align)||!read_u16(f,&bits)) {
                fclose(f); FAIL("invalid WAV fmt chunk");
            }
            if(size>16 && fseek(f,start+(long)size+(size&1u),SEEK_SET)!=0) { fclose(f); FAIL("invalid fmt chunk size"); }
            have_fmt=true;
        } else if(!memcmp(id,"data",4)) { data_size=size; have_data=true; }
        else if(fseek(f,start+(long)size+(size&1u),SEEK_SET)!=0) { fclose(f); FAIL("invalid WAV chunk size"); }
    }
    if(!have_fmt||!have_data) { fclose(f); FAIL("WAV is missing fmt or data chunk"); }
    if(fmt!=1||ch!=1||bits!=16||rate!=SAMPLE_RATE) { fclose(f); FAIL("input must be PCM16 mono 16000 Hz; convert with ffmpeg"); }
    if(data_size<2 || data_size/2 > 0x7fffffffU) { fclose(f); FAIL("invalid WAV data size"); }
    int n=(int)(data_size/2); float *x=new(std::nothrow) float[n];
    if(!x) { fclose(f); FAIL("out of memory reading WAV"); }
    for(int i=0;i<n;i++) { unsigned short u; if(!read_u16(f,&u)) { delete[] x; fclose(f); FAIL("truncated WAV samples"); } x[i]=(float)(short)u/32768.0f; }
    fclose(f); audio->count=n; audio->samples=x; return Result::success();
}
static Result make_filterbank(const Audio *audio, Band *bands, int env_count) {
    const float env_a=1.0f-expf(-1.0f/(SAMPLE_RATE*0.008f));
    for(int b=0;b<BAND_COUNT;b++) {
        bands[b].filter=make_bandpass(center_hz(b));
        bands[b].signal=new(std::nothrow) float[audio->count];
        bands[b].envelope=new(std::nothrow) float[env_count];
        bands[b].mean_energy=0;
        if(!bands[b].signal||!bands[b].envelope) FAIL("out of memory allocating filterbank");
        float env=0; int ei=0; double energy=0;
        for(int i=0;i<audio->count;i++) {
            float y=filter_sample(&bands[b].filter,audio->samples[i]);
            bands[b].signal[i]=y; energy+=(double)y*y;
            env+=env_a*(fabsf(y)-env);
            if(i%DECIMATE==0 && ei<env_count) bands[b].envelope[ei++]=env;
        }
        bands[b].mean_energy=(float)(energy/audio->count);
    }
    return Result::success();
}

/* Pearson correlation of two local envelope slices after removing their
 * local means. A little local linear detrending reduces slow loudness bias. */
static float local_corr(const float *a,const float *b,int start,int count) {
    if(count<80) return 0;
    double ma=0,mb=0,ta=0,tb=0;
    for(int i=0;i<count;i++){ ma+=a[start+i]; mb+=b[start+i]; ta+=i*a[start+i]; tb+=i*b[start+i]; }
    ma/=count; mb/=count;
    const double mt=(count-1)*0.5;
    const double den_t=count*(count*count-1.0)/12.0;
    const double sa=(ta-count*mt*ma)/den_t, sb=(tb-count*mt*mb)/den_t;
    double cross=0,ea=0,eb=0;
    for(int i=0;i<count;i++) {
        const double x=a[start+i]-ma-sa*(i-mt);
        const double y=b[start+i]-mb-sb*(i-mt);
        cross+=x*y; ea+=x*x; eb+=y*y;
    }
    const double d=sqrt(ea*eb);
    return d>1e-16 ? (float)(cross/d) : 0.0f;
}

/* Build local connected components, then carry component IDs forward using
 * maximum shared-band overlap. labels[frame][band] is the time-varying mask. */
static Result build_dynamic_masks(Band *bands,int env_count,int *frame_count,int **labels_out) {
    const int frames=1+(env_count-1)/HOP;
    int *labels=new(std::nothrow) int[frames*BAND_COUNT];
    if(!labels) FAIL("out of memory allocating time-varying masks");
    int previous[BAND_COUNT]; for(int b=0;b<BAND_COUNT;b++) previous[b]=-1;
    int next_group=0;
    int local[BAND_COUNT],component[BAND_COUNT];
    for(int f=0;f<frames;f++) {
        const int center=f*HOP;
        int start=center-FRAME/2; if(start<0) start=0;
        int end=center+FRAME/2; if(end>env_count) end=env_count;
        const int count=end-start;
        bool active[BAND_COUNT]={};
        float frame_rms[BAND_COUNT]={};
        float max_rms=0.0f;
        for(int b=0;b<BAND_COUNT;b++) {
            double e=0; for(int k=0;k<count;k++){double v=bands[b].envelope[start+k];e+=v*v;}
            frame_rms[b]=(float)sqrt(e/count);
            if(frame_rms[b]>max_rms) max_rms=frame_rms[b];
        }
        /* Ignore filter skirts and numerical tails: only bands carrying a
         * meaningful fraction of the frame's envelope energy participate. */
        const float activity_floor=fmaxf(1e-5f,max_rms*0.20f);
        for(int b=0;b<BAND_COUNT;b++)
            active[b]=(frame_rms[b]>=activity_floor && bands[b].mean_energy>1e-10f);
        float corr[BAND_COUNT][BAND_COUNT]={};
        for(int a=0;a<BAND_COUNT;a++) if(active[a]) for(int b=a+1;b<BAND_COUNT;b++) if(active[b]) {
            corr[a][b]=corr[b][a]=local_corr(bands[a].envelope,bands[b].envelope,start,count);
        }
        /* Complete-link grouping: a new band must correlate with every
         * existing member. This prevents a chain of weak adjacent-band links
         * from gluing two unrelated sources into one giant component. */
        for(int b=0;b<BAND_COUNT;b++) local[b]=-1;
        int comps=0;
        for(int b=0;b<BAND_COUNT;b++) if(active[b]) {
            int best_comp=-1; float best_score=-1.0f;
            for(int c=0;c<comps;c++) {
                bool fits=true; float score=0.0f; int members=0;
                for(int j=0;j<b;j++) if(local[j]==c) {
                    float v=corr[b][j]; if(v<CORR_THRESHOLD) { fits=false; break; }
                    score+=v; members++;
                }
                if(fits && members>0) { score/=members; if(score>best_score){best_score=score;best_comp=c;} }
            }
            if(best_comp>=0) local[b]=best_comp;
            else local[b]=comps++;
        }
        /* Greedy one-to-one match of new components to prior group IDs. */
        bool used[BAND_COUNT]={};
        for(int c=0;c<comps;c++) {
            int counts[BAND_COUNT]={};
            for(int b=0;b<BAND_COUNT;b++) if(local[b]==c && previous[b]>=0 && previous[b]<BAND_COUNT) counts[previous[b]]++;
            int best=-1,best_count=0;
            for(int g=0;g<BAND_COUNT;g++) if(!used[g]&&counts[g]>best_count){best=g;best_count=counts[g];}
            if(best<0) { if(next_group>=MAX_GROUPS) best=MAX_GROUPS-1; else best=next_group++; }
            used[best]=true; component[c]=best;
        }
        for(int b=0;b<BAND_COUNT;b++) {
            int g=(local[b]>=0)?component[local[b]]:-1;
            labels[f*BAND_COUNT+b]=g; previous[b]=g;
        }
    }
    *frame_count=frames; *labels_out=labels; return Result::success();
}

/*
 * Deterministic speech-event grammar.
 *
 * This is deliberately not a trained classifier. It detects two primitive
 * acoustic events:
 *   C: a sufficiently energetic, mostly aperiodic, high-ZCR/high-band event
 *   V: a periodic interval with a broad, non-flat resonant spectral envelope
 * A V event is accepted only when a C event occurs nearby in time. This
 * encodes a weak consonant-vowel ordering prior, not a phoneme recognizer.
 */
static const int SPEECH_HOP = 160;       // 10 ms
static const int SPEECH_FRAME = 400;     // 25 ms
struct SpeechFrame { float rms, periodicity, zcr, high_ratio, resonance; int consonant, vowel; };
static float sample_rms(const float *x, int start, int count) {
    double e=0; for(int i=0;i<count;i++){double v=x[start+i];e+=v*v;}
    return (float)sqrt(e/count);
}
static float frame_periodicity(const float *x,int start,int count) {
    double best=0.0, energy=0.0;
    for(int i=0;i<count;i++){double v=x[start+i];energy+=v*v;}
    if(energy<1e-9) return 0.0f;
    const int min_lag=SAMPLE_RATE/400, max_lag=SAMPLE_RATE/75;
    for(int lag=min_lag;lag<=max_lag && lag<count/2;lag++) {
        double c=0,a=0,b=0;
        for(int i=0;i<count-lag;i++){double u=x[start+i],v=x[start+i+lag];c+=u*v;a+=u*u;b+=v*v;}
        double d=sqrt(a*b); if(d>1e-12 && c/d>best) best=c/d;
    }
    return (float)best;
}
static float frame_zcr(const float *x,int start,int count) {
    int crossings=0; for(int i=1;i<count;i++) if((x[start+i]>=0)!=(x[start+i-1]>=0)) crossings++;
    return (float)crossings/(count-1);
}
static float envelope_resonance(Band *bands,int env_start,int env_count) {
    float e[BAND_COUNT]; double total=0;
    for(int b=0;b<BAND_COUNT;b++) {
        int a=env_start, z=env_start+env_count; double q=0;
        if(z>0) { if(a<0)a=0; for(int i=a;i<z;i++){double v=bands[b].envelope[i];q+=v*v;} }
        e[b]=(float)sqrt(q/(env_count>0?env_count:1)); total+=e[b];
    }
    if(total<1e-8) return 0.0f;
    /* Count broad local maxima above the mean envelope level. */
    int peaks=0; float mean=(float)(total/BAND_COUNT);
    for(int b=2;b<BAND_COUNT-2;b++)
        if(e[b]>e[b-1] && e[b]>=e[b+1] && e[b]>mean*1.35f) peaks++;
    /* Two or more resonant regions are more vowel-like than a flat spectrum. */
    return peaks>=2 ? fminf(1.0f,(float)peaks/5.0f) : 0.0f;
}
static Result build_speech_mask(const Audio *audio,Band *bands,int env_count,int *count_out,unsigned char **mask_out,int *cv_pairs_out) {
    if(audio->count<SPEECH_FRAME) FAIL("input is too short for speech-event analysis (need at least 25 ms)");
    const int frames=1+(audio->count-SPEECH_FRAME)/SPEECH_HOP;
    SpeechFrame *feat=new(std::nothrow) SpeechFrame[frames];
    unsigned char *mask=new(std::nothrow) unsigned char[frames];
    if(!feat||!mask){delete[] feat;delete[] mask;FAIL("out of memory allocating speech-event analysis");}
    float max_rms=0.0f;
    for(int f=0;f<frames;f++) {
        int start=f*SPEECH_HOP;
        SpeechFrame *v=&feat[f];
        v->rms=sample_rms(audio->samples,start,SPEECH_FRAME);
        if(v->rms>max_rms)max_rms=v->rms;
        v->periodicity=frame_periodicity(audio->samples,start,SPEECH_FRAME);
        v->zcr=frame_zcr(audio->samples,start,SPEECH_FRAME);
        int es=start/DECIMATE, ec=SPEECH_FRAME/DECIMATE;
        double high=0.0,all=0.0;
        for(int b=0;b<BAND_COUNT;b++) {
            double q=0; for(int k=0;k<ec && es+k<env_count;k++){double a=bands[b].envelope[es+k];q+=a*a;}
            if(q<0)q=0;
            all+=q; if(center_hz(b)>=2500.0f)high+=q;
        }
        v->high_ratio=all>1e-16?(float)(high/all):0.0f;
        v->resonance=envelope_resonance(bands,es,ec);
        v->consonant=0; v->vowel=0; mask[f]=0;
    }
    const float floor_rms=fmaxf(0.0008f,max_rms*0.025f);
    for(int f=0;f<frames;f++) {
        SpeechFrame *v=&feat[f];
        if(v->rms<floor_rms) continue;
        /* C: aperiodic, noisy articulation. ZCR threshold also catches many
           fricatives; high-band energy rejects most low-frequency thumps. */
        v->consonant=(v->periodicity<0.32f && v->zcr>0.075f && v->high_ratio>0.10f);
        /* V: periodic voicing plus a non-flat resonant envelope. */
        v->vowel=(v->periodicity>0.38f && v->resonance>=0.40f);
    }
    int pairs=0, consonants=0, vowels=0;
    for(int f=0;f<frames;f++){if(feat[f].consonant)consonants++;if(feat[f].vowel)vowels++;}
    const int reach=22; /* +/- 220 ms */
    for(int f=0;f<frames;f++) if(feat[f].vowel) {
        int found=-1,dist=reach+1;
        for(int d=1;d<=reach;d++) {
            int a=f-d,b=f+d;
            if(a>=0 && feat[a].consonant && d<dist){found=a;dist=d;}
            if(b<frames && feat[b].consonant && d<dist){found=b;dist=d;}
        }
        if(found>=0) {
            pairs++;
            /* Keep the consonant-vowel transition and a small coarticulation
               margin on either side; adjacent accepted vowels join naturally. */
            int lo=found<f?found:f, hi=found>f?found:f;
            lo=lo>2?lo-2:0; hi=hi+3<frames?hi+3:frames-1;
            for(int k=lo;k<=hi;k++) mask[k]=1;
        }
    }
    /* Fill short holes and soften the binary event track at the sample stage. */
    for(int f=1;f+1<frames;f++) if(mask[f-1] && mask[f+1]) mask[f]=1;
    delete[] feat;
    *count_out=frames; *mask_out=mask; *cv_pairs_out=pairs;
    fprintf(stderr,"speech events: C=%d V=%d paired-V=%d\n",consonants,vowels,pairs);
    return Result::success();
}
static float speech_gate_at(const unsigned char *mask,int frames,int sample) {
    float p=((float)sample-0.5f*SPEECH_FRAME)/SPEECH_HOP;
    if(p<=0.0f)return mask[0]?1.0f:0.0f;
    int f=(int)floorf(p); float t=p-f;
    if(f>=frames-1)return mask[frames-1]?1.0f:0.0f;
    float a=mask[f]?1.0f:0.0f,b=mask[f+1]?1.0f:0.0f;
    return a+(b-a)*t;
}

/* Linearly interpolate frame masks so group changes do not click. */
static float mask_at(const int *labels,int frames,int band,int group,int sample) {
    float env_pos=(float)sample/DECIMATE;
    float frame_pos=env_pos/HOP;
    if(frame_pos<=0) return labels[band]==group ? 1.0f : 0.0f;
    int f=(int)floorf(frame_pos); float frac=frame_pos-f;
    if(f>=frames-1) return labels[(frames-1)*BAND_COUNT+band]==group ? 1.0f : 0.0f;
    int a=labels[f*BAND_COUNT+band], b=labels[(f+1)*BAND_COUNT+band];
    return (a==group ? 1.0f-frac : 0.0f)+(b==group ? frac : 0.0f);
}
static Result write_wav(const char *path,const float *samples,int count) {
    if(count<=0 || (unsigned long long)count>(0xffffffffull-36)/2) FAIL("invalid or oversized WAV output");
    FILE *f=fopen(path,"wb"); if(!f) FAIL("cannot create output WAV");
    float peak=0.0f;
    for(int i=0;i<count;i++) if(fabsf(samples[i])>peak) peak=fabsf(samples[i]);
    const float gain=(peak>0.95f)?(0.95f/peak):1.0f;
    unsigned int data=(unsigned int)count*2u, riff=36u+data;
    unsigned char h[44]={'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,2,0,16,0,'d','a','t','a',0,0,0,0};
    for(int i=0;i<4;i++){h[4+i]=(unsigned char)(riff>>(8*i));h[40+i]=(unsigned char)(data>>(8*i));}
    if(fwrite(h,1,44,f)!=44){fclose(f);FAIL("failed writing WAV header");}
    for(int i=0;i<count;i++) {
        float x=samples[i]*gain; if(x>1)x=1;if(x< -1)x=-1;
        short s=(short)lrintf(x*32767.0f); unsigned short u=(unsigned short)s;
        unsigned char q[2]={(unsigned char)u,(unsigned char)(u>>8)};
        if(fwrite(q,1,2,f)!=2){fclose(f);FAIL("failed writing WAV samples");}
    }
    if(fclose(f)!=0) FAIL("failed closing WAV");
    return Result::success();
}


/*
 * Experimental tonal-music destruction pass.
 *
 * A 1024-sample Hann STFT is used to find narrow spectral peaks that remain
 * at approximately the same frequency for a sustained interval. Such peaks
 * are softly attenuated. Short-lived and frequency-moving structures are
 * retained. This is deliberately a conservative, deterministic heuristic:
 * sustained vowels and some instruments can still be confused.
 */
static const int FFT_SIZE = 1024;
static const int FFT_BINS = FFT_SIZE/2 + 1;
static const int FFT_HOP = 256;
static const int TONAL_RADIUS = 16; /* 512 ms on either side */
static const float LOW_CUT_START_HZ = 90.0f;
static const float LOW_CUT_END_HZ = 180.0f;
static const float LOW_CUT_FLOOR = 0.0f; /* fully remove sub-90 Hz energy */

static void fft(float *re, float *im, bool inverse) {
    int j=0;
    for(int i=1;i<FFT_SIZE;i++) {
        int bit=FFT_SIZE>>1;
        for(;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if(i<j) { float t=re[i];re[i]=re[j];re[j]=t; t=im[i];im[i]=im[j];im[j]=t; }
    }
    for(int len=2;len<=FFT_SIZE;len<<=1) {
        const float angle=(inverse?2.0f:-2.0f)*PI_F/len;
        const float wr0=cosf(angle), wi0=sinf(angle);
        for(int i=0;i<FFT_SIZE;i+=len) {
            float wr=1.0f,wi=0.0f;
            for(int k=0;k<len/2;k++) {
                int a=i+k,b=a+len/2;
                float vr=re[b]*wr-im[b]*wi, vi=re[b]*wi+im[b]*wr;
                re[b]=re[a]-vr; im[b]=im[a]-vi;
                re[a]+=vr; im[a]+=vi;
                float nw=wr*wr0-wi*wi0; wi=wr*wi0+wi*wr0; wr=nw;
            }
        }
    }
    if(inverse) for(int i=0;i<FFT_SIZE;i++){re[i]/=FFT_SIZE;im[i]/=FFT_SIZE;}
}

static Result destroy_stable_tones(const Audio *audio,const char *prefix) {
    const int frames=(audio->count+FFT_HOP-1)/FFT_HOP+1;
    const int padded=audio->count+FFT_SIZE;
    float *magnitudes=new(std::nothrow) float[(size_t)frames*FFT_BINS];
    unsigned char *peaks=new(std::nothrow) unsigned char[(size_t)frames*FFT_BINS];
    float *spectral_flux=new(std::nothrow) float[frames];
    float *spectral_flatness=new(std::nothrow) float[frames];
    float *low_flux=new(std::nothrow) float[frames];
    float *mid_flux=new(std::nothrow) float[frames];
    float *high_flux=new(std::nothrow) float[frames];
    float *low_energy=new(std::nothrow) float[frames];
    float *mid_energy=new(std::nothrow) float[frames];
    float *high_energy=new(std::nothrow) float[frames];
    float *clap_gain=new(std::nothrow) float[frames];
    unsigned char *drum_mask=new(std::nothrow) unsigned char[frames];
    float *re=new(std::nothrow) float[FFT_SIZE];
    float *im=new(std::nothrow) float[FFT_SIZE];
    float *out=new(std::nothrow) float[padded];
    float *weight=new(std::nothrow) float[padded];
    if(!magnitudes||!peaks||!spectral_flux||!spectral_flatness||!low_flux||!mid_flux||!high_flux||!low_energy||!mid_energy||!high_energy||!clap_gain||!drum_mask||!re||!im||!out||!weight) {
        delete[] magnitudes;delete[] peaks;delete[] spectral_flux;delete[] spectral_flatness;
        delete[] low_flux;delete[] mid_flux;delete[] high_flux;delete[] low_energy;delete[] mid_energy;delete[] high_energy;delete[] clap_gain;delete[] drum_mask;
        delete[] re;delete[] im;delete[] out;delete[] weight;
        FAIL("out of memory allocating tonal-destruction STFT");
    }
    memset(peaks,0,(size_t)frames*FFT_BINS);
    memset(spectral_flux,0,sizeof(float)*frames);
    memset(spectral_flatness,0,sizeof(float)*frames);
    memset(low_flux,0,sizeof(float)*frames);memset(mid_flux,0,sizeof(float)*frames);memset(high_flux,0,sizeof(float)*frames);
    memset(low_energy,0,sizeof(float)*frames);memset(mid_energy,0,sizeof(float)*frames);memset(high_energy,0,sizeof(float)*frames);
    for(int f=0;f<frames;f++) clap_gain[f]=1.0f;
    memset(drum_mask,0,(size_t)frames);
    const float pi=PI_F;
    for(int f=0;f<frames;f++) {
        int base=f*FFT_HOP;
        for(int n=0;n<FFT_SIZE;n++) {
            int src=base+n-FFT_SIZE/2;
            float x=(src>=0&&src<audio->count)?audio->samples[src]:0.0f;
            float w=0.5f-0.5f*cosf(2.0f*pi*n/(FFT_SIZE-1));
            re[n]=x*w;im[n]=0.0f;
        }
        fft(re,im,false);
        double sum=0.0;
        for(int b=0;b<FFT_BINS;b++) {
            float m=sqrtf(re[b]*re[b]+im[b]*im[b]);
            magnitudes[(size_t)f*FFT_BINS+b]=m;sum+=m;
        }
        float mean=(float)(sum/FFT_BINS);
        /* Spectral flux catches abrupt broadband attacks such as clicks and
         * claps. Flatness rejects attacks dominated by one narrow tone. */
        double positive_flux=0.0, previous_energy=1e-12, log_sum=0.0, arithmetic_sum=0.0;
        double lf=0.0,mf=0.0,hf=0.0,lp=1e-9,mp=1e-9,hp=1e-9;
        double le=0.0,me=0.0,he=0.0;
        int first_bin=5;
        for(int b=first_bin;b<FFT_BINS;b++) {
            float m=magnitudes[(size_t)f*FFT_BINS+b];
            arithmetic_sum+=m;
            log_sum+=logf(fmaxf(m,1e-12f));
            float prev=f>0?magnitudes[(size_t)(f-1)*FFT_BINS+b]:0.0f;
            if(f>0 && m>prev) positive_flux+=(double)(m-prev);
            if(f>0) previous_energy+=prev;
            const float hz=(float)b*SAMPLE_RATE/FFT_SIZE;
            if(hz<250.0f) { le+=m; if(m>prev)lf+=(m-prev); if(f>0)lp+=prev; }
            else if(hz<900.0f) { me+=m; if(m>prev)mf+=(m-prev); if(f>0)mp+=prev; }
            else { he+=m; if(m>prev)hf+=(m-prev); if(f>0)hp+=prev; }
        }
        spectral_flux[f]=(float)(positive_flux/previous_energy);
        low_flux[f]=(float)(lf/lp);mid_flux[f]=(float)(mf/mp);high_flux[f]=(float)(hf/hp);
        low_energy[f]=(float)le;mid_energy[f]=(float)me;high_energy[f]=(float)he;
        const int flat_bins=FFT_BINS-first_bin;
        const float arithmetic=(float)(arithmetic_sum/flat_bins);
        spectral_flatness[f]=arithmetic>1e-12f
            ? expf((float)(log_sum/flat_bins))/arithmetic : 0.0f;
        /* A narrow local maximum is a candidate musical partial. */
        for(int b=3;b<FFT_BINS-3;b++) {
            float m=magnitudes[(size_t)f*FFT_BINS+b];
            float side=0.0f;
            for(int d=2;d<=4;d++) side+=magnitudes[(size_t)f*FFT_BINS+b-d]+magnitudes[(size_t)f*FFT_BINS+b+d];
            side/=6.0f;
            if(m>mean*2.0f && m>side*2.0f && m>magnitudes[(size_t)f*FFT_BINS+b-1] && m>=magnitudes[(size_t)f*FFT_BINS+b+1])
                peaks[(size_t)f*FFT_BINS+b]=1;
        }
    }
    /*
     * Seed-envelope modulation recovery.
     * Select a few isolated, dynamically varying bins as clean modulation
     * references. A matched target bin keeps its own STFT carrier/content;
     * only its gain envelope is borrowed from the seed.
     */
    const int MAX_SEEDS=8;
    float *seed_env=new(std::nothrow) float[(size_t)frames*MAX_SEEDS];
    int *mod_seed=new(std::nothrow) int[FFT_BINS];
    int *mod_lag=new(std::nothrow) int[FFT_BINS];
    if(!seed_env||!mod_seed||!mod_lag) {
        delete[] seed_env;delete[] mod_seed;delete[] mod_lag;
        delete[] magnitudes;delete[] peaks;delete[] spectral_flux;delete[] spectral_flatness;
        delete[] low_flux;delete[] mid_flux;delete[] high_flux;delete[] low_energy;delete[] mid_energy;delete[] high_energy;delete[] clap_gain;delete[] drum_mask;
        delete[] re;delete[] im;delete[] out;delete[] weight;
        FAIL("out of memory allocating modulation recovery");
    }
    for(int b=0;b<FFT_BINS;b++){mod_seed[b]=-1;mod_lag[b]=0;}
    float seed_score[FFT_BINS]={};
    for(int b=4;b<FFT_BINS-4;b++) {
        float hz=(float)b*SAMPLE_RATE/FFT_SIZE;
        if(hz<180.0f||hz>7000.0f)continue;
        double sum=0.0,sum2=0.0,contrast=0.0;int peak_hits=0;
        for(int f=0;f<frames;f++) {
            float v=magnitudes[(size_t)f*FFT_BINS+b];
            sum+=v;sum2+=(double)v*v;
            float side=0.5f*(magnitudes[(size_t)f*FFT_BINS+b-2]+magnitudes[(size_t)f*FFT_BINS+b+2]);
            contrast+=v/(side+1e-8f);
            if(peaks[(size_t)f*FFT_BINS+b])peak_hits++;
        }
        double mean=sum/frames;
        double cv=mean>1e-10?sqrt(fmax(0.0,sum2/frames-mean*mean))/mean:0.0;
        float peak_rate=(float)peak_hits/frames;
        float isolation=(float)(contrast/frames);
        /* Reject stationary partials and broadband/noisy bins. */
        if(isolation>1.35f&&cv>0.22&&peak_rate<0.62f)
            seed_score[b]=(isolation-1.0f)*(float)fmin(cv,1.5);
    }
    int seed_bins[MAX_SEEDS],seed_count=0;
    for(int n=0;n<MAX_SEEDS;n++) {
        int best=-1;float score=0.0f;
        for(int b=4;b<FFT_BINS-4;b++) {
            if(seed_score[b]<=score)continue;
            bool spaced=true;for(int j=0;j<seed_count;j++)if(abs(b-seed_bins[j])<4)spaced=false;
            if(spaced){best=b;score=seed_score[b];}
        }
        if(best<0)break;
        seed_bins[seed_count++]=best;seed_score[best]=0.0f;
    }
    for(int j=0;j<seed_count;j++) {
        int b=seed_bins[j];double mean=0.0;
        for(int f=0;f<frames;f++){float v=magnitudes[(size_t)f*FFT_BINS+b];seed_env[(size_t)f*MAX_SEEDS+j]=v;mean+=v;}
        mean/=frames;
        for(int f=0;f<frames;f++) {
            float v=seed_env[(size_t)f*MAX_SEEDS+j]/(float)(mean+1e-8);
            float smooth=v;int count=1;
            if(f>0){smooth+=seed_env[(size_t)(f-1)*MAX_SEEDS+j]/(float)(mean+1e-8);count++;}
            if(f+1<frames){smooth+=seed_env[(size_t)(f+1)*MAX_SEEDS+j]/(float)(mean+1e-8);count++;}
            seed_env[(size_t)f*MAX_SEEDS+j]=fminf(1.0f,fmaxf(0.15f,smooth/count));
        }
    }
    int modulation_targets=0;
    for(int b=4;b<FFT_BINS-2;b++) {
        bool is_seed=false;for(int j=0;j<seed_count;j++)if(seed_bins[j]==b)is_seed=true;
        if(is_seed)continue;
        float best_corr=0.0f;int best_seed=-1,best_lag=0;
        for(int j=0;j<seed_count;j++)for(int lag=-2;lag<=2;lag++) {
            int begin=lag<0?-lag:0,end=frames-(lag>0?lag:0),n=end-begin;
            if(n<32)continue;
            double mx=0.0,my=0.0;
            for(int f=begin;f<end;f++){mx+=magnitudes[(size_t)f*FFT_BINS+b];my+=seed_env[(size_t)(f+lag)*MAX_SEEDS+j];}
            mx/=n;my/=n;double cross=0.0,ex=0.0,ey=0.0;
            for(int f=begin;f<end;f++){
                double x=magnitudes[(size_t)f*FFT_BINS+b]-mx;
                double y=seed_env[(size_t)(f+lag)*MAX_SEEDS+j]-my;
                cross+=x*y;ex+=x*x;ey+=y*y;
            }
            double den=sqrt(ex*ey);float corr=den>1e-16?(float)(cross/den):0.0f;
            if(corr>best_corr){best_corr=corr;best_seed=j;best_lag=lag;}
        }
        if(best_corr>=0.72f){mod_seed[b]=best_seed;mod_lag[b]=best_lag;modulation_targets++;}
    }

    /* Classify percussion only after every frame's features are available. */
    for(int f=0;f+1<frames;f++) {
        bool clap=(high_flux[f]>0.48f && mid_flux[f]>0.34f &&
                   spectral_flatness[f]>0.18f &&
                   high_flux[f+1]<high_flux[f]*0.88f &&
                   spectral_flux[f]>0.30f);
        bool drum=(low_flux[f]>0.72f && low_energy[f]>(mid_energy[f]+high_energy[f])*0.12f &&
                   low_flux[f+1]<low_flux[f]*0.90f);
        if(clap) {
            /* Strong initial cut, slow recovery, then a smooth release.
               Total window: 32-frame hold/recovery + 20-frame release. */
            for(int k=0;k<52 && f+k<frames;k++) {
                float g;
                if(k<32) g=0.02f+0.33f*(float)k/31.0f;
                else g=0.35f+0.65f*(float)(k-32)/19.0f;
                if(g<clap_gain[f+k]) clap_gain[f+k]=g;
            }
        }
        if(drum) for(int k=f;k<frames && k<f+6;k++) drum_mask[k]=1;
    }
    memset(out,0,sizeof(float)*padded);memset(weight,0,sizeof(float)*padded);
    int suppressed=0;
    for(int f=0;f<frames;f++) {
        for(int n=0;n<FFT_SIZE;n++){re[n]=0.0f;im[n]=0.0f;}
        int base=f*FFT_HOP;
        for(int n=0;n<FFT_SIZE;n++) {
            int src=base+n-FFT_SIZE/2;
            float x=(src>=0&&src<audio->count)?audio->samples[src]:0.0f;
            float w=0.5f-0.5f*cosf(2.0f*pi*n/(FFT_SIZE-1));
            re[n]=x*w;im[n]=0.0f;
        }
        fft(re,im,false);
        for(int b=0;b<FFT_BINS;b++) {
            float original=sqrtf(re[b]*re[b]+im[b]*im[b]);
            int lo=f-TONAL_RADIUS;if(lo<0)lo=0;
            int hi=f+TONAL_RADIUS+1;if(hi>frames)hi=frames;
            int hits=0,total=0;
            for(int tf=lo;tf<hi;tf++) {bool hit=false;for(int db=-1;db<=1;db++){int q=b+db;if(q>=0&&q<FFT_BINS&&peaks[(size_t)tf*FFT_BINS+q])hit=true;}if(hit)hits++;total++;}
            float persistence=total?(float)hits/total:0.0f;
            /* Persistent narrow partials are treated as instrumental and removed
               completely, rather than merely turned down. */
            float tonal_amount=(total>=20 && persistence>0.62f)?1.0f:0.0f;

            /* Fully remove sub-90 Hz energy; fade back to full level by 180 Hz. */
            const float hz=(float)b*SAMPLE_RATE/FFT_SIZE;
            float low_gain=1.0f;
            if(hz<LOW_CUT_START_HZ) low_gain=LOW_CUT_FLOOR;
            else if(hz<LOW_CUT_END_HZ) {
                float t=(hz-LOW_CUT_START_HZ)/(LOW_CUT_END_HZ-LOW_CUT_START_HZ);
                low_gain=LOW_CUT_FLOOR+(1.0f-LOW_CUT_FLOOR)*t;
            }

            /* Claps receive a deep, long attenuation with a gradual release.
             * Speech plosives can still resemble attacks, so clap decisions
             * require simultaneous mid/high-band flux and spectral flatness. */
            float transient_gain=1.0f;
            if(hz>=250.0f && hz<=8000.0f)
                transient_gain=fminf(transient_gain,clap_gain[f]);
            if(drum_mask[f] && hz>=35.0f && hz<=420.0f) {
                transient_gain=fminf(transient_gain,0.10f);
            }
            float modulation_gain=1.0f;
            if(mod_seed[b]>=0&&tonal_amount<0.5f) {
                int sf=f+mod_lag[b];if(sf<0)sf=0;if(sf>=frames)sf=frames-1;
                modulation_gain=seed_env[(size_t)sf*MAX_SEEDS+mod_seed[b]];
            }
            float scale=(original>1e-12f)?(1.0f-tonal_amount)*low_gain*transient_gain*modulation_gain:1.0f;
            if(tonal_amount>0.05f || low_gain<0.99f || transient_gain<0.99f) suppressed++;
            re[b]*=scale; im[b]*=scale;
            if(b>0&&b<FFT_SIZE/2){re[FFT_SIZE-b]=re[b];im[FFT_SIZE-b]=-im[b];}
        }
        fft(re,im,true);
        for(int n=0;n<FFT_SIZE;n++) {
            int dst=base+n;
            if(dst>=padded)continue;
            float w=0.5f-0.5f*cosf(2.0f*pi*n/(FFT_SIZE-1));
            out[dst]+=re[n]*w;weight[dst]+=w*w;
        }
    }
    float *result=new(std::nothrow) float[audio->count];
    if(!result){delete[] magnitudes;delete[] peaks;delete[] spectral_flux;delete[] spectral_flatness;
        delete[] low_flux;delete[] mid_flux;delete[] high_flux;delete[] low_energy;delete[] mid_energy;delete[] high_energy;delete[] clap_gain;delete[] drum_mask;
        delete[] re;delete[] im;delete[] out;delete[] weight;delete[] seed_env;delete[] mod_seed;delete[] mod_lag;
        FAIL("out of memory allocating tonal-destruction output");}
    for(int i=0;i<audio->count;i++) {int k=i+FFT_SIZE/2;result[i]=weight[k]>1e-8f?out[k]/weight[k]:audio->samples[i];}
    char voice_path[512], damaged_path[512];
    snprintf(voice_path,sizeof(voice_path),"%s_voice_candidate.wav",prefix);
    snprintf(damaged_path,sizeof(damaged_path),"%s_removed_tones.wav",prefix);

    /* Preserve the actual removed component, then normalize only the candidate. */
    float *removed=new(std::nothrow) float[audio->count];
    if(!removed) {
        delete[] result;delete[] magnitudes;delete[] peaks;delete[] spectral_flux;delete[] spectral_flatness;
        delete[] low_flux;delete[] mid_flux;delete[] high_flux;delete[] low_energy;delete[] mid_energy;delete[] high_energy;delete[] clap_gain;delete[] drum_mask;
        delete[] re;delete[] im;delete[] out;delete[] weight;delete[] seed_env;delete[] mod_seed;delete[] mod_lag;
        FAIL("out of memory allocating removed-component output");
    }
    float candidate_peak=0.0f;
    for(int i=0;i<audio->count;i++) {
        removed[i]=audio->samples[i]-result[i];
        float a=fabsf(result[i]);
        if(a>candidate_peak) candidate_peak=a;
    }
    const float normalization_gain=(candidate_peak>1.0e-6f)?(0.95f/candidate_peak):1.0f;
    for(int i=0;i<audio->count;i++) result[i]*=normalization_gain;

    Result wr=write_wav(voice_path,result,audio->count);
    if(wr.ok) wr=write_wav(damaged_path,removed,audio->count);
    delete[] removed;
    if(wr.ok) {
        printf("destruction: %d STFT frames, %d Hz bin spacing, %d attenuated frame-bin decisions\n",frames,SAMPLE_RATE/FFT_SIZE,suppressed);
        printf("low cut: %.0f Hz strongly attenuated, fading to full level at %.0f Hz\n",LOW_CUT_START_HZ,LOW_CUT_END_HZ);
        int clap_frames=0,drum_frames=0;for(int f=0;f<frames;f++){if(clap_gain[f]<0.999f)clap_frames++;if(drum_mask[f])drum_frames++;}
        printf("percussion suppression: %d clap-like frames (250-8000 Hz), %d drum-like frames (35-420 Hz)\n",clap_frames,drum_frames);
        printf("percussion attenuation: claps start at 98%% reduction with a 832 ms recovery/release; drums at 90%% reduction\n");
        printf("modulation recovery: %d seed bins, %d matched target bins (correlation >= 0.72)\n",seed_count,modulation_targets);
        printf("voice candidate peak normalization: %.3fx gain (target peak 0.95)\n",normalization_gain);
        printf("wrote %s (peak-normalized candidate after suppression)\n",voice_path);
        printf("wrote %s (removed component; listen for what was destroyed)\n",damaged_path);
    }
    delete[] result;delete[] magnitudes;delete[] peaks;delete[] spectral_flux;delete[] spectral_flatness;
    delete[] low_flux;delete[] mid_flux;delete[] high_flux;delete[] low_energy;delete[] mid_energy;delete[] high_energy;delete[] clap_gain;delete[] drum_mask;
    delete[] re;delete[] im;delete[] out;delete[] weight;
    delete[] seed_env;delete[] mod_seed;delete[] mod_lag;
    return wr;
}

int main(int argc,char **argv) {
    if(argc<2){fprintf(stderr,"usage: %s input.wav [output_prefix] [--legacy [--raw]]\n",argv[0]);return 2;}
    const char *prefix="voice"; bool use_speech_gate=true, legacy=false;
    for(int a=2;a<argc;a++) {
        if(!strcmp(argv[a],"--raw"))use_speech_gate=false;
        else if(!strcmp(argv[a],"--legacy"))legacy=true;
        else prefix=argv[a];
    }
    Audio audio={0,NULL}; Result r=load_wav(argv[1],&audio);
    if(!r.ok){fprintf(stderr,"error at line %d: %s\n",r.error.line,r.error.message);return 1;}
    if(!legacy) {
        r=destroy_stable_tones(&audio,prefix);
        if(!r.ok) fprintf(stderr,"error at line %d: %s\n",r.error.line,r.error.message);
        delete[] audio.samples;
        return r.ok?0:1;
    }
    const int env_count=(audio.count-1)/DECIMATE+1;
    Band bands[BAND_COUNT]={};
    r=make_filterbank(&audio,bands,env_count);
    if(!r.ok){fprintf(stderr,"error at line %d: %s\n",r.error.line,r.error.message);for(int b=0;b<BAND_COUNT;b++){delete[] bands[b].signal;delete[] bands[b].envelope;}delete[] audio.samples;return 1;}
    int frames=0; int *labels=NULL;
    r=build_dynamic_masks(bands,env_count,&frames,&labels);
    if(!r.ok){fprintf(stderr,"error at line %d: %s\n",r.error.line,r.error.message);for(int b=0;b<BAND_COUNT;b++){delete[] bands[b].signal;delete[] bands[b].envelope;}delete[] audio.samples;return 1;}
    int speech_frames=0,cv_pairs=0; unsigned char *speech_mask=NULL;
    r=build_speech_mask(&audio,bands,env_count,&speech_frames,&speech_mask,&cv_pairs);
    if(!r.ok){fprintf(stderr,"error at line %d: %s\n",r.error.line,r.error.message);delete[] labels;for(int b=0;b<BAND_COUNT;b++){delete[] bands[b].signal;delete[] bands[b].envelope;}delete[] audio.samples;return 1;}
    bool present[MAX_GROUPS]={};
    for(int f=0;f<frames;f++)for(int b=0;b<BAND_COUNT;b++){int g=labels[f*BAND_COUNT+b];if(g>=0&&g<MAX_GROUPS)present[g]=true;}
    int groups=0;for(int g=0;g<MAX_GROUPS;g++)if(present[g])groups++;
    printf("time-varying extractor: %d samples (%.2f s)\n",audio.count,(double)audio.count/SAMPLE_RATE);
    printf("filterbank: %d logarithmic bands, %.0f-%.0f Hz, Q=%.1f\n",BAND_COUNT,MIN_HZ,MAX_HZ,Q);
    printf("local grouping: %.0f ms windows, %.0f ms hop, correlation threshold %.2f\n",1000.0*FRAME/ENV_RATE,1000.0*HOP/ENV_RATE,CORR_THRESHOLD);
    printf("tracked group IDs: %d\n",groups);
    printf("speech event grammar: %d consonant-vowel pair candidates, %d analysis frames\n",cv_pairs,speech_frames);
    printf("speech gate: %s (use --raw for ungated band groups)\n",use_speech_gate?"C-V sequence":"disabled");
    float *mix=new(std::nothrow) float[audio.count];
    if(!mix){fprintf(stderr,"error: out of memory allocating output\n");}
    else {
        for(int g=0;g<MAX_GROUPS;g++) if(present[g]) {
            memset(mix,0,sizeof(float)*audio.count); int members=0;
            for(int b=0;b<BAND_COUNT;b++) {
                bool belongs=false;for(int f=0;f<frames;f++)if(labels[f*BAND_COUNT+b]==g){belongs=true;break;}
                if(!belongs) continue;
                members++;
                for(int s=0;s<audio.count;s++) mix[s]+=bands[b].signal[s]*mask_at(labels,frames,b,g,s)*(use_speech_gate?speech_gate_at(speech_mask,speech_frames,s):1.0f);
            }
            char path[512];snprintf(path,sizeof(path),"%s_group_%02d.wav",prefix,g);
            r=write_wav(path,mix,audio.count);
            if(!r.ok)fprintf(stderr,"error at line %d: %s\n",r.error.line,r.error.message);
            else printf("wrote %s (%d bands at some point)\n",path,members);
        }
        delete[] mix;
    }
    delete[] speech_mask;
    delete[] labels;
    for(int b=0;b<BAND_COUNT;b++){delete[] bands[b].signal;delete[] bands[b].envelope;}
    delete[] audio.samples;
    return 0;
}
