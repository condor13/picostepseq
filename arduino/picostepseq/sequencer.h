/**
 * sequencer.h -- Sequencer for picostepseq
 * 28 Apr 2023 - @todbot / Tod Kurt
 * 15 Aug 2022 - @todbot / Tod Kurt
 * Part of https://github.com/todbot/picostepseq/
 */


const int numsteps = 8;
const int ticks_per_quarternote = 24; // 24 ppq per midi std
const int steps_per_beat_default = 4; // => ticks_per_step = 24/4 = 6

typedef struct {
  uint8_t note; // midi note
  uint8_t vel;  // midi velocity (always 127 currently)
  uint8_t gate; // how long note should be on in 0-15 arbitrary units
  bool on;      // does this note play or is or muted
  int8_t wave;  // per-step synth wave, -1 = follow global wave
} Step;

typedef enum {
  NONE,
  START,
  STOP,
  CLOCK,
} clock_type_t;

typedef enum {
  QUARTER_NOTE = 24,
  EIGHTH_NOTE = 12,
  SIXTEENTH_NOTE = 6,
} valid_ticks_per_step;

const int valid_step_sizes[] = {QUARTER_NOTE, EIGHTH_NOTE, SIXTEENTH_NOTE};
const int valid_step_sizes_cnt = 3;

typedef void (*TriggerFunc)(uint8_t note, uint8_t vel, uint8_t gate, bool on, int8_t wave);
typedef void (*ClockFunc)(clock_type_t type); // , int pos);
typedef void (*BarFunc)(void);  // called at the start of every bar (song mode hook)

// step playback direction within a bar
enum { DIR_FWD = 0, DIR_REV, DIR_PINGPONG, DIR_RANDOM, DIR_COUNT };

// stubs for when Sequencer object is only partially initialized
void fake_note_callback(uint8_t note, uint8_t vel, uint8_t gate, bool on, int8_t wave) {  }
void fake_clock_callback(clock_type_t type) { }

class StepSequencer {
public:

  uint32_t tick_micros; // "micros_per_tick", microsecs per clock (6 clocks / step; 4 steps / quarternote)
  uint32_t last_tick_micros; // only change in update()
  uint32_t held_gate_millis;
  int ticks_per_step;  // expected values: 6 = 1/16th, 12 = 1/8, 24 = 1/4,
  int ticki; // which midi clock we're on
  int stepi; // which sequencer step we're on
  int seqno;
  int transpose;
  bool playing;
  bool send_clock;
  uint32_t extclk_micros; // 0 = internal clock, non-zero = external clock
  TriggerFunc on_func;
  TriggerFunc off_func;
  ClockFunc clk_func;
  Step held_note;
  Step steps[numsteps];
  // per-bar playback shape (song mode sets these from the bar_func, defaults = normal 8-step forward)
  BarFunc bar_func;
  int bar_pos;         // position within the bar, 0..bar_len-1
  int bar_len;         // steps per bar, 1..numsteps
  int bar_dir;         // DIR_FWD, DIR_REV, DIR_PINGPONG (alternates per bar_pass), DIR_RANDOM
  int bar_pass;        // how many times this bar shape has repeated (for ping-pong)
  int song_transpose;  // added on top of transpose
  bool rest;           // true = advance but play nothing

  StepSequencer( float atempo=120, uint8_t aseqno=0 ) {
    transpose = 0;
    last_tick_micros = 0;
    stepi = 0;
    ticki = 0;
    ticks_per_step = 6; // 6 = 1/16th, 12 = 1/8, 24 = 1/4,
    seqno = aseqno;
    playing = false;
    extclk_micros = 0;
    send_clock = false;
    set_tempo(atempo);
    on_func = fake_note_callback;
    off_func = fake_note_callback;
    clk_func = fake_clock_callback;
    bar_func = nullptr;
    reset_bar_shape();
  }

  // back to normal: 8 steps, forward, no song transpose
  void reset_bar_shape() {
    bar_pos = -1;
    bar_len = numsteps;
    bar_dir = DIR_FWD;
    bar_pass = 0;
    song_transpose = 0;
    rest = false;
  }

  // which step plays at bar position pos
  int step_for_pos(int pos) {
    bool reverse = (bar_dir == DIR_REV) || (bar_dir == DIR_PINGPONG && (bar_pass & 1));
    if (bar_dir == DIR_RANDOM) { return rand() % bar_len; }
    return reverse ? (bar_len - 1 - pos) : pos;
  }

