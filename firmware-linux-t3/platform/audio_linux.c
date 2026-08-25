/* Linux audio backend for the T3 (RK3188) — Stage 1: local playback (chimes,
 * beeps, doorbell, announcements) via tinyalsa on the RT3261 codec.
 *
 * Implements the shared audio.h contract (firmware-idf/main/audio.h) that the app
 * calls on MMK_HAS_AUDIO boards. The ESP version drives an ES8311 over I2S; here
 * we open /dev/snd/pcmC0D0p through tinyalsa and reproduce the exact validated
 * codec route captured on stock Android (mixer_route.h) at start.
 *
 * Stage 2 (SIP intercom) will add the mic-capture + G.711 call bridge; the
 * call/ring/read/write entry points below are minimal stubs until then. */
#include "audio.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tinyalsa/pcm.h"
#include "tinyalsa/mixer.h"
#include "mixer_route.h"

/* libre/librem: G.711 u-law coding + the 48k<->8k resampler. Header-only for
 * g711; auresamp is linked from thirdparty/libre/libre.a (tools/build-libre.sh). */
#include <re.h>
#include <rem.h>

#define SND_CARD   0
/* PLAYBACK IS dev0 (rt3261-aif1 -> DAC1), NOT dev1.
 *
 * This was backwards for a long time and cost the panel ALL audio -- no tones,
 * no chime, no ring, and silent SIP calls -- because dev1 opens cleanly, accepts
 * every write and reports no error while producing nothing. Measured on hardware
 * (tone played while capturing on the mic, RMS idle -> playing):
 *
 *     dev1 (aif2/DAC2):  331 ->   88   silent
 *     dev0 (aif1/DAC1):  307 -> 8383   audible   <-- and confirmed by ear
 *
 * The old comment claimed the speaker was analog-wired to DAC2 and reachable
 * only via aif2. It is reachable from DAC1 through "SPOL MIX DAC Switch", which
 * the driver-reset state already has enabled.
 *
 * Capture is dev0 too. A PCM device has independent playback/capture substreams,
 * so this is fine -- they share the aif1 DAI and therefore must run at the SAME
 * rate, which is why everything here is pinned to 48 kHz. Verified full-duplex:
 * the mic captured the tone while dev0 was playing it. */
#define SND_DEV    0
// Capture is the OTHER DAI: dev0 = rt3261-aif1. Verified against stock, which
// opens exactly /dev/snd/pcmC0D0c while recording.
#define CAP_DEV    0
// 48000 is NOT a free choice on device 1. The machine driver's rt3261_voice_hw_params
// pins that link's codec SYSCLK to a fixed 24.576 MHz, and rt3261_hw_params -> get_clk_info
// demands sysclk == rate * 256 * pd for pd in {1,2,3,4,6,8,12,16}. 24576000/(44100*256)
// = 2.177, so 44.1k (and 22.05k/11.025k) can NEVER open here -- the kernel rejects them
// with "Unsupported clock setting" / "can't set codec rt3261-aif2 hw params". The only
// rates that satisfy both the codec and the RK29 I2S rate mask are 48000, 16000, 8000.
#define RATE_HZ    48000
#define CHANS      2

static bool          s_ready;
static uint8_t       s_vol = 80;      // speaker volume 0..100 (scales output amplitude)
static uint8_t       s_user_vol = 80; // Sound > Volume: master level, survives a call
static uint8_t       s_ring_vol = 90; // chime/ring loudness 0..100
static pthread_mutex_t s_play_lock = PTHREAD_MUTEX_INITIALIZER;  // one playback at a time
static volatile bool s_async_busy;    // an async chime/doorbell task is running
static bool          s_in_call;       // a SIP call holds the playback PCM open

