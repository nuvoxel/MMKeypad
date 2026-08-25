/* mmk-selftest -- SSH-triggerable audio/video bring-up test for the repurposed
 * Control4 T3 (RK3188). Standalone musl binary, staged at /usr/sbin/mmk-selftest.
 *
 *   mmk-selftest audio        acoustic loopback: apply the stock-validated RT3261
 *                             mixer route, turn on the speaker amp, play a tone
 *                             out the speaker (card0 dev1) WHILE capturing the
 *                             mic (card0 dev0), then Goertzel-detect the tone in
 *                             the recording. Prints PASS/FAIL + levels and saves
 *                             the recording to /data/selftest-cap.wav so it can
 *                             be pulled over SSH and inspected. Proves DAC->amp->
 *                             speaker AND mic->ADC->capture in one shot.
 *
 *   mmk-selftest mixerdump    dump every mixer control + current value. Use it to
 *                             chase the still-unsolved capture route (audio_linux.c
 *                             TODO): get the mic producing signal, dump here, diff
 *                             against the post-apply_route state -- the delta is
 *                             the missing control.
 *
 *   mmk-selftest camera       (see mmk-selftest-camera.c) capture a frame.
 *
 * Reuses tinyalsa + platform/mixer_route.h so it matches the app's real path.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tinyalsa/mixer.h"
#include "tinyalsa/pcm.h"

#include "mixer_route.h"   /* MIXER_ROUTE[] + mixer_ctl_val_t (from platform/) */

#define CARD      0
#define PLAY_DEV  1        /* rt3261-aif2 -> DAC2 -> speaker */
#define CAP_DEV   0        /* rt3261-aif1 -> ADC (mic)       */
#define RATE      48000
#define CHANS     2
#define TONE_HZ   1000.0
#define TONE_SEC  1.5
#define CAP_SEC   2.2      /* capture longer than playback so we bracket the tone */
#define CAP_WAV   "/data/selftest-cap.wav"

int mmk_selftest_camera(int argc, char **argv);  /* mmk-selftest-camera.c */

/* ---- mixer route (mirrors audio_linux.c apply_mixer_route + set_ext_spk) ---- */

static void apply_route(void) {
    struct mixer *m = mixer_open(CARD);
    if (!m) { printf("mixer_open(card%d) failed\n", CARD); return; }
    int applied = 0, total = (int)(sizeof(MIXER_ROUTE) / sizeof(MIXER_ROUTE[0]));
    for (int i = 0; i < total; i++) {
        const mixer_ctl_val_t *c = &MIXER_ROUTE[i];
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, c->name);
        if (!ctl) continue;
        if (c->is_enum) {
            if (mixer_ctl_set_enum_by_string(ctl, c->sval) == 0) applied++;
        } else {
            int ok = 1;
            for (int v = 0; v < c->nvals; v++)
                if (mixer_ctl_set_value(ctl, v, c->ivals[v]) != 0) ok = 0;
            if (ok) applied++;
        }
    }
    /* Force the output enables + capture switches ON (the idle snapshot leaves
     * them Off). Same set the app uses -- see audio_linux.c for the long story
     * on why the capture side is necessary-but-not-yet-sufficient. */
    static const char *force_on[] = {
        "Speaker Playback Switch", "Ext Spk Switch", "DAC2 Playback Switch",
        "HP Playback Switch",
        "Stereo ADC MIXL ADC1 Switch", "Stereo ADC MIXR ADC1 Switch",
        "RECMIXL BST1 Switch", "Mono ADC MIXL ADC1 Switch",
    };
    for (unsigned i = 0; i < sizeof(force_on) / sizeof(force_on[0]); i++) {
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, force_on[i]);
        if (!ctl) continue;
        int n = mixer_ctl_get_num_values(ctl);
        for (int v = 0; v < n; v++) mixer_ctl_set_value(ctl, v, 1);
    }
    struct mixer_ctl *dsp = mixer_get_ctl_by_name(m, "DSP Function Switch");
    if (dsp) mixer_ctl_set_enum_by_string(dsp, "Disable");
    mixer_close(m);
    printf("route: applied %d/%d controls + forced output/capture enables\n",
           applied, total);
}

