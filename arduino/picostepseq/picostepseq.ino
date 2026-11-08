/**
 * picostepseq.ino  -- picostepseq MIDI step sequencer in Arduino
 * 28 Apr 2023 - @todbot / Tod Kurt
 * 15 Aug 2022 - @todbot / Tod Kurt
 * Part of https://github.com/todbot/picostepseq/
 *
 * Libraries needed (all available via Library Manager):
 * - Bounce2 -- https://github.com/thomasfredericks/Bounce2
 * - RotaryEncoder -- http://www.mathertel.de/Arduino/RotaryEncoderLibrary.aspx
 * - Adafruit_SSD1306 -- https://github.com/adafruit/Adafruit_SSD1306
 * - Adafruit_TinyUSB -- https://github.com/adafruit/Adafruit_TinyUSB_Arduino
 * - MIDI -- https://github.com/FortySevenEffects/arduino_midi_library
 * - ArduinoJson -- https://arduinojson.org/
 * - I2S -- built into the arduino-pico core
 *
 * Audio out: PCM5102A I2S DAC board on GP26 (BCK), GP27 (LCK), GP28 (DIN)
 *
 * To upload:
 * - Use Arduino IDE 1.8.19
 * - Install arduino-pico Arduino core https://arduino-pico.readthedocs.io/en/latest/install.html
 * - In "Tools", set "Flash Size: 2MB (Sketch: 1MB / FS: 1MB)"
 * - In "Tools", set "Tools / USB Stack: Adafruit TinyUSB"
 * - Program the sketch to the Pico with "Upload"
 *
 **/

#include <Wire.h>
#include <RotaryEncoder.h>
#include <Bounce2.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <I2S.h>
#include <hardware/gpio.h>  // to hand GP16 between UART (MIDI) and plain GPIO (clock/trig out)

#include "fonts/helvnCB6pt7b.h"
#include "fonts/ter_u12n7b.h"  // CirPy's default displayio font, it seems

#include "sequencer.h"
#include "synth.h"
#include "images.h"
#include "wave_icons.h"
#include "activity_anim.h"
#include "saved_sequences_json.h"  // to bootstrap the "saved_sequences.json" file

#define myfont helvnCB6pt7b  // sigh
#define myfont2 ter_u12n7b

// what the MIDI OUT DIN (GP16) sends
enum { OUT_MIDI = 0, OUT_CLK4, OUT_CLK24, OUT_TRIG, OUT_COUNT };
const char* const out_mode_labels[OUT_COUNT] = { "MIDI", "CLK 4", "CLK 24", "TRIG" };

typedef struct {
  int  step_size;  // aka "seqr.ticks_per_step"
  int  midi_chan;
  int  midi_velocity;
  bool midi_send_clock;
  bool midi_forward_usb;
  bool midi_forward_uart;
  int  out_mode;   // OUT_MIDI, OUT_CLK4, OUT_CLK24, OUT_TRIG
} Config;

Config cfg  = {
  //.step_size = SIXTEENTH_NOTE,
  .step_size = EIGHTH_NOTE,
  .midi_chan = 1,
  .midi_velocity = 80,
  .midi_send_clock = true,
  .midi_forward_usb = true,
  .midi_forward_uart = true,
  .out_mode = OUT_MIDI,
};

const char* save_file = "/saved_sequences.json";
const bool midi_out_debug = true;
const bool midi_in_debug = true;

const int numseqs = 8;

// begin hardware definitions
const int dw = 128;
const int dh = 64;

const int led_pins[] = { 0, 2, 4, 6, 8, 10, 12, 14 };
const int key_pins[] = { 1, 3, 5, 7, 9, 11, 13, 15 };

const int encoderA_pin = 18;
const int encoderB_pin = 19;
const int encoderSW_pin = 22;

const int oled_sda_pin = 20;
const int oled_scl_pin = 21;

const int oled_i2c_addr = 0x3C;

const int midi_tx_pin = 16;
const int midi_rx_pin = 17;

// PCM5102A I2S DAC (LRCK/"LCK" must be BCK+1)
const int i2s_bck_pin = 26;   // + GP27 = LCK
const int i2s_data_pin = 28;
// end hardware definitions

Adafruit_USBD_MIDI usb_midi;                                  // USB MIDI object
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDIusb);  // USB MIDI
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDIuart);      // Serial MIDI

Adafruit_SSD1306 display(dw, dh, &Wire, -1);

I2S i2s(OUTPUT);

Bounce2::Button keys[numsteps];
Bounce2::Button encoder_switch;

RotaryEncoder encoder(encoderA_pin, encoderB_pin, RotaryEncoder::LatchMode::FOUR3);  // A,B order sets turn direction
void checkEncoderPosition() {  encoder.tick();  } // call tick() to check the state.
int encoder_pos_last = 0;

int led_vals[numsteps];
int led_fade = 85;

float tempo = 100;
StepSequencer seqr;
Step sequences[numseqs][numsteps];

uint8_t midiclk_cnt = 0;
uint32_t midiclk_last_micros = 0;


#include "saveload.h" /// FIXME:

//
// -- clock / trig out on the MIDI OUT DIN (GP16), core0 only
//
int out_mode_applied = OUT_MIDI;
bool out_pulse_on = false;
uint32_t out_pulse_start_micros = 0;
uint8_t out_tick_count = 0;
const uint32_t out_pulse_micros = 5000;  // ~5ms pulses