// ── capture / mic ──────────────────────────────────────────────────────────
// WORKING since 2026-07-19. The layout, all hardware-verified:
//
//   CAPTURE  = card0 dev0 (rt3261-aif1)     <- CAP_DEV
//   PLAYBACK = card0 dev0 (rt3261-aif1)     <- SND_DEV (DAC1 -> SPOL MIX -> speaker)
//
// Playback and capture are the SAME DAI (aif1), so both must run at the same rate
// -- everything here is 48 kHz. Full duplex on it is verified: the mic captured a
// tone while dev0 was playing it.
//
// Capture MUST run at 48 kHz. The driver accepts an 8 kHz capture open and then
// silently does not honour it: 10 reads of 320 frames complete in 69 ms against
// 400 ms expected. 48 kHz is correctly hardware-paced (102 ms vs 100 ms). Channels
// are fixed at 2; a mono pcm_open is rejected.
//
// THE MIC WAS KILLED BY OUR OWN apply_mixer_route(). The mixer_route.h snapshot was
// captured from stock while IDLE -- capture torn down -- so replaying it left the
// codec somewhere a capture stream cannot start from, and every read returned exact
// digital zero. Leaving the codec in its DRIVER-RESET state fixes it, and playback
// is unaffected (the playback fix was always device+rate, never the route).
// Proof, on an ANALOG control, which is the only test that distinguishes a live
// mic from an artifact:
//
//     IN1 Boost    0     4     8
//     peak        28    61   113     (no replay)   <- monotonic == real analog path
//     rms        0.0   0.0   0.0     (replay applied)
//
// The codec's hardware AEC is ON by default and we want it: DSP Function Switch =
// AEC+NS+FENS in the driver-reset state gives 24-33 dB of echo suppression
// (measured: mic RMS while the speaker plays drops 4722 -> 299 on the 7",
// 22343 -> 495 on the 10"). The mic path runs THROUGH that DSP -- stock routes it
// via IF2 ADC R Mux = TxDP -- so "bypassing the DSP" cuts capture entirely. Do not.
//
// HOW TO TEST, because signal level alone misled repeatedly here:
//   1. gain sensitivity -- RMS must scale with IN1 Boost;
//   2. timing -- reading N seconds of audio must take N seconds;
//   3. per-period checksums -- successive reads must differ (a periodic signal
//      holds a stable 1-second RMS even while every period differs);
//   4. always across a REBOOT. Codec state survives an app restart, so a restart
//      shows you the PREVIOUS boot's codec state and lies to you.

// ── mixer route ─────────────────────────────────────────────────────────────
// Replay the full saved snapshot so the codec ends up in the proven playback
// config (DAC2 -> SPK MIX -> SPO -> Ext Spk, Class-D 2.77x), regardless of the
// kernel's power-on mixer defaults.
static void apply_mixer_route(void)
{
    struct mixer *m = mixer_open(SND_CARD);
    if (!m) { printf("audio: mixer_open failed\n"); return; }
    int applied = 0;
    for (int i = 0; i < MIXER_ROUTE_N; i++) {
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
    // Force the output enables ON regardless of the snapshot (it captured
    // "Speaker Playback Switch: Off"): these gate the SPKVOL -> speaker path.
    //
    // The last four are the CAPTURE route. The stock idle snapshot leaves every
    // Stereo ADC mixer switch Off, so the ADC feeds the aif2 capture stream pure
    // digital zeroes -- pcm_open succeeds, the I2S DMA runs ("i2s dma info:
    // periodsize(1280)" in dmesg), and every sample reads exactly 0.
    //
    // STATUS 2026-07-18: THIS IS NOT YET A WORKING CAPTURE ROUTE. Setting these
    // four from the shell produced real mic signal several times (peaks of ~4.6k
    // on room noise, and ~32.6k railed with the left pair off), so the hardware
    // does capture. But with the app applying exactly this set at audio_start(),
    // capture reads an exact 0 at every rate. The working readings only ever
    // appeared after long manual poke sequences, which means some control OUTSIDE
    // this set is doing the real work and the four below are incidental.
    //
    // They are left in because they are harmless and probably necessary-but-not-
    // sufficient. Do not trust this as the answer.
    //
    // HOW TO FINISH IT: get the mic producing signal, then dump all 159 controls
    // in that known-good state and diff against the state right after
    // apply_mixer_route(). The delta is the answer. Bisecting control-by-control
    // against a live app is what wasted the last round -- every app restart
    // re-applies the whole snapshot and resets the codec's power state underneath
    // the experiment, so results do not reproduce.
    static const char *force_on[] = {
        "Speaker Playback Switch", "Ext Spk Switch", "DAC2 Playback Switch",
        "HP Playback Switch",  // harmless if unused; some routes need it
        "Stereo ADC MIXL ADC1 Switch", "Stereo ADC MIXR ADC1 Switch",  // mic -> aif2
        "RECMIXL BST1 Switch", "Mono ADC MIXL ADC1 Switch",            // gain staging
    };
    for (unsigned i = 0; i < sizeof(force_on) / sizeof(force_on[0]); i++) {
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, force_on[i]);
        if (!ctl) continue;
        int n = mixer_ctl_get_num_values(ctl);
        for (int v = 0; v < n; v++) mixer_ctl_set_value(ctl, v, 1);
    }
    // Take the RT3261's voice DSP out of the capture path. Both readings where the
    // mic produced real signal had this at Disable; the run with it at the stock
    // AEC+NS+FENS measured 0. The DSP block powers up and straight back down on
    // every capture open ("rt3261_dsp_event(): PMU" then "PMD" in dmesg), i.e. it
    // is never actually initialised, so leaving it inline just mutes us.
    // TRADEOFF: this gives up the codec's hardware AEC, which a speakerphone
    // intercom genuinely wants. Revisit once two-way calls run -- if echo is bad,
    // the options are initialising the DSP properly or doing AEC in software.
    struct mixer_ctl *dsp = mixer_get_ctl_by_name(m, "DSP Function Switch");
    if (dsp) mixer_ctl_set_enum_by_string(dsp, "Disable");

    mixer_close(m);
    printf("audio: mixer route applied (%d/%d controls)\n", applied, MIXER_ROUTE_N);
}

