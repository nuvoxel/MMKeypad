/* Exercises the exact audio_call_* path: capture dev0 @48k stereo -> auresamp
 * 48k/2ch -> 8k/1ch -> G.711 u-law encode -> decode -> analyse at 8 kHz.
 * A 1 kHz tone is played on dev1 concurrently, so a working chain shows that
 * tone dominating the recovered 8 kHz signal. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <re.h>
#include <rem.h>
#include "tinyalsa/asoundlib.h"

#define HW_RATE 48000
#define HW_CH   2
#define SIP_RATE 8000
#define RATIO   (HW_RATE/SIP_RATE)
#define SIPN    160
#define HWN     (SIPN*RATIO)

static volatile int stop_play;
static void *player(void *a){
    double amp=*(double*)a, ph=0, st=2*M_PI*1000.0/HW_RATE;
    struct pcm_config c; memset(&c,0,sizeof c);
    c.channels=HW_CH; c.rate=HW_RATE; c.period_size=HWN; c.period_count=6; c.format=PCM_FORMAT_S16_LE;
    struct pcm *p=pcm_open(0,1,PCM_OUT,&c);
    if(!p||!pcm_is_ready(p)){ printf("  play open failed\n"); return 0; }
    short buf[HWN*HW_CH];
    while(!stop_play){
        for(int i=0;i<HWN;i++,ph+=st){ short v=(short)(amp*22000*sin(ph)); buf[i*HW_CH]=v; buf[i*HW_CH+1]=v; }
        if(pcm_write(p,buf,sizeof buf)) break;
    }
    pcm_close(p); return 0;
}
static double goertzel(const short*x,int n,double f){
    double w=2*M_PI*f/SIP_RATE,c=2*cos(w),s1=0,s2=0,s0;
    for(int i=0;i<n;i++){ s0=x[i]+c*s1-s2; s2=s1; s1=s0; }
    return s1*s1+s2*s2-c*s1*s2;
}
int main(int argc,char**argv){
    double amp = argc>1?atof(argv[1]):0.0;
    struct auresamp rs; auresamp_init(&rs);
    if(auresamp_setup(&rs,HW_RATE,HW_CH,SIP_RATE,1)){ printf("resamp setup FAILED\n"); return 1; }

    struct pcm_config c; memset(&c,0,sizeof c);
    c.channels=HW_CH; c.rate=HW_RATE; c.period_size=HWN; c.period_count=6; c.format=PCM_FORMAT_S16_LE;
    struct pcm *cap=pcm_open(0,0,PCM_IN,&c);
    if(!cap||!pcm_is_ready(cap)){ printf("capture open FAILED: %s\n",cap?pcm_get_error(cap):"null"); return 1; }

    pthread_t th; if(amp>0){ stop_play=0; pthread_create(&th,0,player,&amp); usleep(200000); }

    static short hw[HWN*HW_CH], pcm8[HWN*HW_CH], back[SIPN*40]; static unsigned char ulaw[SIPN];
    int bn=0, peak=0; double sq=0; int nsamp=0;
    for(int f=0; f<55; f++){
        if(pcm_read(cap,hw,sizeof hw)) break;
        size_t outc=(size_t)HWN*HW_CH;   /* capacity in, count out */
        if(auresamp(&rs,pcm8,&outc,hw,(size_t)HWN*HW_CH)){ printf("auresamp FAILED\n"); break; }
        for(size_t i=0;i<outc;i++) ulaw[i]=g711_pcm2ulaw(pcm8[i]);          /* encode */
        if(f<15) continue;   /* discard the capture-start transient */
        for(size_t i=0;i<outc;i++){                                          /* decode */
            short v=g711_ulaw2pcm(ulaw[i]);
            if(bn<(int)(sizeof back/sizeof back[0])) back[bn++]=v;
            int a=v<0?-v:v; if(a>peak)peak=a; sq+=(double)v*v; nsamp++;
        }
        if(f==0) printf("  frame: %d hw frames -> %zu 8k samples (expect %d)\n", HWN, outc, SIPN);
    }
    if(amp>0){ stop_play=1; pthread_join(th,0); }
    pcm_close(cap);
    if(!nsamp){ printf("no audio\n"); return 1; }
    double e1k=goertzel(back,bn,1000.0), eoff=goertzel(back,bn,1700.0);
    printf("  round-trip: peak=%d rms=%.1f  energy@1k=%.3e off@1.7k=%.3e ratio=%.1fx %s\n",
           peak, sqrt(sq/nsamp), e1k, eoff, eoff>0?e1k/eoff:0.0,
           (eoff>0 && e1k/eoff>10.0)?"<== 1kHz RECOVERED":"");
    return 0;
}