void out_pulse() {
  if (out_mode_applied == OUT_MIDI) { return; }
  digitalWrite(midi_tx_pin, HIGH);
  out_pulse_on = true;
  out_pulse_start_micros = micros();
}

// called on every 24ppqn sequencer clock tick while playing
void out_clock_tick() {
  if (out_mode_applied == OUT_CLK24 ||
      (out_mode_applied == OUT_CLK4 && (out_tick_count % (ticks_per_quarternote / 4)) == 0)) {
    out_pulse();
  }
  out_tick_count++;
}

// apply mode changes made by the UI, and end pulses
void out_update() {
  int mode = cfg.out_mode;
  if (mode != out_mode_applied) {
    if (mode == OUT_MIDI) {
      gpio_set_function(midi_tx_pin, GPIO_FUNC_UART);  // give GP16 back to the MIDI UART
    } else if (out_mode_applied == OUT_MIDI) {
      pinMode(midi_tx_pin, OUTPUT);  // detach from UART, plain GPIO
      digitalWrite(midi_tx_pin, LOW);
    }
    out_mode_applied = mode;
    out_pulse_on = false;
  }
  if (out_pulse_on) {
    uint32_t len = min(out_pulse_micros, seqr.tick_micros / 2);  // stays short at fast 24ppqn
    if (micros() - out_pulse_start_micros >= len) {
      digitalWrite(midi_tx_pin, LOW);
      out_pulse_on = false;
    }
  }
}

//
// -- MIDI sending & receiving functions
//

// note on: MIDI out + internal synth (also used for step previews)
void send_note_on(uint8_t note, uint8_t vel, uint8_t gate, bool on, int8_t wave) {
  if (on) {
    MIDIusb.sendNoteOn(note, cfg.midi_velocity, cfg.midi_chan);
    MIDIuart.sendNoteOn(note, cfg.midi_velocity, cfg.midi_chan);
    synth_note_on(note, wave);
  }
  if (midi_out_debug) { Serial.printf("noteOn:  %d %d %d %d\n", note, vel, gate, on); }
}

// callback used by Sequencer to trigger note on (core0): plays the note, and TRIG out on active steps
void seq_note_on(uint8_t note, uint8_t vel, uint8_t gate, bool on, int8_t wave) {
  if (on && out_mode_applied == OUT_TRIG) { out_pulse(); }
  send_note_on(note, vel, gate, on, wave);
}

// callback used by Sequencer to trigger note off
void send_note_off(uint8_t note, uint8_t vel, uint8_t gate, bool on, int8_t wave) {
  if (on) {
    MIDIusb.sendNoteOff(note, cfg.midi_velocity, cfg.midi_chan);
    MIDIuart.sendNoteOff(note, cfg.midi_velocity, cfg.midi_chan);
    synth_note_off(note);
  }
  if (midi_out_debug) { Serial.printf("noteOff: %d %d %d %d\n", note, vel, gate, on); }
}

// callback used by Sequencer to send midi clock when internally triggered
// (sequencer always calls this; MIDI clock is only sent if enabled, clock out follows its own mode)
void send_clock_start_stop(clock_type_t type) {
  if (type == START) {
    out_tick_count = 0;  // CLK 4 pulses line up with the first step
    if (!cfg.midi_send_clock) { return; }
    MIDIusb.sendStart();
    MIDIuart.sendStart();
  } else if (type == STOP) {
    if (!cfg.midi_send_clock) { return; }
    MIDIusb.sendStop();
    MIDIuart.sendStop();
  } else if (type == CLOCK) {
    out_clock_tick();
    if (!cfg.midi_send_clock) { return; }
    MIDIusb.sendClock();
    MIDIuart.sendClock();
  }
  if (midi_out_debug) { Serial.printf("\tclk:%d\n", type); }
}

// void handle_midi_in_songpos(unsigned int beats) {
//     Serial.printf("songpos:%d\n", beats);
//     if( beats == 0 ) {
//         seqr.stepi = 0;
//     }
// }

void handle_midi_in_start() {
  seqr.play();
  midiclk_cnt = 0;
  if (midi_in_debug) { Serial.println("midi in start"); }
}

void handle_midi_in_stop() {
  seqr.stop();
  if (midi_in_debug) { Serial.println("midi in stop"); }
}

// FIXME: midi continue?
void handle_midi_in_clock() {
  uint32_t now_micros = micros();
  //const int fscale = 230; // out of 255, 230/256 = .9 filter
  // once every ticks_per_step, play note (24 ticks per quarter note => 6 ticks per 16th note)
  out_clock_tick();  // clock out follows external MIDI clock too
  if (midiclk_cnt % seqr.ticks_per_step == 0) {  // ticks_per_step = 6 for 16th note
    seqr.trigger_ext(now_micros);  // FIXME: figure out 2nd arg (was step_millis)
  }
  midiclk_cnt++;
  // once every quarter note, calculate new BPM, but be a little cautious about it
  if( midiclk_cnt == ticks_per_quarternote ) {
      uint32_t new_tick_micros = (now_micros - midiclk_last_micros) / ticks_per_quarternote;
      if( new_tick_micros > seqr.tick_micros/2 && new_tick_micros < seqr.tick_micros*2 ) {
        seqr.tick_micros = new_tick_micros;
      }
      midiclk_last_micros = now_micros;
      midiclk_cnt = 0;
  }
}