  // get tempo as floating point, computed dynamically from ticks_micros
  float tempo() {
    return 60 * 1000 * 1000 / tick_micros / ticks_per_quarternote; //steps_per_beat * ticks_per_step);
  }

  // set tempo as floating point, computes ticks_micros
  void set_tempo(float bpm) {
    tick_micros = 60 * 1000 * 1000 / bpm / ticks_per_quarternote;
  }

  void update() {
    uint32_t now_micros = micros();
    if( (now_micros - last_tick_micros) < tick_micros ) { return; }  // not yet
    last_tick_micros = now_micros;

    // // serious debug cruft here
    // Serial.printf("up:%2d %d  %6ld %6ld  dt:%6ld t:%6ld last:%6ld\n",
    //               ticki, playing,  held_gate_millis, millis(),
    //               delta_t, tick_micros, last_tick_micros );
    //Serial.printf("up: %ld \t h:%2x/%2x/%1x/%d\n", held_gate_millis,
    //              held_note.note, held_note.vel, held_note.gate, held_note.on);

    // if we have a held note and it's time to turn it off, turn it off
    if( held_gate_millis != 0 && millis() >= held_gate_millis ) {
      held_gate_millis = 0;
      off_func( held_note.note, held_note.vel, held_note.gate, held_note.on, held_note.wave);
    }

    if( send_clock && playing && !extclk_micros ) {
      clk_func( CLOCK );
    }

    if( ticki == 0 ) {
      // do a sequence step (i.e. every "ticks_per_step" ticks)
      if( extclk_micros ) {
        // do nothing, let midi clock trigger notes, but fall back to
        // internal clock if not externally clocked for a while
        if( (now_micros - extclk_micros) > tick_micros * ticks_per_quarternote ) {
          extclk_micros = 0;
          Serial.println("Turning EXT CLOCK off");
        }
      }
      else {  // else internally clocked
        trigger(now_micros, 0); // delta_t);
      }
    }

    // increment our ticks-per-step counter: 0,1,2,3,4,5, 0,1,2,3,4,5, ...
    ticki = (ticki + 1) % ticks_per_step;
  }

  // Trigger step when externally clocked (turns on external clock flag)
  void trigger_ext(uint32_t now_micros) {
    extclk_micros = now_micros;
    trigger(now_micros,0);
  }

  // Trigger step in sequence, when internally clocked
  //void trigger(uint32_t now, uint16_t delta_t) {
  void trigger(uint32_t now_micros, uint16_t delta_t) {
    if( !playing ) { return; }
    (void)delta_t; // silence unused variable

    bar_pos++;  // go to next step
    if (bar_pos >= bar_len) { bar_pos = 0; }
    if (bar_pos == 0 && bar_func) { bar_func(); }  // song mode may load a new pattern / bar shape here
    if (bar_pos >= bar_len) { bar_pos = 0; }
    stepi = step_for_pos(bar_pos);

    if (rest) { return; }  // nothing to play this bar

    Step s = steps[stepi];
    int n = s.note + transpose + song_transpose;
    s.note = (n < 0) ? 0 : (n > 127) ? 127 : n;

    on_func(s.note, s.vel, s.gate, s.on, s.wave);

    held_note = s;
    uint32_t micros_per_step = ticks_per_step * tick_micros;
    uint32_t gate_micros = s.gate * micros_per_step / 16;  // s.gate is arbitary 0-15 value
    held_gate_millis = (now_micros + gate_micros) / 1000;
  }

  void toggle_play_stop() {
    if( playing ) { stop(); }
    else { play(); }
  }

  // signal to sequencer/MIDI core we want to start playing
  void play() {
    stepi = -1; // hmm, but goes to 0 on first downbeat
    bar_pos = -1;
    ticki = 0;
    playing = true;
    if(send_clock && !extclk_micros) {
      clk_func( START );
    }
  }

  // signal to sequencer/MIDI core we want to stop/pause playing
  // FIXME: unclear this works correctly
  void pause() {
    playing = false;
  }

  // continue after pause() from where it stopped
  void resume() {
    playing = true;
  }

  // signal to sequencer/MIDI core we want to stop playing
  void stop() {
    playing = false;
    stepi = -1;
    bar_pos = -1;
    if(send_clock && !extclk_micros) {
      clk_func( STOP );
    }
  }

};