// Set every value of a boolean mixer control (stereo switches have two).
static void set_ctl_bool(const char *name, int on)
{
    struct mixer *m = mixer_open(SND_CARD);
    if (!m) return;
    struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, name);
    if (ctl) {
        unsigned n = mixer_ctl_get_num_values(ctl);
        for (unsigned v = 0; v < n; v++) mixer_ctl_set_value(ctl, v, on ? 1 : 0);
    }
    mixer_close(m);
}

static void set_ext_spk(int on)   // "Ext Spk Switch" ~= the speaker amp enable
{
    set_ctl_bool("Ext Spk Switch", on);
}

/* Unmute the speaker output. THIS IS WHY THE T3 HAD NO SOUND AT ALL -- not
 * tones, not calls, ever.
 *
 * We deliberately skip apply_mixer_route() (replaying the stock IDLE snapshot
 * kills capture -- see the note above), and the driver-reset state gets the whole
 * chain right except for one control. Dumped off a live panel:
 *
 *     DAC2 Playback Switch      1 1     SPK MIXL DAC L2 Switch    1
 *     SPOL MIX SPKVOL L Switch  1       SPK MIXR DAC R2 Switch    1
 *     Speaker Playback Volume   31 31   Speaker Playback Switch   0 0   <-- muted
 *
 * "Speaker Playback Switch" is the final output mute and defaults OFF. It lives
 * in apply_mixer_route()'s force_on[] list, which never runs, so nothing ever
 * turned it on: playback opened cleanly, accepted every write, and was silent.
 * The earlier claim that "playback is unaffected by skipping the route" was
 * verified only indirectly (mic RMS rising) and was wrong.
 *
 * Output-only control -- it does not touch the capture path, so the mic stays
 * working. */
static void enable_speaker_out(void)
{
    set_ctl_bool("Speaker Playback Switch", 1);
}

// ── tone synthesis ──────────────────────────────────────────────────────────
typedef struct { float freq; int ms; } note_t;

// ── in-call alert injection ────────────────────────────────────────────────
// During a call the bridge owns the playback PCM (dev1). A chime or doorbell must
// NOT open dev1 again -- the second open fails or fights the call stream. Instead
// the tone is rendered here at the hardware rate (mono) and audio_call_write()
// mixes it into the outgoing call audio, so alerts are audible mid-call without a
// second stream. Rendering happens on the caller's thread; the mixer only reads,
// so a lock around the buffer swap is enough.
#define INJECT_MAX (RATE_HZ * 3)      /* up to 3 s of alert */
static int16_t s_inject[INJECT_MAX];
static int     s_inject_len, s_inject_pos;
static pthread_mutex_t s_inject_lock = PTHREAD_MUTEX_INITIALIZER;

// Render a note sequence into the injection buffer (mono, RATE_HZ), replacing
// whatever was queued. Same envelope as play_notes_dev so alerts sound identical
// in and out of a call.
static void inject_notes(const note_t *notes, int n, float amp)
{
    pthread_mutex_lock(&s_inject_lock);
    const float scale = amp * (s_vol / 100.0f) * 0.6f * 32767.0f;
    int out = 0;
    for (int k = 0; k < n && out < INJECT_MAX; k++) {
        int frames = (int)((long)RATE_HZ * notes[k].ms / 1000);
        int fade = RATE_HZ / 100;
        double phase = 0.0, dp = 2.0 * M_PI * notes[k].freq / RATE_HZ;
        for (int i = 0; i < frames && out < INJECT_MAX; i++, out++) {
            float env = 1.0f;
            if (i < fade)                 env = i / (float)fade;
            else if (i > frames - fade)   env = (frames - i) / (float)fade;
            s_inject[out] = (int16_t)(sinf((float)phase) * scale * env);
            phase += dp; if (phase > 2 * M_PI) phase -= 2 * M_PI;
        }
    }
    s_inject_len = out;
    s_inject_pos = 0;
    pthread_mutex_unlock(&s_inject_lock);
}