//
void midi_read_and_forward() {
  if (MIDIusb.read()) {
    midi::MidiType t = MIDIusb.getType();
    switch (t) {
      case midi::Start:
        handle_midi_in_start();  break;
      case midi::Stop:
        handle_midi_in_stop();  break;
      case midi::Clock:
        handle_midi_in_clock();  break;
      default: break;
    }
    // forward the midi msg to other port
    MIDIuart.send(t, MIDIusb.getData1(), MIDIusb.getData2(), MIDIusb.getChannel());
  }

  if (MIDIuart.read()) {
    midi::MidiType t = MIDIuart.getType();
    switch (t) {
      case midi::Start:
        handle_midi_in_start();  break;
      case midi::Stop:
        handle_midi_in_stop();  break;
      case midi::Clock:
        handle_midi_in_clock();  break;
      default: break;
    }
    MIDIusb.send(t, MIDIuart.getData1(), MIDIuart.getData2(), MIDIuart.getChannel());
  }
}

////////////////////////////

//
// ---  MIDI in/out setup on core0
//
void setup() {
  TinyUSBDevice.setManufacturerDescriptor("todbot");
  TinyUSBDevice.setProductDescriptor("PicoStepSeq");

  Serial1.setRX(midi_rx_pin);
  Serial1.setTX(midi_tx_pin);

  MIDIusb.begin();
  MIDIusb.turnThruOff();   // turn off echo
  MIDIuart.begin(MIDI_CHANNEL_OMNI); // don't forget OMNI
  MIDIuart.turnThruOff();  // turn off echo

  Serial.begin(115200);

  // AUDIO
  synth_init();
  i2s.setBCLK(i2s_bck_pin);   // LRCK is automatically BCLK+1
  i2s.setDATA(i2s_data_pin);
  i2s.setBitsPerSample(32);   // 64*fs bit clock, keeps the PCM5102 PLL happy at low rates
  i2s.setBuffers(8, 128);    // 8 x 64 stereo frames = ~23ms of buffering
  if (!i2s.begin(synth_sample_rate)) {
    Serial.println("I2S init failed");
  }
}

// keep the I2S DMA buffers topped up with (stereo) synth audio
void synth_update() {
  int16_t buf_l[synth_block_size];
  int16_t buf_r[synth_block_size];
  synth_bpm = seqr.tempo();  // delay time follows tempo
  while (i2s.availableForWrite() >= synth_block_size * 8) {  // 8 bytes per stereo 32-bit frame
    synth_render_block(buf_l, buf_r);
    for (int i = 0; i < synth_block_size; i++) {
      i2s.write32((int32_t)buf_l[i] << 16, (int32_t)buf_r[i] << 16);
    }
  }
}

//
// --- MIDI in/out & audio on core0
//
void loop() {
  midi_read_and_forward();
  seqr.update();  // will call send_note_{on,off} callbacks
  out_update();
  synth_update();
}

void configure_sequencer() {
  Serial.println("configuring sequencer");
  seqr.set_tempo(tempo);
  seqr.ticks_per_step = cfg.step_size;
  seqr.on_func = seq_note_on;
  seqr.off_func = send_note_off;
  seqr.clk_func = send_clock_start_stop;
  seqr.send_clock = true;  // always get clock ticks; MIDI clock sending is gated in send_clock_start_stop()
};

//
// --- UI setup on core1 (buttons, knobs, display, LEDs)
//
void setup1() {
  // delay(5000);  // for debugging

  LittleFS.begin();

  sequences_read();
  sequence_load(seqr.seqno);  // 0
  synth_patch_read();

  configure_sequencer();

  // KEYS
  for (uint8_t i = 0; i < numsteps; i++) {
    keys[i].attach(key_pins[i], INPUT_PULLUP);
    keys[i].setPressedState(LOW);
  }

  // LEDS
  for (uint8_t i = 0; i < numsteps; i++) {
    pinMode(led_pins[i], OUTPUT);
    analogWrite(led_pins[i], i * 10);  // just to test
  }

  // ENCODER
  pinMode(encoderA_pin, INPUT_PULLUP);
  pinMode(encoderB_pin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(encoderA_pin), checkEncoderPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(encoderB_pin), checkEncoderPosition, CHANGE);
  encoder_switch.attach(encoderSW_pin, INPUT_PULLUP);
  encoder_switch.setPressedState(LOW);

  // DISPLAY
  Wire.setSDA(oled_sda_pin);
  Wire.setSCL(oled_scl_pin);
  Wire.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, oled_i2c_addr)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;) ;  // Don't proceed, loop forever
  }

  displaySplash();
}