static void set_ext_spk(int on) {
    struct mixer *m = mixer_open(CARD);
    if (!m) return;
    struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, "Ext Spk Switch");
    if (ctl) mixer_ctl_set_value(ctl, 0, on ? 1 : 0);
    mixer_close(m);
}

/* ---- concurrent capture thread ---- */

struct cap_ctx {
    struct pcm *pcm;
    int16_t   *buf;       /* CHANS interleaved */
    int        want;      /* frames to read */
    int        got;       /* frames actually read */
    int        err;
};

static void *cap_thread(void *arg) {
    struct cap_ctx *c = arg;
    while (c->got < c->want) {
        int chunk = c->want - c->got;
        if (chunk > 1024) chunk = 1024;
        int r = pcm_readi(c->pcm, c->buf + (size_t)c->got * CHANS, chunk);
        if (r < 0) { c->err = 1; printf("capture: read error: %s\n", pcm_get_error(c->pcm)); break; }
        c->got += (r > 0) ? r : chunk;
    }
    return NULL;
}

/* ---- analysis ---- */

/* Goertzel magnitude of one channel at frequency f (normalized 0..~32767). */
static double goertzel(const int16_t *x, int nframes, int ch, double f) {
    double w = 2.0 * M_PI * f / RATE, coeff = 2.0 * cos(w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < nframes; i++) {
        double s0 = (double)x[(size_t)i * CHANS + ch] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return sqrt(power > 0 ? power : 0) / (nframes / 2.0);
}

static void write_wav(const char *path, const int16_t *x, int nframes) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("wav: cannot open %s\n", path); return; }
    unsigned data = (unsigned)nframes * CHANS * 2, sr = RATE;
    unsigned char h[44];
    memcpy(h, "RIFF", 4);          *(unsigned *)(h + 4) = 36 + data;
    memcpy(h + 8, "WAVEfmt ", 8);  *(unsigned *)(h + 16) = 16;
    *(unsigned short *)(h + 20) = 1; *(unsigned short *)(h + 22) = CHANS;
    *(unsigned *)(h + 24) = sr;    *(unsigned *)(h + 28) = sr * CHANS * 2;
    *(unsigned short *)(h + 32) = CHANS * 2; *(unsigned short *)(h + 34) = 16;
    memcpy(h + 36, "data", 4);     *(unsigned *)(h + 40) = data;
    fwrite(h, 1, 44, f);
    fwrite(x, 2, (size_t)nframes * CHANS, f);
    fclose(f);
    printf("wav: wrote %s (%d frames, %.2fs)\n", path, nframes, (double)nframes / RATE);
}

