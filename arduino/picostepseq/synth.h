/**
 * synth.h -- tiny gritty mono synth voice + stereo ping-pong delay for picostepseq
 *
 * Signal flow:
 *   model (saw/square/pulse/saw+sub/FM2/FM3/FBFM/PD) -> drive/clip -> resonant LPF
 *     -> amp env -> bitcrush -> (dry, centre) + stereo ping-pong delay -> L/R
 *
 * - Oscillators are intentionally NOT band-limited (aliasing = grit)
 * - One Attack/Decay envelope drives amplitude, filter cutoff and (via the Env
 *   amount) the FM index / PD amount ("pluck")
 * - Step gate ends the note early with a short release
 * - Audio runs in integer math, envelope/filter/model coefficients are computed
 *   once per block (control rate) in float
 *
 * Threading: synth_note_on()/synth_note_off() may be called from either core,
 * they only post events. synth_render_block() must only be called from one core.
 *
 * This file has no Arduino dependencies so the DSP can be tested on a desktop.
 */

#include <stdint.h>
#include <math.h>

const int synth_sample_rate = 22050;  // low-ish on purpose: lo-fi & cheap
const int synth_block_size = 32;      // samples per control-rate update (~1.45 ms)

enum {
  // voice
  SYN_WAVE = 0,
  SYN_ATTACK,
  SYN_DECAY,
  SYN_CUTOFF,
  SYN_RESO,
  SYN_ENVAMT,
  SYN_GRIT,
  // delay
  SYN_DLY_TIME,
  SYN_DLY_FBK,
  SYN_DLY_MIX,
  SYN_DLY_TONE,
  SYN_DLY_GRIT,
  // model controls (A, B) for each model that has them
  SYN_FM2_RATIO,
  SYN_FM2_INDEX,
  SYN_FM3_RATIO,
  SYN_FM3_INDEX,
  SYN_FBFM_FEEDBACK,
  SYN_FBFM_INDEX,
  SYN_PD_SHAPE,
  SYN_PD_AMOUNT,
  SYN_COUNT
};

enum {
  WAVE_SAW = 0, WAVE_SQUARE, WAVE_PULSE, WAVE_SAWSUB,
  WAVE_FM2, WAVE_FM3, WAVE_FBFM, WAVE_PD,
  WAVE_COUNT
};

const char* const synth_wave_labels[WAVE_COUNT] = {
  "saw", "square", "pulse", "saw+sub", "FM 2-op", "FM 3-op", "FB FM", "PD"
};

// which params are a model's two controls (-1 = none), and their short labels
const int synth_model_param_a[WAVE_COUNT] = { -1, -1, -1, -1, SYN_FM2_RATIO, SYN_FM3_RATIO, SYN_FBFM_FEEDBACK, SYN_PD_SHAPE };
const int synth_model_param_b[WAVE_COUNT] = { -1, -1, -1, -1, SYN_FM2_INDEX, SYN_FM3_INDEX, SYN_FBFM_INDEX, SYN_PD_AMOUNT };
const char* const synth_model_label_a[WAVE_COUNT] = { "", "", "", "", "Rat", "Rat", "Fbk", "Shp" };
const char* const synth_model_label_b[WAVE_COUNT] = { "", "", "", "", "Idx", "Idx", "Idx", "Amt" };

// FM ratios always in tune; a ratio param (0-127) snaps to one of these
const int fm_ratio_count = 9;
const uint8_t fm_ratios_x2[fm_ratio_count] = { 1, 2, 3, 4, 6, 7, 8, 10, 14 };  // ratio * 2
const char* const fm_ratio_labels[fm_ratio_count] = { "0.5", "1", "1.5", "2", "3", "3.5", "4", "5", "7" };
inline int fm_ratio_index(int p) { return (p * (fm_ratio_count - 1) + 63) / 127; }
const int fm_ratio_step = 16;  // param change per encoder detent, so each detent = next ratio

// tempo-synced delay times, in quarter notes
const int dly_time_count = 6;
const float dly_time_quarters[dly_time_count] = { 0.25f, 1.0f / 3, 0.5f, 0.75f, 2.0f / 3, 1.0f };
const char* const dly_time_labels[dly_time_count] = { "1/16", "1/8T", "1/8", "1/8.", "1/4T", "1/4" };

