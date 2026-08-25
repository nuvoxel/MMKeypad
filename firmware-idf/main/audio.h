#pragma once
#include <stdbool.h>
#include <stdint.h>

// ── Phase-3 audio: ES8311 codec (mic ADC + speaker DAC) over I2S ─────────────
// Shares the touch I2C bus (bsp_i2c_bus()) for codec control. audio_start()
// brings up I2S + the codec and opens it at 16 kHz; the speaker amp is left
// DISABLED until something plays. Call once, after bsp_display_start().
// Returns true on success (false leaves audio_ready() == false; callers no-op).
bool audio_start(void);
bool audio_ready(void);

// Speaker output volume 0..100% (DAC) and mic gain in dB. Applied immediately.
void audio_set_volume(uint8_t pct);
// Settings "Sound > Volume" — the panel's master output level (see audio.c).
void audio_set_user_volume(uint8_t pct);
void audio_set_mic_gain(float db);
// Ringer/chime loudness 0..100% (scales the announcement/door chime amplitude).
void audio_set_ringer_volume(uint8_t pct);

// Looping incoming-call ring tone. audio_ring_start() turns the amp on and plays
// a repeating ring cadence (ringer-volume scaled) on its own task until
// audio_ring_stop() (which blocks until the tone task exits but leaves the amp as
// it is, so the caller owns amp-off on decline vs hand-off to call audio).
// Outgoing-call RINGBACK: what the CALLER hears while the far end rings. Distinct
// from audio_ring_start() (which is what the CALLEE hears). Placing a call used to
// produce no audio at all on either platform -- the UI showed "outgoing" and the
// speaker stayed silent, so a call in progress was indistinguishable from a dead
// one. Stopped by audio_ringback_stop(), and implicitly on answer/teardown.
void audio_ringback_start(void);
void audio_ringback_stop(void);
void audio_ring_start(void);
// Like audio_ring_start() but doorbell=true rings with the door chime sample
// (used when the incoming call is a Control4 door station).
void audio_ring_start_ex(bool doorbell);
void audio_ring_stop(void);

// Single short confirmation beep (e.g. call connected). Non-blocking; manages the
// amp itself. Ringer-volume scaled. Safe before audio_ready() (no-op).
void audio_beep(void);

// Play a short two-note notification chime on the speaker. Blocks (~0.6s) and
// manages the amp (enable -> play -> disable). Safe to call before audio_ready()
// (no-op). Used by the Control4 announcement path.
void audio_play_chime(void);

// Non-blocking chime: plays audio_play_chime() on a one-shot task. Overlapping
// calls while a chime is already sounding are ignored. Call from any task.
void audio_chime_async(void);

// Doorbell sound (linphone "oldphone" telephone ring, the stock C4-softphone sample —
// see CMakeLists/doorbell.pcm). Blocking + non-blocking task version. Used by Play Door
// Chime, and by the door-station ring (audio_ring_start_ex(true)).
void audio_play_doorbell(void);
void audio_doorbell_async(void);

// Blocking single heads-up beep (used by the auto-answer path so it actually sounds
// BEFORE the call audio takes over the codec). Manages the amp.
void audio_play_beep(void);

// Non-blocking audio self-test (bring-up / field diagnostic): chime + tone
// (speaker), 2s mic capture with peak/RMS logged (mic), then 5s mic->speaker
// loopback. Logs progress. Triggered by the `audiotest` protocol message.
void audio_selftest_async(void);

// Raw codec I/O. Buffers are 16-bit PCM at the codec's open format — always
// 16 kHz stereo (mic on the LEFT slot). Return bytes processed, <0 on error.
int  audio_read(void *buf, int bytes);
int  audio_write(const void *buf, int bytes);
void audio_amp(bool on);     // FM8002E speaker amp (active-low); off at boot

// Mark the start/end of a voice call: enable the amp + set call gains
// (audio_call_end silences the amp and restores UI gains). The codec stays in
// its proven 16 kHz stereo config the whole time — NO reopen — so the mic keeps
// reading the LEFT slot exactly as the self-test loopback proves it does.
// `sample_rate` is the SIP-side PCM rate (8000 for G.711); kept for the log only.
bool audio_call_begin(int sample_rate);
// True between audio_call_begin() and audio_call_end() -- lets the sipvol handler
// tell a genuine in-call override from an idle push.
bool audio_in_call(void);
void audio_call_end(void);

// Call-path codec bridge. esp_rtc does ONLY RTP framing (not sample coding), so
// its send/receive_audio callbacks exchange RAW G.711 µ-law bytes (1 byte/sample
// @ 8 kHz for PCMU). These do the µ-law encode/decode AND the 8k<->16k resample +
// mono<->stereo mapping against the codec's 16 kHz stereo frames. `n` counts
// µ-law bytes; return µ-law bytes processed, <0 on error.
int  audio_call_read(void *dst_ulaw, int n);          // mic   -> SIP (encode)
int  audio_call_write(const void *src_ulaw, int n);   // SIP   -> speaker (decode)