// Render + play a sequence of notes (S16 stereo @ RATE_HZ). amp 0..1 overall.
// A short raised-cosine fade in/out per note avoids click artifacts.
static void play_notes_dev(int dev, const note_t *notes, int n, float amp)
{
    if (!s_ready) return;
    // A call owns the playback device -- mix the alert into the call instead of
    // opening a second stream on it (which would fail or disturb the call).
    if (s_in_call) { inject_notes(notes, n, amp); return; }
    struct pcm_config cfg = {
        .channels = CHANS, .rate = RATE_HZ,
        .period_size = 1024, .period_count = 4,
        .format = PCM_FORMAT_S16_LE,
    };
    struct pcm *pcm = pcm_open(SND_CARD, dev, PCM_OUT, &cfg);
    if (!pcm || !pcm_is_ready(pcm)) {
        printf("audio: pcm_open(card%d,dev%d) failed: %s\n", SND_CARD, dev,
               pcm ? pcm_get_error(pcm) : "(null)");
        if (pcm) pcm_close(pcm);
        return;
    }
    pthread_mutex_lock(&s_play_lock);
    /* AFTER pcm_open, never before: both of these are DAPM-managed and the codec
     * clears them when the stream powers down, so anything set at startup is
     * already back to 0 by the time a tone plays (measured: 1 1 before a tone,
     * 0 0 after). Without them the tone is synthesised and written correctly and
     * is simply inaudible. */
    enable_speaker_out();
    set_ext_spk(1);
    const float scale = amp * (s_vol / 100.0f) * 0.6f * 32767.0f;
    for (int k = 0; k < n; k++) {
        int frames = (int)((long)RATE_HZ * notes[k].ms / 1000);
        int fade = RATE_HZ / 100;   // 10 ms fade
        int16_t buf[512 * CHANS];
        double phase = 0.0, dp = 2.0 * M_PI * notes[k].freq / RATE_HZ;
        int done = 0;
        while (done < frames) {
            int chunk = frames - done; if (chunk > 512) chunk = 512;
            for (int i = 0; i < chunk; i++) {
                int idx = done + i;
                float env = 1.0f;
                if (idx < fade)            env = idx / (float)fade;
                else if (idx > frames - fade) env = (frames - idx) / (float)fade;
                int16_t s = (int16_t)(sinf((float)phase) * scale * env);
                phase += dp; if (phase > 2 * M_PI) phase -= 2 * M_PI;
                buf[i * CHANS] = s; buf[i * CHANS + 1] = s;
            }
            if (pcm_writei(pcm, buf, chunk) < 0) break;
            done += chunk;
        }
    }
    pcm_close(pcm);
    /* Leave the amp on if a call started while we were sounding -- the call owns
     * it from that point (audio_call_begin/end), and switching it off here would
     * mute the call. */
    if (!s_in_call) set_ext_spk(0);
    pthread_mutex_unlock(&s_play_lock);
}
static void play_notes(const note_t *notes, int n, float amp)
{
    play_notes_dev(SND_DEV, notes, n, amp);
}

