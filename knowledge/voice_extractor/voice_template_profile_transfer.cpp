// voice_template_profile_transfer.cpp
// Experimental fragment matching + envelope transfer (libsndfile + FFTW3).
// Usage:
//   ./voice_template_profile_transfer profile template.wav target.wav profile.json
//   ./voice_template_profile_transfer transfer template.wav target.wav profile.json output.wav
// Outside accepted matched target fragments, output is silence.
// Spectral descriptor is a coarse formant/spectral-envelope proxy, NOT true LPC
// formant tracking or a dependable singer/word recognizer. Review profile.json.

#include <sndfile.h>
#include <fftw3.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;

constexpr int NFFT=1024, HOP=256, BINS=NFFT/2+1;
constexpr double EPS=1e-10;

struct Audio { int sr=0; vector<double> x; };
struct Frame { double rms=0; vector<double> spec; };
struct Frag { double ts,te,xs,xe,score; };

Audio load(const string& p) {
    SF_INFO i{}; SNDFILE*f=sf_open(p.c_str(),SFM_READ,&i);
    if(!f) throw runtime_error("Cannot open "+p+": "+sf_strerror(nullptr));
    vector<double> v((size_t)i.frames*i.channels);
    auto n=sf_readf_double(f,v.data(),i.frames); sf_close(f);
    if(n!=i.frames||i.channels<1||i.samplerate<=0) throw runtime_error("Invalid/short WAV: "+p);
    Audio a; a.sr=i.samplerate; a.x.resize(i.frames);
    for(sf_count_t k=0;k<i.frames;++k){ double s=0; for(int c=0;c<i.channels;++c)s+=v[(size_t)k*i.channels+c]; a.x[k]=s/i.channels; }
    return a;
}
void save(const string&p,const Audio&a){
    SF_INFO i{}; i.samplerate=a.sr;i.channels=1;i.format=SF_FORMAT_WAV|SF_FORMAT_PCM_16;
    SNDFILE*f=sf_open(p.c_str(),SFM_WRITE,&i);if(!f)throw runtime_error("Cannot write "+p);
    auto n=sf_writef_double(f,a.x.data(),a.x.size());sf_close(f);if(n!=(sf_count_t)a.x.size())throw runtime_error("Short WAV write");
}
vector<Frame> features(const Audio&a){
    vector<Frame> out; double*in=fftw_alloc_real(NFFT); fftw_complex*z=fftw_alloc_complex(BINS);
    auto plan=fftw_plan_dft_r2c_1d(NFFT,in,z,FFTW_ESTIMATE);
    for(size_t st=0;st<a.x.size();st+=HOP){
        double e=0;
        for(int k=0;k<NFFT;++k){long ix=(long)st+k-NFFT/2;double x=(ix>=0&&ix<(long)a.x.size())?a.x[ix]:0;
            double w=.5-.5*cos(2*M_PI*k/(NFFT-1));in[k]=x*w;e+=x*x;}
        fftw_execute(plan);Frame f;f.rms=sqrt(e/NFFT+EPS);f.spec.assign(32,0);
        for(int b=1;b<BINS;++b){double hz=(double)b*a.sr/NFFT;int band=clamp((int)floor(log2(1+hz/80)*5),0,31);f.spec[band]+=log(hypot(z[b][0],z[b][1])+EPS);}
        double m=accumulate(f.spec.begin(),f.spec.end(),0.0)/f.spec.size(),n=0;
        for(double&x:f.spec){x-=m;n+=x*x;}n=sqrt(n+EPS);for(double&x:f.spec)x/=n;
        out.push_back(move(f));if(st+HOP>=a.x.size())break;
    }
    fftw_destroy_plan(plan);fftw_free(in);fftw_free(z);return out;
}
vector<pair<int,int>> regions(const vector<Frame>&f){
    vector<pair<int,int>> r;if(f.empty())return r;vector<double>v;for(auto&x:f)v.push_back(x.rms);
    auto s=v;sort(s.begin(),s.end());double th=max(s[s.size()/3]*2.8,s.back()*.035);
    bool on=false;int st=0;
    for(int i=0;i<(int)f.size();++i){bool active=f[i].rms>th;if(active&&!on){st=i;on=true;}if(!active&&on){if(i-st>=6)r.push_back({st,i});on=false;}}
    if(on&&(int)f.size()-st>=6)r.push_back({st,(int)f.size()});
    vector<pair<int,int>> m;for(auto p:r){if(!m.empty()&&p.first-m.back().second<=7)m.back().second=p.second;else m.push_back(p);}return m;
}
double sim(const Frame&a,const Frame&b){double d=0;for(size_t i=0;i<a.spec.size();++i)d+=a.spec[i]*b.spec[i];return d;}
Frag match(const vector<Frame>&t,pair<int,int>r,const vector<Frame>&x){
    int len=r.second-r.first;Frag best{0,0,0,0,-2};if(len<2)return best;
    for(double scale:{.70,.80,.90,1.,1.10,1.20,1.30,1.40}){
        int qlen=max(2,(int)round(len*scale));if(qlen>(int)x.size())continue;int step=max(1,qlen/12);
        for(int q=0;q+qlen<=(int)x.size();q+=step){double s=0;for(int j=0;j<12;++j){int ti=r.first+(int)(j/11.0*(len-1));int qi=q+(int)(j/11.0*(qlen-1));s+=sim(t[ti],x[qi]);}
            s/=12;if(s>best.score)best={double(r.first),double(r.second),double(q),double(q+qlen),s};}
    }return best;
}
double num(const string&o,const string&k){regex re("\""+k+"\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)");smatch m;if(!regex_search(o,m,re))throw runtime_error("Missing field "+k);return stod(m[1]);}
vector<Frag> read_profile(const string&p){
    ifstream f(p);if(!f)throw runtime_error("Cannot open "+p);string s((istreambuf_iterator<char>(f)),{});regex re("\\{[^{}]*\\}");vector<Frag>v;
    for(sregex_iterator i(s.begin(),s.end(),re),e;i!=e;++i){string o=i->str();if(o.find("template_start")==string::npos)continue;
        Frag q{num(o,"template_start"),num(o,"template_end"),num(o,"target_start"),num(o,"target_end"),num(o,"confidence_cosine")};if(q.te>q.ts&&q.xe>q.xs)v.push_back(q);}
    return v;
}
void profile(const Audio&t,const Audio&x,const string&out){
    if(t.sr!=x.sr)throw runtime_error("Sample rates must match; resample first.");
    auto tf=features(t),xf=features(x);auto rr=regions(tf);ofstream j(out);if(!j)throw runtime_error("Cannot write "+out);
    j<<fixed<<setprecision(6)<<"{\n  \"method\":\"coarse_log_spectral_template_match\",\n  \"sample_rate\":"<<t.sr<<",\n  \"hop_seconds\":"<<(double)HOP/t.sr<<",\n  \"warning\":\"Proxy spectral envelope; inspect matches; not robust formant recognition.\",\n  \"fragments\":[\n";
    bool first=true;for(auto r:rr){auto m=match(tf,r,xf);if(m.score<.30)continue;if(!first)j<<",\n";first=false;
        double ts=r.first*(double)HOP/t.sr,te=r.second*(double)HOP/t.sr,xs=m.xs*(double)HOP/t.sr,xe=m.xe*(double)HOP/t.sr;
        double speed=(te-ts)/(xe-xs+EPS);
        j<<"    {\"template_start\":"<<ts<<",\"template_end\":"<<te<<",\"target_start\":"<<xs<<",\"target_end\":"<<xe
         <<",\"template_landmark_A\":"<<ts<<",\"template_landmark_B\":"<<te
         <<",\"target_landmark_A\":"<<xs<<",\"target_landmark_B\":"<<xe
         <<",\"speed_ratio_template_over_target\":"<<speed<<",\"time_scale_target_over_template\":"<<1/(speed+EPS)
         <<",\"confidence_cosine\":"<<m.score<<"}";
    }j<<"\n  ]\n}\n";cerr<<"Wrote "<<out<<"; template regions found: "<<rr.size()<<". Review matches.\n";
}
double template_rms(const Audio&t,double time,double start,double end){
    long c=(long)(time*t.sr),rad=max(1,t.sr/100),a=max(0L,c-rad),b=min((long)t.x.size(),c+rad);double e=0;int n=0;
    for(long i=a;i<b;++i){e+=t.x[i]*t.x[i];++n;}double local=sqrt(e/max(1,n)+EPS);
    long lo=max(0L,(long)(start*t.sr)),hi=min((long)t.x.size(),(long)(end*t.sr));double peak=0;
    for(long i=lo;i<hi;++i)peak=max(peak,abs(t.x[i]));return clamp(local/(peak+EPS),0.0,1.0);
}
void transfer(const Audio&t,const Audio&x,const string&json,const string&outpath){
    if(t.sr!=x.sr)throw runtime_error("Sample rates must match.");
    auto fs=read_profile(json);if(fs.empty())throw runtime_error("No accepted matches in JSON.");
    Audio out;out.sr=x.sr;out.x.assign(x.x.size(),0);
    for(auto&p:fs){long a=max(0L,(long)(p.xs*x.sr)),b=min((long)x.x.size(),(long)(p.xe*x.sr));if(b<=a)continue;
        double dur=p.xe-p.xs,fade=min(.04,dur*.15);
        for(long i=a;i<b;++i){double u=(double)(i-a)/max(1L,b-a-1),tt=p.ts+u*(p.te-p.ts);
            double gain=template_rms(t,tt,p.ts,p.te),edge=1;
            edge=min(edge,clamp(((double)(i-a)/x.sr)/max(fade,EPS),0.0,1.0));
            edge=min(edge,clamp(((double)(b-i)/x.sr)/max(fade,EPS),0.0,1.0));
            out.x[i]=x.x[i]*gain*edge;
        }
    }
    save(outpath,out);cerr<<"Wrote "<<outpath<<"; unmatched regions are silence.\n";
}
int main(int argc,char**argv){try{
    if(argc<4){cerr<<"Usage:\n  "<<argv[0]<<" profile template.wav target.wav [profile.json]\n  "<<argv[0]<<" transfer template.wav target.wav profile.json output.wav\n";return 1;}
    string mode=argv[1];Audio t=load(argv[2]),x=load(argv[3]);
    if(mode=="profile")profile(t,x,argc>4?argv[4]:"voice_profile.json");
    else if(mode=="transfer"){if(argc<6)throw runtime_error("transfer requires profile.json and output.wav");transfer(t,x,argv[4],argv[5]);}
    else throw runtime_error("Mode must be profile or transfer");
}catch(const exception&e){cerr<<"Error: "<<e.what()<<"\n";return 1;}return 0;}