/**
 * UI input "verbs":
 *
 * encoder tap                -- play/stop (on a page: select next item / load)
 * encoder hold 1s            -- on LOAD/SAVE page: save to selected slot
 * encoder turn               -- transpose (on a page: change selected value / slot / step wave)
 * encoder pushhold + turn    -- bpm
 *
 * key tap                    -- mute/unmute (playing) or play step note (paused)
 *                               (on STEP page: select that step)
 * key hold + encoder turn    -- change step's note (on STEP page: that step's wave)
 * key hold + encoder pushhold + encoder turn -- change step's gate
 *
 * key1 is also SHIFT, once a second key is pressed while it's held:
 * SHIFT + key2  -- page 1: SYNTH
 * SHIFT + key3  -- page 2: WAVE (model + its two controls)
 * SHIFT + key4  -- page 3: STEP (wave per step)
 * SHIFT + key5  -- page 4: DELAY
 * SHIFT + key6  -- page 5: MIDI (incl. clock/trig out mode)
 * SHIFT + key7  -- page 6: LOAD/SAVE
 * SHIFT + same key again, or SHIFT + key8 -- leave page
 * (key1 alone still behaves as a normal step key)
 *
 * The sequencer keeps running while a page is shown.
 */

// variables for UI state management
int encoder_delta = 0;
uint32_t encoder_push_millis;
bool encoder_turned_while_held = false;
bool encoder_hold_done = false;  // long-hold action (save) already fired for this push
uint32_t step_push_millis;
int step_push = -1;
bool step_edited = false;
bool shift_used = false;          // key1 is being used as SHIFT for a combo
bool key_consumed[numsteps];      // key press was used by a page: no preview / mute on release

enum {
  CFG_STEPSIZE = 0,
  CFG_MIDICHAN,
  CFG_MIDIVEL,
  CFG_MIDISENDCLK,
  CFG_OUTMODE,
  CFG_COUNT
};

// page numbers match the SHIFT key index (key2 = 1, ...)
enum {
  PAGE_NONE = 0,
  PAGE_SYNTH,
  PAGE_WAVE,
  PAGE_STEP,
  PAGE_DELAY,
  PAGE_MIDI,
  PAGE_SLOTS,
  PAGE_COUNT
};

int page = PAGE_NONE;
int page_sel = 0;           // selected item on current page
char page_msg[22] = "";     // transient message on LOAD/SAVE page
uint32_t page_msg_millis = 0;

// SYNTH page selection order (left column, then right column)
const int synth_page_order[] = { SYN_WAVE, SYN_GRIT, SYN_CUTOFF, SYN_RESO, SYN_ATTACK, SYN_DECAY, SYN_ENVAMT };
const int synth_page_count = sizeof(synth_page_order) / sizeof(synth_page_order[0]);
// DELAY page selection order
const int delay_page_order[] = { SYN_DLY_TIME, SYN_DLY_FBK, SYN_DLY_MIX, SYN_DLY_TONE, SYN_DLY_GRIT };
const int delay_page_count = sizeof(delay_page_order) / sizeof(delay_page_order[0]);

bool wave_has_controls(int wave) { return synth_model_param_a[wave] >= 0; }
bool is_ratio_param(int p) { return p == SYN_FM2_RATIO || p == SYN_FM3_RATIO; }

int page_item_count() {
  switch (page) {
    case PAGE_SYNTH: return synth_page_count;
    case PAGE_WAVE: return wave_has_controls(synth_params[SYN_WAVE]) ? 3 : 1;
    case PAGE_STEP: return numsteps;
    case PAGE_DELAY: return delay_page_count;
    case PAGE_MIDI: return CFG_COUNT;
    case PAGE_SLOTS: return numseqs;
    default: return 1;
  }
}

// SHIFT + key pressed: open/close pages
void page_shift_key(int key) {
  if (key == 7) { page = PAGE_NONE; return; }  // SHIFT+8 = exit
  if (key < PAGE_SYNTH || key >= PAGE_COUNT) { return; }
  if (page == key) { page = PAGE_NONE; return; }  // same combo again = exit
  page = key;
  page_sel = (page == PAGE_SLOTS) ? seqr.seqno : 0;
  page_msg[0] = 0;
}

// encoder turned while a page is shown
void page_turn(int delta) {
  if (page == PAGE_SYNTH) {
    int p = synth_page_order[page_sel];
    synth_param_adjust(p, (p == SYN_WAVE) ? delta : delta * 2);
  }
  else if (page == PAGE_WAVE) {
    int wave = synth_params[SYN_WAVE];
    if (page_sel == 0) {
      synth_param_adjust(SYN_WAVE, delta);
      if (page_sel >= page_item_count()) { page_sel = 0; }
    } else {
      int p = (page_sel == 1) ? synth_model_param_a[wave] : synth_model_param_b[wave];
      synth_param_adjust(p, is_ratio_param(p) ? delta * fm_ratio_step : delta * 2);
    }
  }
  else if (page == PAGE_STEP) {
    int w = seqr.steps[page_sel].wave + delta;
    seqr.steps[page_sel].wave = constrain(w, -1, WAVE_COUNT - 1);
  }
  else if (page == PAGE_DELAY) {
    int p = delay_page_order[page_sel];
    synth_param_adjust(p, (p == SYN_DLY_TIME) ? delta : delta * 2);
  }
  else if (page == PAGE_MIDI) {
    if (page_sel == CFG_STEPSIZE) {
      // 1/16 -> 1/8 -> 1/4
      int idx = (cfg.step_size == SIXTEENTH_NOTE) ? 0 : (cfg.step_size == EIGHTH_NOTE) ? 1 : 2;
      idx = constrain(idx + delta, 0, 2);
      const int sizes[3] = { SIXTEENTH_NOTE, EIGHTH_NOTE, QUARTER_NOTE };
      cfg.step_size = sizes[idx];
      configure_sequencer();
    }
    else if (page_sel == CFG_MIDICHAN) {
      cfg.midi_chan = constrain(cfg.midi_chan + delta, 1, 16);
    }
    else if (page_sel == CFG_MIDIVEL) {
      cfg.midi_velocity = constrain(cfg.midi_velocity + delta * 2, 1, 127);
    }
    else if (page_sel == CFG_MIDISENDCLK) {
      cfg.midi_send_clock = !cfg.midi_send_clock;
    }
    else if (page_sel == CFG_OUTMODE) {
      cfg.out_mode = constrain(cfg.out_mode + delta, 0, OUT_COUNT - 1);
    }
  }
  else if (page == PAGE_SLOTS) {  // pick slot, wrapping around
    page_sel = (page_sel + delta % numseqs + numseqs) % numseqs;
  }
}