static int audio_loopback(void) {
    printf("=== mmk-selftest audio: acoustic loopback (play + mic capture) ===\n");
    apply_route();
    set_ext_spk(1);

    struct pcm_config cfg = {
        .channels = CHANS, .rate = RATE,
        .period_size = 1024, .period_count = 4,
        .format = PCM_FORMAT_S16_LE,
    };
    struct pcm *play = pcm_open(CARD, PLAY_DEV, PCM_OUT, &cfg);
    struct pcm *cap  = pcm_open(CARD, CAP_DEV,  PCM_IN,  &cfg);
    if (!play || !pcm_is_ready(play)) {
        printf("FAIL: playback pcm_open(card%d dev%d): %s\n", CARD, PLAY_DEV,
               play ? pcm_get_error(play) : "(null)");
        return 2;
    }
    if (!cap || !pcm_is_ready(cap)) {
        printf("FAIL: capture pcm_open(card%d dev%d): %s  (app holding it? kill mmkeypad)\n",
               CARD, CAP_DEV, cap ? pcm_get_error(cap) : "(null)");
        return 2;
    }

    int play_frames = (int)(TONE_SEC * RATE);
    int cap_frames  = (int)(CAP_SEC  * RATE);
    int16_t *rec  = calloc((size_t)cap_frames * CHANS, sizeof(int16_t));
    int16_t *tone = malloc((size_t)play_frames * CHANS * sizeof(int16_t));
    if (!rec || !tone) { printf("FAIL: OOM\n"); return 2; }

    /* build the tone (S16 stereo, ~0.5 FS, 10ms fades) */
    double ph = 0, dp = 2.0 * M_PI * TONE_HZ / RATE;
    int fade = RATE / 100;
    for (int i = 0; i < play_frames; i++) {
        double env = 1.0;
        if (i < fade) env = (double)i / fade;
        else if (i > play_frames - fade) env = (double)(play_frames - i) / fade;
        int16_t s = (int16_t)(sin(ph) * 0.5 * 32767 * env);
        ph += dp; if (ph > 2 * M_PI) ph -= 2 * M_PI;
        tone[i * CHANS] = s; tone[i * CHANS + 1] = s;
    }

    /* start capturing first so the tone lands inside the recording */
    struct cap_ctx cc = { .pcm = cap, .buf = rec, .want = cap_frames };
    pthread_t th;
    pthread_create(&th, NULL, cap_thread, &cc);
    usleep(150 * 1000);   /* let capture prime */

    printf("playing %.1fs @ %.0f Hz, capturing %.1fs ...\n", TONE_SEC, TONE_HZ, CAP_SEC);
    int done = 0;
    while (done < play_frames) {
        int chunk = play_frames - done; if (chunk > 1024) chunk = 1024;
        if (pcm_writei(play, tone + (size_t)done * CHANS, chunk) < 0) {
            printf("playback: write error: %s\n", pcm_get_error(play)); break;
        }
        done += chunk;
    }
    pthread_join(th, NULL);
    set_ext_spk(0);
    pcm_close(play); pcm_close(cap);

    /* analyse the captured left channel */
    int n = cc.got;
    long peak = 0; double sumsq = 0;
    for (int i = 0; i < n; i++) {
        int v = rec[(size_t)i * CHANS];
        long a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        sumsq += (double)v * v;
    }
    double rms  = n ? sqrt(sumsq / n) : 0;
    double tmag = goertzel(rec, n, 0, TONE_HZ);
    double n1   = goertzel(rec, n, 0, TONE_HZ * 0.5);
    double n2   = goertzel(rec, n, 0, TONE_HZ * 1.73);
    double noise = (n1 + n2) / 2.0 + 1e-9;
    double ratio = tmag / noise;

    write_wav(CAP_WAV, rec, n);
    printf("\n--- results ---\n");
    printf("captured      : %d frames (%.2fs)\n", n, (double)n / RATE);
    printf("mic peak      : %ld / 32767\n", peak);
    printf("mic rms       : %.0f\n", rms);
    printf("tone@%.0fHz mag: %.1f   off-band: %.1f   ratio: %.1fx\n",
           TONE_HZ, tmag, noise, ratio);

    int rc;
    if (peak == 0) {
        printf("VERDICT: FAIL -- capture reads all zeros (capture ROUTE dead; playback\n"
               "         may still be fine -- listen for the tone). See audio_linux.c TODO.\n");
        rc = 1;
    } else if (ratio >= 4.0 && tmag > 50) {
        printf("VERDICT: PASS -- tone detected in the mic recording. Speaker + mic BOTH work.\n");
        rc = 0;
    } else {
        printf("VERDICT: PARTIAL -- mic captures signal (peak %ld) but the tone isn't\n"
               "         dominant. Either the speaker isn't sounding (amp/route) or the\n"
               "         mic isn't acoustically hearing it. Inspect %s.\n", peak, CAP_WAV);
        rc = 1;
    }
    free(rec); free(tone);
    return rc;
}

/* ---- mixer dump (capture-route debugging aid) ---- */

static int mixer_dump(void) {
    struct mixer *m = mixer_open(CARD);
    if (!m) { printf("mixer_open failed\n"); return 2; }
    unsigned n = mixer_get_num_ctls(m);
    printf("=== mixer dump: %u controls ===\n", n);
    for (unsigned i = 0; i < n; i++) {
        struct mixer_ctl *c = mixer_get_ctl(m, i);
        if (!c) continue;
        const char *name = mixer_ctl_get_name(c);
        unsigned nv = mixer_ctl_get_num_values(c);
        enum mixer_ctl_type t = mixer_ctl_get_type(c);
        printf("%-40s ", name);
        if (t == MIXER_CTL_TYPE_ENUM) {
            int idx = mixer_ctl_get_value(c, 0);
            const char *s = mixer_ctl_get_enum_string(c, idx);
            printf("= [%d] %s\n", idx, s ? s : "?");
        } else {
            for (unsigned v = 0; v < nv && v < 2; v++) printf("%d ", mixer_ctl_get_value(c, v));
            printf("\n");
        }
    }
    mixer_close(m);
    return 0;
}