// ── public API (audio.h) ────────────────────────────────────────────────────
bool audio_start(void)
{
    // Only set the mixer route; do NOT probe-open the PCM here. Opening it at boot
    // starts the I2S DMA/clock, which on the RK3188 contends with the LCDC and makes
    // the display flicker. The PCM is opened on demand in play_notes() instead.
    //
    // DO NOT replay the 159-control mixer snapshot by default. IT BREAKS THE MIC.
    //
    // The snapshot in mixer_route.h was captured from stock while IDLE, i.e. with
    // capture torn down, so replaying it puts the codec into a state a capture
    // stream cannot start from: every read returns exact digital zero. Skipping it
    // and leaving the codec in its driver-reset state makes the mic work
    // immediately. Proven on hardware 2026-07-19 by the only test that counts --
    // gain sensitivity: with the replay skipped, IN1 Boost 0/4/8 gives
    // rms 3.0 / 14.0 / 25.2, monotonic with ANALOG gain. With the replay it is a
    // flat 0.0 at every setting.
    //
    // The replay is also unnecessary for playback: the speaker works from the
    // driver-reset state (the real playback fix was the device+rate -- aif2/PCM
    // dev1 at 48 kHz -- not the route). Verified acoustically: with playback
    // running, mic RMS rises from 8 to 94 at IN1 Boost 0, and to ~9700 at boost 6.
    //
    // Set /data/nvx/force-mixer-route to restore the old behaviour for A/B testing.
    if (access("/data/nvx/force-mixer-route", F_OK) == 0) {
        printf("audio: FORCING mixer route replay (/data/nvx/force-mixer-route) — mic will be dead\n");
        apply_mixer_route();
    }
    // Output-only, safe for capture: see enable_speaker_out().
    enable_speaker_out();
    s_ready = true;
    printf("audio: start OK (PCM opens on demand, speaker output unmuted)\n");
    return s_ready;
}
bool audio_ready(void) { return s_ready; }

void audio_set_volume(uint8_t pct)        { s_vol = pct > 100 ? 100 : pct; }
/* The Sound > Volume slider: the panel's master level, which a call must not
 * permanently overwrite. Kept separate from s_vol (which the intercom proxy
 * drives during a call) and restored by audio_call_end(). */
/* Master "Sound > Volume" level. On the T3 there is no separate call mixer to
 * override it, so this is simply the output level (parity with the ESP path). */
void audio_set_user_volume(uint8_t pct)   { s_user_vol = pct > 100 ? 100 : pct;
                                            audio_set_volume(s_user_vol); }
bool audio_in_call(void)                  { return s_in_call; }
void audio_set_ringer_volume(uint8_t pct) { s_ring_vol = pct > 100 ? 100 : pct; }
void audio_set_mic_gain(float db)         { (void)db; }   // Stage 2 (capture)

// Two-note notification chime (ding-dong), ringer-volume scaled. Blocks (~0.5s).
void audio_play_chime(void)
{
    static const note_t chime[] = { { 880.0f, 180 }, { 660.0f, 300 } };
    play_notes(chime, 2, s_ring_vol / 100.0f);
}
void audio_play_beep(void)   // single heads-up beep, blocking
{
    static const note_t beep[] = { { 1000.0f, 130 } };
    play_notes(beep, 1, s_ring_vol / 100.0f);
}
void audio_beep(void) { audio_play_beep(); }

// Doorbell: classic two-tone chime rung twice.
void audio_play_doorbell(void)
{
    static const note_t bell[] = { { 660.0f, 260 }, { 520.0f, 380 },
                                   { 660.0f, 260 }, { 520.0f, 420 } };
    play_notes(bell, 4, s_ring_vol / 100.0f);
}

// ── async wrappers (detached thread; ignore overlap while one is sounding) ──
static void *th_chime(void *a)    { (void)a; audio_play_chime();    s_async_busy = false; return 0; }
static void *th_doorbell(void *a) { (void)a; audio_play_doorbell(); s_async_busy = false; return 0; }
static void run_async(void *(*fn)(void *))
{
    if (!s_ready || s_async_busy) return;
    s_async_busy = true;
    pthread_t t;
    if (pthread_create(&t, NULL, fn, NULL) == 0) pthread_detach(t);
    else s_async_busy = false;
}
void audio_chime_async(void)    { run_async(th_chime); }
void audio_doorbell_async(void) { run_async(th_doorbell); }