// encoder tapped while a page is shown
void page_tap() {
  if (page == PAGE_SLOTS) {
    sequence_load(page_sel);
    snprintf(page_msg, sizeof(page_msg), "  loaded seq %d", page_sel + 1);
    page_msg_millis = millis();
  } else {
    page_sel = (page_sel + 1) % page_item_count();
  }
}

// encoder held 1s while a page is shown
void page_hold() {
  if (page == PAGE_SLOTS) {
    sequence_save(page_sel);  // to RAM; written to flash now if paused, else on next pause
    seqr.seqno = page_sel;
    snprintf(page_msg, sizeof(page_msg), "  saved to seq %d", page_sel + 1);
    page_msg_millis = millis();
    if (!seqr.playing) {
      sequences_write();
    }
  }
}

//
// --- UI handling on core1
//
void loop1() {
  // LEDS update
  for (int i = 0; i < numsteps; i++) {
    Step s = seqr.steps[i];
    int v = 0;                         // UI: off = muted
    int si = (seqr.stepi-0) % numsteps;
    if (i == si) { v = 255; }  // UI: bright red = indicates sequence postion
    else if (s.on) {
      v = 40;
    }                                    // UI: dim red = indicates mute/unmute state
    v = max(led_vals[i] - led_fade, v);  // nice 80s fade
    led_vals[i] = v;
    analogWrite(led_pins[i], v);
  }

  // ENCODER update
  encoder_switch.update();
  encoder.tick();
  int encoder_pos = encoder.getPosition();
  if (encoder_pos != encoder_pos_last) {
    encoder_delta = encoder_pos - encoder_pos_last;
    encoder_pos_last = encoder_pos;
  }

  uint32_t now = millis();

  // on encoder turn
  if (encoder_delta) {
    if (encoder_push_millis > 0) { encoder_turned_while_held = true; }
    // UI: SHIFT held: ignore turns
    if (shift_used && keys[0].isPressed()) {
    }
    // UI: STEP page, step key held + turn = change that step's wave
    else if (page == PAGE_STEP && step_push > -1 && encoder_push_millis == 0) {
      page_sel = step_push;
      page_turn(encoder_delta);
      step_edited = true;
    }
    // UI: encoder turn + push while step key held = change step's gate
    else if (step_push > -1 && encoder_push_millis > 0) {
      int gate = seqr.steps[step_push].gate;
      gate = constrain(gate + encoder_delta, 1, 15);
      seqr.steps[step_push].gate = gate;
      step_edited = true;
    }
    // UI: encoder turn while step key held = change step's note
    else if (step_push > -1) {
      Step s = seqr.steps[step_push];
      if (!seqr.playing) {  // step preview note off
        send_note_off(s.note, s.vel, s.gate, true, s.wave);
      }
      s.note = constrain(s.note + encoder_delta, 1, 127);
      if (!seqr.playing) {  // step preview note on
        send_note_on(s.note, s.vel, s.gate, true, s.wave);
      }
      seqr.steps[step_push] = s;
      step_edited = true;
    }
    // UI: encoder turn while encoder pushed = change tempo
    else if (encoder_push_millis > 0) {
      tempo = tempo + encoder_delta;
      seqr.set_tempo(tempo);
      Serial.printf("TEMPO: %d %f\n", (int)seqr.tempo(), seqr.tempo());
    }
    // UI: encoder turn on a page = change selected value
    else if (page != PAGE_NONE) {
      page_turn(encoder_delta);
    }
    // UI: encoder turn = change transpose
    else {
      seqr.transpose = constrain(seqr.transpose + encoder_delta, -36, 36);
    }
    encoder_delta = 0;  // we own turning, and we've used it
  }

  // on encoder push
  if (encoder_switch.pressed()) {  // push encoder button
    encoder_push_millis = now;     // save when (and that) it was pushed
    encoder_turned_while_held = false;
    encoder_hold_done = false;
  }

  // UI: encoder held 1s (no turn, no key) on a page = page hold action (save)
  if (encoder_push_millis > 0 && !encoder_hold_done && !encoder_turned_while_held &&
      step_push == -1 && page != PAGE_NONE && (now - encoder_push_millis) > 1000) {
    encoder_hold_done = true;
    page_hold();
  }

  if (encoder_switch.released()) {  // release encoder button
    // UI: encoder tap with no key held and no turn
    if (step_push == -1 && !encoder_turned_while_held && (now - encoder_push_millis) < 300) {
      if (page != PAGE_NONE) {
        page_tap();  // select next / load
      } else {
        seqr.toggle_play_stop();  // play/pause
        delay(10);  // wait a bit for sequencer to change state
        if (!seqr.playing) {
          sequences_write();  // write to disk on pause
          synth_patch_write();  // only writes if changed
        }
      }
    }
    encoder_push_millis = 0;  // we own it, and we're done with it
  }

  // KEYS update

  // press/release event check
  for (uint8_t i = 0; i < numsteps; i++) {
    keys[i].update();
    Step s = seqr.steps[i];

    if (keys[i].pressed()) {  // press event
      step_push = i;
      step_push_millis = now;
      key_consumed[i] = false;

      if (i > 0 && keys[0].isPressed() && encoder_push_millis == 0) {
        // UI: SHIFT combo (key1 held + this key): open/close pages
        shift_used = true;
        key_consumed[i] = true;
        page_shift_key(i);
      }
      else if (page == PAGE_STEP && encoder_push_millis == 0) {
        // UI: STEP page, key selects that step
        page_sel = i;
        key_consumed[i] = true;
      }
      else if (encoder_push_millis > 0) { // pushing encoder
        // do nothing
      } else {
        if (!seqr.playing) {
          send_note_on(s.note, s.vel, s.gate, true, s.wave);
        }
      }
    }

    else if (keys[i].released()) {    // release event
      if (encoder_push_millis > 0) {
        // pushing encoder (gate edit combo): nothing to do on release
      } else if (key_consumed[i]) {
        // used by a page or SHIFT combo: no mute / note off
      } else if (shift_used) {  // UI: key1 as SHIFT, no mute toggling
        if (i == 0 && !seqr.playing) {
          send_note_off(s.note, s.vel, s.gate, true, s.wave);  // end key1's preview note
        }
      } else {  // UI: mutes or play notes
        // UI: playing: step keys = mutes/unmutes
        if (seqr.playing) {
          if (!step_edited) {
            seqr.steps[i].on = !seqr.steps[i].on;
          }
        }
        // UI: paused: step keys = play step notes
        else {
          send_note_off(s.note, s.vel, s.gate, true, s.wave);
        }
      }
      key_consumed[i] = false;
      step_push_millis = 0;
      step_push = -1;  //
      step_edited = false;
      // SHIFT combo is over once all keys are up
      bool any_pressed = false;
      for (uint8_t j = 0; j < numsteps; j++) {
        if (keys[j].isPressed()) { any_pressed = true; }
      }
      if (!any_pressed) { shift_used = false; }
    }
  }

  // DISPLAY update
  switch (page) {
    case PAGE_SYNTH: displayPageSynth(); break;
    case PAGE_WAVE: displayPageWave(); break;
    case PAGE_STEP: displayPageStep(); break;
    case PAGE_DELAY: displayPageDelay(); break;
    case PAGE_MIDI: displayPageMidi(); break;
    case PAGE_SLOTS: displayPageSlots(); break;
    default: displayUpdate(step_push); break;
  }
}

