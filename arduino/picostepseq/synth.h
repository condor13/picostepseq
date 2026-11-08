/**
 * synth.h -- tiny gritty mono synth voice for picostepseq
 *
 * Signal flow:  osc (saw/square/pulse/saw+sub) -> drive/clip -> resonant LPF -> amp env -> bitcrush
 *
 * - Oscillators are intentionally NOT band-limited (aliasing = grit)
 * - One Attack/Decay envelope drives both amplitude and filter cutoff
 * - Step gate ends the note early with a short release
 * - Audio runs in integer math, envelope/filter coefficients are computed
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
  SYN_WAVE = 0,
  SYN_ATTACK,
  SYN_DECAY,
  SYN_CUTOFF,
  SYN_RESO,
  SYN_ENVAMT,
  SYN_GRIT,
  SYN_COUNT
};

enum { WAVE_SAW = 0, WAVE_SQUARE, WAVE_PULSE, WAVE_SAWSUB, WAVE_COUNT };

const char* synth_param_names[SYN_COUNT] = { "wave", "atk", "dcy", "cut", "res", "env", "grit" };
const int synth_param_max[SYN_COUNT] = { WAVE_COUNT - 1, 127, 127, 127, 127, 127, 127 };
const char* synth_wave_names[WAVE_COUNT] = { "saw", "squ", "pls", "sub" };

// the global patch, edited by UI (core1), read by audio (core0)
volatile int synth_params[SYN_COUNT] = {
  WAVE_SAW,  // wave
  2,         // attack
  45,        // decay
  45,        // cutoff
  70,        // resonance
  70,        // env amount
  25,        // grit
};
volatile bool synth_patch_dirty = false;

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
volatile uint8_t synth_off_note = 0;
volatile uint32_t synth_on_count = 0;
volatile uint32_t synth_off_count = 0;

void synth_note_on(int note) {
  if (note < 0) { note = 0; }
  if (note > 127) { note = 127; }
  synth_on_note = note;
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
  uint32_t inc;
  uint8_t note;
  int env_stage;
  float env;
  int32_t amp_last;  // Q15 amplitude at end of previous block
  int32_t low, band; // filter state
  uint32_t on_seen, off_seen;
};

SynthVoice synth_voice;
uint32_t synth_note_incs[128];

void synth_init() {
  for (int n = 0; n < 128; n++) {
    double f = 440.0 * pow(2.0, (n - 69) / 12.0);
    synth_note_incs[n] = (uint32_t)(f / synth_sample_rate * 4294967296.0);
  }
  synth_voice = SynthVoice();
  synth_voice.inc = synth_note_incs[48];
  synth_voice.on_seen = synth_on_count;
  synth_voice.off_seen = synth_off_count;
}

static inline int32_t synth_clamp(int32_t x, int32_t lim) {
  return (x > lim) ? lim : (x < -lim) ? -lim : x;
}

// render one block of mono 16-bit samples
void synth_render_block(int16_t* out) {
  SynthVoice& v = synth_voice;

  // -- handle note events
  uint32_t onc = synth_on_count;
  if (onc != v.on_seen) {
    v.on_seen = onc;
    __sync_synchronize();
    v.note = synth_on_note;
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
  int wave = synth_params[SYN_WAVE];
  float p_atk = synth_params[SYN_ATTACK] / 127.0f;
  float p_dcy = synth_params[SYN_DECAY] / 127.0f;
  float p_cut = synth_params[SYN_CUTOFF] / 127.0f;
  float p_res = synth_params[SYN_RESO] / 127.0f;
  float p_env = synth_params[SYN_ENVAMT] / 127.0f;
  float p_grit = synth_params[SYN_GRIT] / 127.0f;

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

  uint32_t phase = v.phase;
  uint32_t sub_phase = v.sub_phase;
  uint32_t inc = v.inc;
  int32_t low = v.low, band = v.band;

  for (int i = 0; i < synth_block_size; i++) {
    // oscillator, about +/-32767
    int32_t s;
    switch (wave) {
      default:
      case WAVE_SAW: s = (int32_t)(phase >> 16) - 32768; break;
      case WAVE_SQUARE: s = (phase & 0x80000000u) ? 32767 : -32767; break;
      case WAVE_PULSE: s = (phase < 0x40000000u) ? 32767 : -32767; break;
      case WAVE_SAWSUB:
        s = (((int32_t)(phase >> 16) - 32768) >> 1) + ((sub_phase & 0x80000000u) ? 16383 : -16383);
        break;
    }
    phase += inc;
    sub_phase += inc >> 1;

    // drive into hard clip
    s = synth_clamp((s * drive16) >> 4, 32767);

    // state-variable lowpass, run at half scale so Q14 math fits in 32 bits
    s >>= 1;
    low = synth_clamp(low + ((f14 * band) >> 14), 32767);
    int32_t high = synth_clamp(s - low - ((q14 * band) >> 14), 32767);
    band = synth_clamp(band + ((f14 * high) >> 14), 32767);

    // amp envelope, ramped across block
    int32_t amp = amp_start + ((amp_end - amp_start) * i) / synth_block_size;
    int32_t o = synth_clamp(((low << 1) * amp) >> 15, 32767);

    out[i] = (int16_t)(o & crush_mask);
  }

  v.phase = phase;
  v.sub_phase = sub_phase;
  v.low = low;
  v.band = band;
  v.amp_last = amp_end;
}