const int synth_param_max[SYN_COUNT] = {
  WAVE_COUNT - 1, 127, 127, 127, 127, 127, 127,   // voice
  dly_time_count - 1, 127, 127, 127, 127,         // delay
  127, 127, 127, 127, 127, 127, 127, 127,         // models
};

// the global patch, edited by UI (core1), read by audio (core0)
volatile int synth_params[SYN_COUNT] = {
  WAVE_SAW,  // wave
  2,         // attack
  45,        // decay
  45,        // cutoff
  70,        // resonance
  70,        // env amount
  25,        // grit
  3,         // delay time: 1/8 dotted
  55,        // delay feedback
  40,        // delay mix
  70,        // delay tone
  20,        // delay grit
  16, 50,    // FM2: ratio 1, index
  48, 45,    // FM3: ratio 2, index
  60, 40,    // FBFM: feedback, index
  30, 80,    // PD: shape, amount
};
volatile bool synth_patch_dirty = false;
volatile float synth_bpm = 120;  // set by the sequencer side, used for delay time

// change a param by a delta, clamped to its range
void synth_param_adjust(int p, int delta) {
  if (p < 0 || p >= SYN_COUNT) { return; }
  int v = synth_params[p] + delta;
  if (v < 0) { v = 0; }
  if (v > synth_param_max[p]) { v = synth_param_max[p]; }
  synth_params[p] = v;
  synth_patch_dirty = true;
}

// --- note events (posted from any core) ---

volatile uint8_t synth_on_note = 0;
volatile int8_t synth_on_wave = -1;  // per-step wave, -1 = follow global wave
volatile uint8_t synth_off_note = 0;
volatile uint32_t synth_on_count = 0;
volatile uint32_t synth_off_count = 0;

void synth_note_on(int note, int wave = -1) {
  if (note < 0) { note = 0; }
  if (note > 127) { note = 127; }
  if (wave >= WAVE_COUNT) { wave = -1; }
  synth_on_note = note;
  synth_on_wave = wave;
  __sync_synchronize();
  synth_on_count = synth_on_count + 1;
}

void synth_note_off(int note) {
  if (note < 0) { note = 0; }
  if (note > 127) { note = 127; }
  synth_off_note = note;
  __sync_synchronize();
  synth_off_count = synth_off_count + 1;
}

// --- voice state (owned by the audio core) ---

enum { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_RELEASE };

struct SynthVoice {
  uint32_t phase;
  uint32_t sub_phase;
  uint32_t mod1_phase, mod2_phase;  // FM modulators
  int32_t fb1, fb2;                 // FB FM feedback history
  uint32_t inc;
  uint8_t note;
  int8_t wave;                      // -1 = follow global
  int env_stage;
  float env;
  int32_t amp_last;  // Q15 amplitude at end of previous block
  int32_t low, band; // filter state
  uint32_t on_seen, off_seen;
};

SynthVoice synth_voice;
uint32_t synth_note_incs[128];
int16_t synth_sine[1024];  // full-scale (Q15) sine

// --- stereo ping-pong delay ---

const int dly_max_samples = synth_sample_rate * 3 / 2;  // 1.5 s per side
int16_t dly_buf_l[dly_max_samples];
int16_t dly_buf_r[dly_max_samples];
struct {
  int wr;          // write index
  float time_cur;  // current delay in samples (slews toward target)
  int32_t lp_l, lp_r;  // tone lowpass state
} dly;

void synth_init() {
  for (int n = 0; n < 128; n++) {
    double f = 440.0 * pow(2.0, (n - 69) / 12.0);
    synth_note_incs[n] = (uint32_t)(f / synth_sample_rate * 4294967296.0);
  }
  for (int i = 0; i < 1024; i++) {
    synth_sine[i] = (int16_t)(32767 * sin(6.283185307179586 * i / 1024.0));
  }
  synth_voice = SynthVoice();
  synth_voice.inc = synth_note_incs[48];
  synth_voice.wave = -1;
  synth_voice.on_seen = synth_on_count;
  synth_voice.off_seen = synth_off_count;
  for (int i = 0; i < dly_max_samples; i++) { dly_buf_l[i] = 0; dly_buf_r[i] = 0; }
  dly.wr = 0;
  dly.time_cur = 0;
  dly.lp_l = dly.lp_r = 0;
}

static inline int32_t synth_clamp(int32_t x, int32_t lim) {
  return (x > lim) ? lim : (x < -lim) ? -lim : x;
}