//
// --- display details
//
typedef struct { int x; int y;  const char* str;} pos_t;

//// {x,y} locations of play screen items
const int step_text_pos[] = { 0, 10, 16, 10, 32, 10, 48, 10, 64, 10, 80, 10, 96, 10, 112, 10 };
const pos_t bpm_text_pos    = {.x=0,  .y=57, .str="bpm:%3d" };
const pos_t trans_text_pos  = {.x=55, .y=57, .str="trs:%+2d" };
const pos_t seqno_text_pos  = {.x=0,  .y=45, .str="seq:%d" };
const pos_t playpause_pos   = {.x=128-image_playpause_w, .y=64-image_playpause_h, .str="" };  // bottom right

const pos_t oct_text_offset = { .x=3, .y=10,  .str="" };
const pos_t edit_text_offset= { .x=3, .y=22,  .str="" };

const char* note_strs[] = { "C ", "C#", "D ", "D# ", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B ", "C " };

int notenum_to_oct(int notenum) {
  return (notenum / 12) - 2;
}
const char* notenum_to_notestr(int notenum) {
  return note_strs[notenum % 12];
}

void displayUpdate(int selected_step) {
  display.clearDisplay();
  display.setFont(&myfont);
  display.setTextColor(WHITE, 0);
  for (int i = 0; i < numsteps; i++) {
    Step s = seqr.steps[i];
    const char* nstr = notenum_to_notestr(s.note);
    int o = notenum_to_oct(s.note);
    int x = step_text_pos[i * 2], y = step_text_pos[i * 2 + 1];
    display.setCursor(x, y);
    display.print(nstr);
    display.setCursor(x + oct_text_offset.x, y + oct_text_offset.y);
    display.printf("%1d", o);
    display.setCursor(x + edit_text_offset.x, y + edit_text_offset.y);
    display.print((i == selected_step) ? '^' : (s.on) ? ' '
                                                      : '*');
  }

  display.setFont(&myfont2);

  // bpm
  display.setCursor(bpm_text_pos.x, bpm_text_pos.y);
  display.printf(bpm_text_pos.str, (int)(seqr.tempo()));

  // transpose
  display.setCursor(trans_text_pos.x, trans_text_pos.y);
  display.printf(trans_text_pos.str, seqr.transpose);

  // seqno
  display.setCursor(seqno_text_pos.x, seqno_text_pos.y);
  display.printf(seqno_text_pos.str, seqr.seqno + 1);  // user sees 1-8

  // play/pause
  display.drawBitmap(playpause_pos.x, playpause_pos.y,
                     seqr.playing ? image_play_hover_bits : image_pause_hover_bits,
                     image_playpause_w, image_playpause_h, WHITE);

  display.display();
}

//
// --- pages (SHIFT + key2..7), built-in 6x8 font, coords are top-left
//
const char* page_title_left = "synthIO";

void drawPageTitle(const char* right) {
  display.fillRect(0, 0, dw, 9, WHITE);
  display.setTextColor(0);
  display.setTextWrap(false);
  display.setCursor(1, 1);
  display.print(page_title_left);
  display.setCursor(dw - 1 - 6 * strlen(right), 1);
  display.print(right);
  display.setTextColor(WHITE);
}

// label: when selected, select arrow at x-4 and label moves 3px right
void drawPageLabel(int x, int y, const char* str, bool selected) {
  if (selected) {
    display.drawBitmap(x - 4, y, image_Select_bits, image_select_w, image_select_h, WHITE);
    x += 3;
  }
  display.setCursor(x, y);
  display.print(str);
}

// outlined bar, filled in proportion to val/maxv
void drawPageBar(int x, int y, int w, int val, int maxv) {
  display.drawRect(x, y, w, 7, WHITE);
  int fw = (w - 2) * val / maxv;
  if (fw > 0) { display.fillRect(x + 1, y + 1, fw, 5, WHITE); }
}

// wave name + icon on a page row (icon at the right end of the row)
void drawWaveRow(int y, int wave) {
  display.setCursor(39, y);
  display.print(synth_wave_labels[wave]);
  display.drawBitmap(dw - wave_icon_w - 2, y, wave_icons[wave + 1], wave_icon_w, wave_icon_h, WHITE);
}

// page 1: SYNTH
void displayPageSynth() {
  display.clearDisplay();
  display.setFont();
  drawPageTitle("EDIT");
  int sel = synth_page_order[page_sel];
  const int row1 = 27, row2 = 40, row3 = 52;

  drawPageLabel(6, 14, "Wve", sel == SYN_WAVE);
  drawWaveRow(14, synth_params[SYN_WAVE]);

  drawPageLabel(5, row1, "Grt", sel == SYN_GRIT);
  drawPageBar(31, row1, 32, synth_params[SYN_GRIT], 127);
  drawPageLabel(5, row2, "Cut", sel == SYN_CUTOFF);
  drawPageBar(31, row2, 32, synth_params[SYN_CUTOFF], 127);
  drawPageLabel(5, row3, "Res", sel == SYN_RESO);
  drawPageBar(31, row3, 32, synth_params[SYN_RESO], 127);

  drawPageLabel(70, row1, "Atk", sel == SYN_ATTACK);
  drawPageBar(93, row1, 33, synth_params[SYN_ATTACK], 127);
  drawPageLabel(70, row2, "Dcy", sel == SYN_DECAY);
  drawPageBar(93, row2, 33, synth_params[SYN_DECAY], 127);
  drawPageLabel(70, row3, "Env", sel == SYN_ENVAMT);
  drawPageBar(93, row3, 33, synth_params[SYN_ENVAMT], 127);

  display.display();
}

// one model control row on the WAVE page: label, bar, value text
void drawModelControl(int y, int p, const char* label, bool selected) {
  drawPageLabel(5, y, label, selected);
  drawPageBar(31, y, 64, synth_params[p], 127);
  char val[8];
  if (is_ratio_param(p)) {
    snprintf(val, sizeof(val), "x%s", fm_ratio_labels[fm_ratio_index(synth_params[p])]);
  } else {
    snprintf(val, sizeof(val), "%d", synth_params[p]);
  }
  display.setCursor(100, y);
  display.print(val);
}

// page 2: WAVE (model + its two controls)
void displayPageWave() {
  display.clearDisplay();
  display.setFont();
  drawPageTitle("WAVE");
  int wave = synth_params[SYN_WAVE];

  drawPageLabel(6, 14, "Mdl", page_sel == 0);
  drawWaveRow(14, wave);

  if (wave_has_controls(wave)) {
    drawModelControl(27, synth_model_param_a[wave], synth_model_label_a[wave], page_sel == 1);
    drawModelControl(40, synth_model_param_b[wave], synth_model_label_b[wave], page_sel == 2);
  } else {
    display.setCursor(5, 33);
    display.print("no controls");
  }
  display.display();
}

// draw 8 boxes (2 rows x 4), selected one filled
void drawBoxGrid(int sel, void (*draw_content)(int i, int x, int y, uint16_t color)) {
  for (int i = 0; i < 8; i++) {
    int x = 9 + (i % 4) * 30, y = 25 + (i / 4) * 13;
    uint16_t color = WHITE;
    if (i == sel) {
      display.fillRect(x, y, 19, 11, WHITE);
      color = 0;
    } else {
      display.drawRect(x, y, 19, 11, WHITE);
    }
    draw_content(i, x, y, color);
  }
}

void drawStepBoxContent(int i, int x, int y, uint16_t color) {
  display.drawBitmap(x + 2, y + 2, wave_icons[seqr.steps[i].wave + 1], wave_icon_w, wave_icon_h, color);
}

// page 3: STEP (wave per step)
void displayPageStep() {
  display.clearDisplay();
  display.setFont();
  drawPageTitle("STEP");
  display.setCursor(5, 14);
  display.print("Wave per step");
  drawBoxGrid(page_sel, drawStepBoxContent);
  int w = seqr.steps[page_sel].wave;
  char info[22];
  snprintf(info, sizeof(info), "step %d: %s", page_sel + 1, (w < 0) ? "global" : synth_wave_labels[w]);
  display.setCursor(1, 56);
  display.print(info);
  display.display();
}

// page 4: DELAY
void displayPageDelay() {
  display.clearDisplay();
  display.setFont();
  drawPageTitle("DELAY");
  int sel = delay_page_order[page_sel];
  const int row1 = 27, row2 = 40;

  drawPageLabel(6, 14, "Tim", sel == SYN_DLY_TIME);
  display.setCursor(39, 14);
  display.print(dly_time_labels[synth_params[SYN_DLY_TIME]]);

  drawPageLabel(5, row1, "Fbk", sel == SYN_DLY_FBK);
  drawPageBar(31, row1, 32, synth_params[SYN_DLY_FBK], 127);
  drawPageLabel(5, row2, "Mix", sel == SYN_DLY_MIX);
  drawPageBar(31, row2, 32, synth_params[SYN_DLY_MIX], 127);

  drawPageLabel(70, row1, "Ton", sel == SYN_DLY_TONE);
  drawPageBar(93, row1, 33, synth_params[SYN_DLY_TONE], 127);
  drawPageLabel(70, row2, "Grt", sel == SYN_DLY_GRIT);
  drawPageBar(93, row2, 33, synth_params[SYN_DLY_GRIT], 127);

  // actual delay time at the current tempo
  int ms = (int)(dly_time_quarters[synth_params[SYN_DLY_TIME]] * 60000.0f / seqr.tempo());
  int max_ms = dly_max_samples * 1000 / synth_sample_rate;
  char info[22];
  snprintf(info, sizeof(info), "ping-pong %4d ms%s", min(ms, max_ms), (ms > max_ms) ? "!" : "");
  display.setCursor(5, 53);
  display.print(info);
  display.display();
}

// page 5: MIDI / step config / clock-trig out
void displayPageMidi() {
  display.clearDisplay();
  display.setFont();
  drawPageTitle("MIDI");
  const char* labels[CFG_COUNT] = { "Step size", "MIDI chan", "MIDI vel", "Send clk", "Out" };
  const int rows[CFG_COUNT] = { 12, 22, 32, 42, 52 };
  char val[8];
  for (int i = 0; i < CFG_COUNT; i++) {
    drawPageLabel(5, rows[i], labels[i], page_sel == i);
    display.setCursor(87, rows[i]);
    if (i == CFG_STEPSIZE) {
      display.print((cfg.step_size == SIXTEENTH_NOTE) ? "1/16" : (cfg.step_size == EIGHTH_NOTE) ? "1/8" : "1/4");
    } else if (i == CFG_MIDICHAN) {
      snprintf(val, sizeof(val), "%d", cfg.midi_chan);
      display.print(val);
    } else if (i == CFG_MIDIVEL) {
      snprintf(val, sizeof(val), "%d", cfg.midi_velocity);
      display.print(val);
    } else if (i == CFG_MIDISENDCLK) {
      display.print(cfg.midi_send_clock ? "on" : "off");
    } else {
      display.print(out_mode_labels[cfg.out_mode]);
    }
  }
  display.display();
}

void drawSlotBoxContent(int i, int x, int y, uint16_t color) {
  display.setTextColor(color);
  display.setCursor(x + 7, y + 2);
  display.print(i + 1);
  display.setTextColor(WHITE);
}

// page 6: LOAD/SAVE
void displayPageSlots() {
  display.clearDisplay();
  display.setFont();
  drawPageTitle("LOAD/SAVE");
  display.setCursor(5, 14);
  display.print("Seq slot");
  drawBoxGrid(page_sel, drawSlotBoxContent);
  display.setCursor(1, 56);
  if (page_msg[0] && millis() - page_msg_millis < 1500) {
    display.print(page_msg);
  } else {
    display.print("tap:load  hold:save");
  }
  display.display();
}

// 5 seconds of the "activity" animation, with the LED dance
void displaySplash() {
  const uint32_t splash_millis = 5000;
  const uint32_t frame_millis = 70;
  const int frame_count = sizeof(activity_64_64_14f_frames) / sizeof(activity_64_64_14f_frames[0]);
  uint32_t start = millis();
  uint32_t t;
  while ((t = millis() - start) < splash_millis) {
    int f = (t / frame_millis) % frame_count;
    display.clearDisplay();
    display.drawBitmap((dw - 64) / 2, (dh - 64) / 2, activity_64_64_14f_frames[f], 64, 64, WHITE);
    display.display();
    for (int j = 0; j < numsteps; j++) {  // a little LED dance
      int v = 30 + 30 * sin((j * 6.2 / 8) + t / 50.0);
      analogWrite(led_pins[j], v);
    }
  }
}