// Self-test (bring-up / field diagnostic, mirrors the ESP-IDF version):
//   1) chime            -> speaker + amp (listen)
//   2) ~5s mic->speaker loopback (raw, same rate, no resample) -> both at once
//      (talk, hear yourself). Uses the same PCM devices/order as the SIP call
//      bridge (playback opened before capture -- see audio_call_begin for why),
//      just without the G.711/resample step since this never leaves the box.
// Bails early (silently) if a real call starts mid-test.
static void *th_selftest(void *a)
{
    (void)a;
    audio_play_chime();
    usleep(300000);

    if (s_in_call) { s_async_busy = false; return 0; }   // don't fight a live call

    struct pcm_config cfg = {
        .channels = CHANS, .rate = RATE_HZ,
        .period_size = 1024, .period_count = 4,
        .format = PCM_FORMAT_S16_LE,
    };
    struct pcm *play = pcm_open(SND_CARD, SND_DEV, PCM_OUT, &cfg);
    struct pcm *cap  = (play && pcm_is_ready(play)) ? pcm_open(SND_CARD, CAP_DEV, PCM_IN, &cfg) : NULL;
    if (play && pcm_is_ready(play) && cap && pcm_is_ready(cap)) {
        set_ext_spk(1);
        static int16_t buf[1024 * CHANS];
        long peak = 0, frames = 0;
        int i = 0;
        printf("audio: selftest loopback started (speak now, ~5s)\n");
        for (; i < 5 * RATE_HZ / 1024 && !s_in_call; i++) {
            if (pcm_read(cap, buf, sizeof(buf))) {
                printf("audio: selftest pcm_read failed at block %d: %s\n",
                       i, pcm_get_error(cap));
                break;
            }
            /* Report what the mic actually heard: "no audio" and "mic is dead"
             * are indistinguishable from the outside otherwise. */
            for (unsigned k = 0; k < sizeof(buf) / sizeof(buf[0]); k++) {
                long v = buf[k] < 0 ? -(long)buf[k] : buf[k];
                if (v > peak) peak = v;
            }
            frames++;
            if (pcm_write(play, buf, sizeof(buf))) {
                printf("audio: selftest pcm_write failed at block %d: %s\n",
                       i, pcm_get_error(play));
                break;
            }
        }
        set_ext_spk(0);
        printf("audio: selftest done — %ld blocks, mic peak=%ld/32767 %s\n",
               frames, peak,
               peak < 100 ? "(SILENT — mic not capturing)" : "(mic OK)");
    } else {
        printf("audio: selftest loopback open failed (play=%p cap=%p)\n", (void *)play, (void *)cap);
    }
    if (cap)  pcm_close(cap);
    if (play) pcm_close(play);

    s_async_busy = false;
    return 0;
}
void audio_selftest_async(void) { run_async(th_selftest); }

void audio_amp(bool on) { set_ext_spk(on); }

// ── ring / call bridge — Stage 2 (SIP) stubs ────────────────────────────────
/* Repeating ring cadence on its own thread, per the audio.h contract. This used
 * to be a single audio_chime_async() with a no-op stop, so an incoming intercom
 * call produced one ~0.5s ding and then silence -- indistinguishable from "it
 * never rang", which is exactly how it was reported. (It was inaudible entirely
 * until playback moved to dev0.) */
static volatile bool s_ring_run;
static bool          s_ring_door;
static bool          s_ring_back;   /* outgoing ringback rather than incoming ring */

static void *th_ring(void *a)
{
    (void)a;
    while (s_ring_run) {
        if (s_ring_back) {
            /* US-style ringback: a ~1s burst, then a long gap. Deliberately not
             * the ding-dong chime -- the caller hearing the doorbell they just
             * rang is confusing. */
            static const note_t rb[] = { { 440.0f, 500 }, { 480.0f, 500 } };
            play_notes(rb, 2, (s_ring_vol / 100.0f) * 0.7f);
            for (int i = 0; i < 30 && s_ring_run; i++) usleep(100000);
        } else {
            if (s_ring_door) audio_play_doorbell(); else audio_play_chime();
            /* Gap between rings, polled so a stop lands promptly. */
            for (int i = 0; i < 12 && s_ring_run; i++) usleep(100000);
        }
    }
    return NULL;
}

static void ring_thread_start(bool door, bool ringback)
{
    if (!s_ready || s_ring_run) return;
    s_ring_door = door;
    s_ring_back = ringback;
    s_ring_run  = true;
    pthread_t t;
    if (pthread_create(&t, NULL, th_ring, NULL) == 0) pthread_detach(t);
    else s_ring_run = false;
}

void audio_ringback_start(void) { ring_thread_start(false, true); }
void audio_ringback_stop(void)  { s_ring_run = false; }

void audio_ring_start_ex(bool door) { ring_thread_start(door, false); }
void audio_ring_start(void) { audio_ring_start_ex(false); }

/* Does NOT block until the tone thread exits, unlike the ESP-IDF version: the
 * callers are the LVGL and libre threads, and a note in progress would stall
 * them for up to ~0.5s. The thread stops at the next note boundary; the flag
 * also blocks a restart, so no two ring threads can overlap. */