// sine lookup from a 32-bit phase
static inline int32_t sine32(uint32_t ph) { return synth_sine[ph >> 22]; }

// render one block of stereo 16-bit samples
void synth_render_block(int16_t* out_l, int16_t* out_r) {
  SynthVoice& v = synth_voice;

  // -- handle note events
  uint32_t onc = synth_on_count;
  if (onc != v.on_seen) {
    v.on_seen = onc;
    __sync_synchronize();
    v.note = synth_on_note;
    v.wave = synth_on_wave;
    v.inc = synth_note_incs[v.note];
    v.env_stage = ENV_ATTACK;  // retrigger from current level (no click)
  }
  uint32_t offc = synth_off_count;
  if (offc != v.off_seen) {
    v.off_seen = offc;
    __sync_synchronize();
    if (synth_off_note == v.note && v.env_stage != ENV_IDLE) {
      v.env_stage = ENV_RELEASE;
    }
  }

  // -- snapshot params
  int wave = (v.wave >= 0) ? v.wave : synth_params[SYN_WAVE];
  float p_atk = synth_params[SYN_ATTACK] / 127.0f;
  float p_dcy = synth_params[SYN_DECAY] / 127.0f;
  float p_cut = synth_params[SYN_CUTOFF] / 127.0f;
  float p_res = synth_params[SYN_RESO] / 127.0f;
  float p_env = synth_params[SYN_ENVAMT] / 127.0f;
  float p_grit = synth_params[SYN_GRIT] / 127.0f;
  int pa = synth_model_param_a[wave];
  int pb = synth_model_param_b[wave];
  float p_a = (pa >= 0) ? synth_params[pa] / 127.0f : 0;
  float p_b = (pb >= 0) ? synth_params[pb] / 127.0f : 0;

  // -- envelope, advanced once per block
  const float block_secs = (float)synth_block_size / synth_sample_rate;
  if (v.env_stage == ENV_ATTACK) {
    float attack_secs = 0.001f + p_atk * p_atk * 2.0f;  // 1ms - 2s
    v.env += block_secs / attack_secs;
    if (v.env >= 1.0f) { v.env = 1.0f; v.env_stage = ENV_DECAY; }
  } else if (v.env_stage == ENV_DECAY) {
    float decay_secs = 0.02f + p_dcy * p_dcy * 4.0f;    // 20ms - 4s
    v.env *= expf(-block_secs * 4.0f / decay_secs);
  } else if (v.env_stage == ENV_RELEASE) {
    v.env *= expf(-block_secs / 0.006f);                // fast release, avoids clicks
  }
  if (v.env_stage >= ENV_DECAY && v.env < 0.0005f) {
    v.env = 0;
    v.env_stage = ENV_IDLE;
  }

  // -- amplitude: ramp linearly across the block to avoid zipper noise
  int32_t amp_start = v.amp_last;
  int32_t amp_end = (int32_t)(v.env * 26000);  // leave headroom for resonance

  // -- filter coefficients (Chamberlin state-variable, Q14)
  float octaves = p_cut * 7.0f + p_env * 6.0f * v.env;
  float fc = 30.0f * powf(2.0f, octaves);
  float f = 2.0f * sinf(3.14159265f * fc / synth_sample_rate);
  if (f > 1.0f) { f = 1.0f; }  // keep the SVF stable (~sr/6)
  float q = 1.9f - p_res * 1.84f;  // 1/Q: 1.9 (mellow) .. 0.06 (screamy)
  int32_t f14 = (int32_t)(f * 16384);
  int32_t q14 = (int32_t)(q * 16384);

  // -- grit: pre-filter drive + post bitcrush
  int32_t drive16 = 16 + (int32_t)(p_grit * 112);   // 1x .. 8x  (Q4)
  int crush_bits = (int)(p_grit * 10);              // drop 0..10 bits
  int32_t crush_mask = ~((1 << crush_bits) - 1);

  // -- model coefficients
  // "pluck": with Env amount up, the FM index / PD amount follows the envelope
  float envmix = 1.0f - 0.8f * p_env * (1.0f - v.env);
  uint32_t inc = v.inc;
  uint32_t mod1_inc = inc, mod2_inc = inc;
  int32_t idx1_q12 = 0, idx2_q12 = 0, fb_q12 = 0;    // phase-mod depths, Q12 of a full cycle
  int32_t pd_knee = 32768, pd_s1_q8 = 256, pd_s2_q8 = 256, pd_reso_q8 = 256, pd_mix_q8 = 0;
  const float rad_to_q12 = 4096.0f / 6.2831853f;
  if (wave == WAVE_FM2 || wave == WAVE_FM3) {
    uint32_t r2 = fm_ratios_x2[fm_ratio_index(synth_params[pa])];
    mod1_inc = (uint32_t)(((uint64_t)inc * r2) >> 1);   // wraps at very high notes = more grit
    mod2_inc = mod1_inc * 2;                             // FM3: 2nd modulator an octave above the 1st
    float index = p_b * p_b * 10.0f * envmix;            // squared: finer control in the dark range
    idx1_q12 = (int32_t)(index * rad_to_q12);
    idx2_q12 = (int32_t)(index * 0.6f * rad_to_q12);
  } else if (wave == WAVE_FBFM) {
    fb_q12 = (int32_t)(p_a * 3.2f * rad_to_q12);
    idx1_q12 = (int32_t)((0.5f + p_b * p_b * 5.0f * envmix) * rad_to_q12);
  } else if (wave == WAVE_PD) {
    float d = 0.5f - 0.49f * p_b * envmix;               // knee: 0.5 = sine .. 0.01 = sharp
    pd_knee = (int32_t)(d * 65536);
    pd_s1_q8 = (int32_t)(0.5f / d * 256);
    pd_s2_q8 = (int32_t)(0.5f / (1.0f - d) * 256);
    pd_reso_q8 = (int32_t)((1.0f + p_a * 7.0f * envmix) * 256);
    pd_mix_q8 = (int32_t)(p_a * 256);
  }

  // -- delay coefficients
  float dly_target = dly_time_quarters[synth_params[SYN_DLY_TIME]] * (60.0f / synth_bpm) * synth_sample_rate;
  if (dly_target > dly_max_samples - 1) { dly_target = dly_max_samples - 1; }
  if (dly_target < 1) { dly_target = 1; }
  float dslew = dly_target - dly.time_cur;  // slew = short tape-like warble on tempo/time change
  const float max_slew = synth_block_size * 0.25f;
  if (dslew > max_slew) { dslew = max_slew; }
  if (dslew < -max_slew) { dslew = -max_slew; }
  if (dly.time_cur == 0) { dslew = dly_target; }  // first block: jump
  dly.time_cur += dslew;
  int dly_samples = (int)dly.time_cur;
  int32_t dly_fbk_q15 = (int32_t)(synth_params[SYN_DLY_FBK] / 127.0f * 0.95f * 32767);
  int32_t dly_mix_q15 = (int32_t)(synth_params[SYN_DLY_MIX] / 127.0f * 32767);
  float tone = synth_params[SYN_DLY_TONE] / 127.0f;
  float tone_fc = 300.0f * powf(2.0f, tone * 5.2f);   // 300 Hz .. ~11 kHz
  int32_t dly_tone_q14 = (int32_t)((1.0f - expf(-6.2831853f * tone_fc / synth_sample_rate)) * 16384);
  float dg = synth_params[SYN_DLY_GRIT] / 127.0f;
  int32_t dly_drive16 = 16 + (int32_t)(dg * 32);      // 1x .. 3x per pass, compounding
  int dly_crush = (int)(dg * 7);
  int32_t dly_crush_mask = ~((1 << dly_crush) - 1);

  uint32_t phase = v.phase;
  uint32_t sub_phase = v.sub_phase;
  uint32_t m1 = v.mod1_phase, m2 = v.mod2_phase;
  int32_t fb1 = v.fb1, fb2 = v.fb2;
  int32_t low = v.low, band = v.band;
  int wr = dly.wr;
  int32_t lp_l = dly.lp_l, lp_r = dly.lp_r;

  for (int i = 0; i < synth_block_size; i++) {
    // oscillator / model, about +/-32767 for classic waves, quieter for FM/PD
    int32_t s;
    switch (wave) {
      default:
      case WAVE_SAW: s = (int32_t)(phase >> 16) - 32768; break;
      case WAVE_SQUARE: s = (phase & 0x80000000u) ? 32767 : -32767; break;
      case WAVE_PULSE: s = (phase < 0x40000000u) ? 32767 : -32767; break;
      case WAVE_SAWSUB:
        s = (((int32_t)(phase >> 16) - 32768) >> 1) + ((sub_phase & 0x80000000u) ? 16383 : -16383);
        break;
      case WAVE_FM2: {
        int32_t mod = sine32(m1);
        s = sine32(phase + ((uint32_t)(mod * idx1_q12) << 5));
        s = (s * 5) >> 3;
        break; }
      case WAVE_FM3: {
        int32_t mod2 = sine32(m2);
        int32_t mod1 = sine32(m1 + ((uint32_t)(mod2 * idx2_q12) << 5));
        s = sine32(phase + ((uint32_t)(mod1 * idx1_q12) << 5));
        s = (s * 5) >> 3;
        break; }
      case WAVE_FBFM: {
        int32_t mod = sine32(m1 + ((uint32_t)(((fb1 + fb2) >> 1) * fb_q12) << 5));
        fb2 = fb1;
        fb1 = mod;
        s = sine32(phase + ((uint32_t)(mod * idx1_q12) << 5));
        s = (s * 5) >> 3;
        break; }
      case WAVE_PD: {
        int32_t p = phase >> 16;  // 0..65535
        // casio-style bent phase -> -cos
        int32_t pw = (p < pd_knee) ? ((p * pd_s1_q8) >> 8) : 32768 + (((p - pd_knee) * pd_s2_q8) >> 8);
        int32_t bent = -synth_sine[((pw >> 6) + 256) & 1023];
        // windowed resonant sine
        int32_t rp = ((p * pd_reso_q8) >> 8) & 0xffff;
        int32_t reso = (((65535 - p) >> 1) * synth_sine[rp >> 6]) >> 15;
        s = (((256 - pd_mix_q8) * bent + ((pd_mix_q8 * reso * 3) >> 1)) >> 8);
        s = (s * 17) >> 5;
        break; }
    }
    phase += inc;
    sub_phase += inc >> 1;
    m1 += mod1_inc;
    m2 += mod2_inc;

    // drive into hard clip
    s = synth_clamp((s * drive16) >> 4, 32767);

    // state-variable lowpass, run at half scale so Q14 math fits in 32 bits
    s >>= 1;
    low = synth_clamp(low + ((f14 * band) >> 14), 32767);
    int32_t high = synth_clamp(s - low - ((q14 * band) >> 14), 32767);
    band = synth_clamp(band + ((f14 * high) >> 14), 32767);

    // amp envelope, ramped across block
    int32_t amp = amp_start + ((amp_end - amp_start) * i) / synth_block_size;
    int32_t dry = synth_clamp(((low << 1) * amp) >> 15, 32767) & crush_mask;

    // ping-pong delay: dry -> L line, L -> R line, R -> L line
    int rd = wr - dly_samples;
    if (rd < 0) { rd += dly_max_samples; }
    int32_t tap_l = dly_buf_l[rd];
    int32_t tap_r = dly_buf_r[rd];
    int32_t in_l = synth_clamp(dry + ((tap_r * dly_fbk_q15) >> 15), 32767);
    int32_t in_r = (tap_l * dly_fbk_q15) >> 15;
    lp_l += ((in_l - lp_l) * dly_tone_q14) >> 14;   // tone: darker each repeat
    lp_r += ((in_r - lp_r) * dly_tone_q14) >> 14;
    int32_t w_l = synth_clamp((lp_l * dly_drive16) >> 4, 32767) & dly_crush_mask;  // grit: dirtier each repeat
    int32_t w_r = synth_clamp((lp_r * dly_drive16) >> 4, 32767) & dly_crush_mask;
    dly_buf_l[wr] = (int16_t)w_l;
    dly_buf_r[wr] = (int16_t)w_r;
    if (++wr >= dly_max_samples) { wr = 0; }

    out_l[i] = (int16_t)synth_clamp(dry + ((tap_l * dly_mix_q15) >> 15), 32767);
    out_r[i] = (int16_t)synth_clamp(dry + ((tap_r * dly_mix_q15) >> 15), 32767);
  }

  v.phase = phase;
  v.sub_phase = sub_phase;
  v.mod1_phase = m1;
  v.mod2_phase = m2;
  v.fb1 = fb1;
  v.fb2 = fb2;
  v.low = low;
  v.band = band;
  v.amp_last = amp_end;
  dly.wr = wr;
  dly.lp_l = lp_l;
  dly.lp_r = lp_r;
}