/* ---- mic-only capture (measure the mic in the CURRENT codec state) ---- */

static int mic_only(int seconds, int do_route, int dev) {
    if (do_route) { apply_route(); }
    struct pcm_config cfg = {
        .channels = CHANS, .rate = RATE,
        .period_size = 1024, .period_count = 4,
        .format = PCM_FORMAT_S16_LE,
    };
    printf("mic: capturing card%d dev%d ...\n", CARD, dev);
    struct pcm *cap = pcm_open(CARD, dev, PCM_IN, &cfg);
    if (!cap || !pcm_is_ready(cap)) {
        printf("FAIL: capture pcm_open: %s\n", cap ? pcm_get_error(cap) : "(null)");
        return 2;
    }
    int frames = seconds * RATE;
    int16_t *rec = calloc((size_t)frames * CHANS, sizeof(int16_t));
    int got = 0;
    while (got < frames) {
        int chunk = frames - got; if (chunk > 1024) chunk = 1024;
        int r = pcm_readi(cap, rec + (size_t)got * CHANS, chunk);
        if (r < 0) { printf("read err: %s\n", pcm_get_error(cap)); break; }
        got += (r > 0) ? r : chunk;
    }
    pcm_close(cap);
    long peakL = 0, peakR = 0; double sq = 0;
    for (int i = 0; i < got; i++) {
        int l = rec[(size_t)i * CHANS], r = rec[(size_t)i * CHANS + 1];
        long al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        if (al > peakL) peakL = al; if (ar > peakR) peakR = ar;
        sq += (double)l * l;
    }
    write_wav(CAP_WAV, rec, got);
    printf("mic: %d frames, peakL=%ld peakR=%ld rms=%.0f  (route=%s)\n",
           got, peakL, peakR, got ? sqrt(sq / got) : 0, do_route ? "applied" : "as-is");
    free(rec);
    return (peakL > 20 || peakR > 20) ? 0 : 1;
}

/* ---- poke a single mixer control (codec-route experiments) ---- */

static int ctl_set(int argc, char **argv, int is_enum) {
    if (argc < 2) { printf("usage: set|setenum \"Control Name\" <val...>\n"); return 2; }
    struct mixer *m = mixer_open(CARD);
    if (!m) { printf("mixer_open failed\n"); return 2; }
    struct mixer_ctl *c = mixer_get_ctl_by_name(m, argv[0]);
    if (!c) { printf("control not found: %s\n", argv[0]); mixer_close(m); return 2; }
    if (is_enum) {
        int rc = mixer_ctl_set_enum_by_string(c, argv[1]);
        printf("setenum \"%s\" = \"%s\" -> %s\n", argv[0], argv[1], rc == 0 ? "ok" : "FAIL");
    } else {
        int nv = mixer_ctl_get_num_values(c);
        for (int v = 0; v < nv; v++) {
            const char *sv = (v + 1 < argc) ? argv[v + 1] : argv[argc - 1];
            mixer_ctl_set_value(c, v, atoi(sv));
        }
        printf("set \"%s\" =", argv[0]);
        for (int v = 0; v < nv; v++) printf(" %d", mixer_ctl_get_value(c, v));
        printf("\n");
    }
    mixer_close(m);
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "audio"))     return audio_loopback();
    if (!strcmp(cmd, "mixerdump")) return mixer_dump();
    if (!strcmp(cmd, "mic"))       return mic_only(argc > 2 ? atoi(argv[2]) : 2, 0, argc > 3 ? atoi(argv[3]) : CAP_DEV);
    if (!strcmp(cmd, "mic-route")) return mic_only(argc > 2 ? atoi(argv[2]) : 2, 1, argc > 3 ? atoi(argv[3]) : CAP_DEV);
    if (!strcmp(cmd, "set"))       return ctl_set(argc - 1, argv + 1, 0);
    if (!strcmp(cmd, "setenum"))   return ctl_set(argc - 1, argv + 1, 1);
    if (!strcmp(cmd, "camera"))    return mmk_selftest_camera(argc - 1, argv + 1);
    printf("usage: mmk-selftest <audio|mic [sec]|mic-route [sec]|set NAME v..|setenum NAME str|mixerdump|camera>\n");
    return 2;
}