void audio_ring_stop(void) { s_ring_run = false; }
int  audio_read(void *b, int n)      { (void)b; return n; }
int  audio_write(const void *b, int n){ (void)b; return n; }
// Returns false: capture does not work on this kernel (see the capture/mic note
// above), so there is no two-way call path yet. Outbound-only audio -- playing an
// announcement or a doorbell chime out the speaker -- does work and does not go
// through here.
// NOTE for the future bridge: playback and capture share the aif2 DAI, so BOTH
// directions must run at the SAME rate. Opening capture at 8k while playback ran
// at 48k fails outright with "cannot set hw params". G.711 wants 8k, so open both
// at 8k.
// ── SIP call bridge: mic <-> G.711, speaker <-> G.711 ───────────────────────
// Hardware truths this is built on, all measured (see the T3 teardown README):
//   - CAPTURE is card0 dev0 (aif1); PLAYBACK is card0 dev1 (aif2). They are
//     DIFFERENT DAIs, so each can run at its own rate and both can be open at once.
//   - Capture MUST run at 48 kHz. The driver accepts an 8 kHz capture open and
//     then does not honour it (reads come back ~6x too fast), so we capture at
//     48 kHz and resample to G.711's 8 kHz in software. Channels are fixed at 2 --
//     a mono pcm_open is rejected outright.
//   - Do NOT touch the mixer here. Replaying the idle snapshot is what used to
//     kill capture, and the codec's driver-reset state already has the hardware
//     AEC engaged (DSP Function Switch = AEC+NS+FENS, worth 24-33 dB). The mic
//     path runs THROUGH that DSP, so disabling it also cuts capture.
//
// 48000/8000 is exactly 6, and librem's resampler picks fir_48_4 for this ratio
// (pass 0-3 kHz, stop 5-24 kHz) -- the right voice-band anti-alias filter.
#define CALL_HW_RATE   48000
#define CALL_HW_CH     2
#define CALL_SIP_RATE  8000
#define CALL_RATIO     (CALL_HW_RATE / CALL_SIP_RATE)   /* 6 */
// One 20 ms RTP frame: 160 u-law bytes <-> 960 hw frames. Sizing the ALSA period
// to match keeps every read/write a whole packet, with no straddling.
#define CALL_SIP_SAMPS 160
#define CALL_HW_FRAMES (CALL_SIP_SAMPS * CALL_RATIO)    /* 960 */

static struct pcm      *s_cap, *s_play;
static struct auresamp  s_rs_in, s_rs_out;   /* mic 48k->8k, spk 8k->48k */

static struct pcm *call_pcm_open(int dev, unsigned flags)
{
    struct pcm_config cfg = {
        .channels = CALL_HW_CH, .rate = CALL_HW_RATE,
        .period_size = CALL_HW_FRAMES, .period_count = 6,
        .format = PCM_FORMAT_S16_LE,
    };
    struct pcm *p = pcm_open(SND_CARD, dev, flags, &cfg);
    if (p && pcm_is_ready(p)) return p;
    printf("audio: call pcm_open(dev%d,%s) failed: %s\n", dev,
           (flags & PCM_IN) ? "in" : "out", p ? pcm_get_error(p) : "null");
    if (p) pcm_close(p);
    return NULL;
}

bool audio_call_begin(int rate)
{
    if (s_in_call) return true;
    if (rate && rate != CALL_SIP_RATE)
        printf("audio: call rate %d requested, bridging via %d\n", rate, CALL_SIP_RATE);

    auresamp_init(&s_rs_in);
    auresamp_init(&s_rs_out);
    if (auresamp_setup(&s_rs_in,  CALL_HW_RATE, CALL_HW_CH, CALL_SIP_RATE, 1) ||
        auresamp_setup(&s_rs_out, CALL_SIP_RATE, 1, CALL_HW_RATE, CALL_HW_CH)) {
        printf("audio: call resampler setup failed\n");
        return false;
    }

    // ORDER MATTERS: open PLAYBACK first, then capture. Opening playback while a
    // capture stream is already running degrades capture badly -- measured on
    // hardware, mic RMS drops ~20.6 -> 8.1 and stops responding to sound at all.
    // Opening playback first leaves capture intact (21.3 -> 21.1) and the mic then
    // tracks the speaker normally (RMS rises to 30.6 on a 1 kHz tone). Opening
    // playback flips the ADDA clock divider (codec reg 0x73: 1114 -> 6614), which
    // an already-running capture stream evidently does not survive.
    s_play = call_pcm_open(SND_DEV, PCM_OUT);
    s_cap  = call_pcm_open(CAP_DEV, PCM_IN);
    if (!s_cap || !s_play) { audio_call_end(); return false; }

    pthread_mutex_lock(&s_inject_lock);
    s_inject_len = s_inject_pos = 0;      /* no stale alert into a fresh call */
    pthread_mutex_unlock(&s_inject_lock);

    /* After the streams are open: DAPM clears these on power-down, so enabling
     * them any earlier is lost (see play_notes_dev). This is why call audio was
     * silent too, not just tones. */
    enable_speaker_out();
    set_ext_spk(1);
    s_in_call = true;
    printf("audio: call up (cap dev%d / play dev%d @ %d Hz, G.711 @ %d Hz)\n",
           CAP_DEV, SND_DEV, CALL_HW_RATE, CALL_SIP_RATE);
    return true;
}

