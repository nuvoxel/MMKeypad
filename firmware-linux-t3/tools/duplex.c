/* Why does capture level drop when playback is open?
 * Measures the mic at three stages and dumps the codec regs at each, so we can
 * see exactly what opening the playback stream does to the capture path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include "tinyalsa/asoundlib.h"

#define RATE 48000
#define CH   2
#define PER  960
#define ADDR 0x1c

static int i2cfd = -1;
static int rd(unsigned char reg, unsigned *val){
    unsigned char b[2];
    struct i2c_msg m[2] = {{.addr=ADDR,.flags=0,.len=1,.buf=&reg},{.addr=ADDR,.flags=I2C_M_RD,.len=2,.buf=b}};
    struct i2c_rdwr_ioctl_data d={.msgs=m,.nmsgs=2};
    if(ioctl(i2cfd,I2C_RDWR,&d)<0) return -1;
    *val=(b[0]<<8)|b[1]; return 0;
}
/* registers that matter for the capture chain + clocking */
static const unsigned char REGS[] = {0x00,0x0d,0x1d,0x27,0x28,0x29,0x2a,0x2f,0x3b,0x3c,0x45,
                                     0x61,0x62,0x63,0x64,0x65,0x6a,0x6c,0x70,0x73,0x80,0x81,0x82,0xc6};
static void snap(unsigned *out){ for(unsigned i=0;i<sizeof REGS;i++) if(rd(REGS[i],&out[i])) out[i]=0xffff; }
static void diff(const char*la,const unsigned*a,const char*lb,const unsigned*b){
    int n=0;
    for(unsigned i=0;i<sizeof REGS;i++) if(a[i]!=b[i]){
        printf("      reg %02x: %04x (%s) -> %04x (%s)\n",REGS[i],a[i],la,b[i],lb); n++; }
    if(!n) printf("      (no register changes)\n");
}
static double level(struct pcm *p,int frames){
    static short buf[PER*CH]; double sq=0; int n=0,peak=0;
    for(int f=0;f<frames;f++){
        if(pcm_read(p,buf,PER*CH*2)) break;
        for(int i=0;i<PER*CH;i++){ int v=buf[i]<0?-buf[i]:buf[i]; if(v>peak)peak=v; sq+=(double)buf[i]*buf[i]; n++; }
    }
    printf("      peak=%-6d rms=%.1f\n",peak,n?sqrt(sq/n):0.0);
    return n?sqrt(sq/n):0.0;
}
int main(int argc,char**argv){
    int order = argc>1?atoi(argv[1]):0;   /* 0 = capture first, 1 = playback first */
    i2cfd=open("/dev/i2c-4",O_RDWR);
    struct pcm_config c; memset(&c,0,sizeof c);
    c.channels=CH; c.rate=RATE; c.period_size=PER; c.period_count=6; c.format=PCM_FORMAT_S16_LE;
    unsigned s0[sizeof REGS], s1[sizeof REGS], s2[sizeof REGS];
    struct pcm *cap=NULL,*play=NULL;

    printf("=== open order: %s ===\n", order? "PLAYBACK first" : "CAPTURE first");
    if(order){ play=pcm_open(0,1,PCM_OUT,&c); printf("  playback opened\n"); }
    cap=pcm_open(0,0,PCM_IN,&c);
    if(!cap||!pcm_is_ready(cap)){ printf("capture open failed\n"); return 1; }

    printf("  [1] capture only, settling\n"); level(cap,25); snap(s0); level(cap,25);
    printf("  [1] baseline:\n"); snap(s0); level(cap,25);

    if(!order){ play=pcm_open(0,1,PCM_OUT,&c); printf("  [2] playback OPENED (no audio written yet)\n"); }
    else       printf("  [2] playback already open\n");
    snap(s1); level(cap,25);
    printf("    regs after playback open:\n"); diff("before",s0,"after",s1);

    if(play && pcm_is_ready(play)){
        static short sil[PER*CH];
        double ph=0, st=2*M_PI*1000.0/RATE;
        int tone = getenv("TONE") != NULL;
        for(int i=0;i<PER;i++,ph+=st){ short v = tone ? (short)(0.6*22000*sin(ph)) : 0; sil[i*CH]=v; sil[i*CH+1]=v; }
        for(int i=0;i<10;i++) pcm_write(play,sil,sizeof sil);
        printf("  [3] playback WRITING (%s):\n", tone?"1 kHz TONE":"silence");
        snap(s2); level(cap,25);
        printf("    regs after writing:\n"); diff("open",s1,"writing",s2);
    }
    if(play) pcm_close(play);
    pcm_close(cap);
    return 0;
}