void audio_call_end(void)
{
    if (s_cap)  { pcm_close(s_cap);  s_cap  = NULL; }
    if (s_play) { pcm_close(s_play); s_play = NULL; }
    if (s_in_call) set_ext_spk(0);
    s_in_call = false;

    /* Restore the panel's own level. The intercom proxy drives s_vol during a
     * call and pushes 0 freely; without this the panel stays at whatever the
     * call left behind -- often 0 -- and every later sound is silent with no
     * indication why. Mirrors audio_call_end() in the ESP-IDF build. */
    s_vol = s_user_vol;
}

// mic -> SIP. `n` counts u-law bytes wanted; returns u-law bytes produced.
int audio_call_read(void *dst_ulaw, int n)
{
    if (!s_in_call || !s_cap || n <= 0) return -1;
    if (n > CALL_SIP_SAMPS) n = CALL_SIP_SAMPS;      /* one frame per call */

    static int16_t hw[CALL_HW_FRAMES * CALL_HW_CH];
    // NOT sized to the 8 kHz output: librem's downsampler FIR-filters the WHOLE
    // input into this buffer and only then decimates in place, so it demands
    // capacity >= the INPUT sample count (see the `*outc < inc` check in
    // rem/auresamp/resamp.c) and returns ENOMEM otherwise. Sizing it to the
    // output count is the obvious mistake, and it fails every call.
    static int16_t pcm8[CALL_HW_FRAMES * CALL_HW_CH];
    int frames = n * CALL_RATIO;

    if (pcm_read(s_cap, hw, frames * CALL_HW_CH * sizeof(int16_t)))
        return -1;

    size_t outc = (size_t)frames * CALL_HW_CH;   /* capacity in, count out */
    if (auresamp(&s_rs_in, pcm8, &outc, hw, (size_t)frames * CALL_HW_CH))
        return -1;

    uint8_t *out = dst_ulaw;
    for (size_t i = 0; i < outc; i++) out[i] = g711_pcm2ulaw(pcm8[i]);
    return (int)outc;
}

// SIP -> speaker. `n` counts u-law bytes; returns u-law bytes consumed.
int audio_call_write(const void *src_ulaw, int n)
{
    if (!s_in_call || !s_play || n <= 0) return -1;
    if (n > CALL_SIP_SAMPS) n = CALL_SIP_SAMPS;

    static int16_t pcm8[CALL_SIP_SAMPS];
    static int16_t hw[CALL_HW_FRAMES * CALL_HW_CH];
    const uint8_t *in = src_ulaw;

    for (int i = 0; i < n; i++) pcm8[i] = g711_ulaw2pcm(in[i]);

    size_t outc = sizeof(hw) / sizeof(hw[0]);
    if (auresamp(&s_rs_out, hw, &outc, pcm8, (size_t)n))
        return -1;

    // Mix in any queued alert (chime/doorbell raised mid-call). Mono source into
    // both channels, with headroom so the sum cannot clip the call audio.
    pthread_mutex_lock(&s_inject_lock);
    if (s_inject_pos < s_inject_len) {
        for (size_t i = 0; i + 1 < outc && s_inject_pos < s_inject_len; i += 2) {
            int16_t a = s_inject[s_inject_pos++];
            int l = hw[i]     / 2 + a / 2;
            int r = hw[i + 1] / 2 + a / 2;
            hw[i]     = (int16_t)(l >  32767 ?  32767 : l < -32768 ? -32768 : l);
            hw[i + 1] = (int16_t)(r >  32767 ?  32767 : r < -32768 ? -32768 : r);
        }
    }
    pthread_mutex_unlock(&s_inject_lock);

    if (pcm_write(s_play, hw, outc * sizeof(int16_t)))
        return -1;
    return n;
}
