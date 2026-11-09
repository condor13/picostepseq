/**
 * song.h -- TRACKER song mode for picostepseq
 *
 * A song is a loop of up to 16 cells, played down each column of the TRACKER page.
 * Each cell plays one of the 8 saved patterns with:
 *   rep  -- x1..x6, x8 (repeat N times) or /2../5 (play only every Nth time round the loop)
 *   trs  -- transpose -24..+24 semitones
 *   len  -- bar length 1..8 steps
 *   dir  -- fwd / rev / ping-pong (alternates direction each time the cell plays) / random
 *   dmix -- delay mix for this cell, -1 = use the DELAY page setting
 * The first empty cell marks the end of the loop.
 *
 * song_bar() runs on core0 (from the sequencer, at the start of every bar).
 * Everything else runs on core1 (UI).
 */

const int song_len = 16;
const int8_t song_reps[] = { -5, -4, -3, -2, 1, 2, 3, 4, 5, 6, 8 };  // /5 .. x1 .. x8
const int song_reps_count = sizeof(song_reps) / sizeof(song_reps[0]);
const char* const song_dir_labels[DIR_COUNT] = { "fwd", "rev", "png", "rnd" };

typedef struct {
  int8_t pat;   // pattern 0-7, -1 = empty (loop end)
  int8_t rep;   // 1..8 = xN, -2..-5 = /N
  int8_t trs;   // -24..24
  uint8_t len;  // 1..numsteps
  uint8_t dir;  // DIR_*
  int16_t dmix; // -1 = global, 0..127
} SongCell;

SongCell song[song_len];
volatile bool song_mode = false;        // song is the active playback (playing or paused)
volatile int song_cell_playing = -1;    // for display
int song_cell = -1;                     // core0 playback state
int song_rep_left = 0;
int song_loop = 0;
Step song_saved_steps[numsteps];        // pattern-mode steps, restored when song mode ends
bool song_dirty = false;
const char* song_file = "/song.json";

void song_clear_cell(SongCell& c) {
  c.pat = -1;
  c.rep = 1;
  c.trs = 0;
  c.len = numsteps;
  c.dir = DIR_FWD;
  c.dmix = -1;
}

void song_init() {
  for (int i = 0; i < song_len; i++) { song_clear_cell(song[i]); }
}

bool song_cell_has_mods(const SongCell& c) {
  return c.pat >= 0 && (c.trs != 0 || c.len != numsteps || c.dir != DIR_FWD || c.dmix >= 0);
}

// text shown in a TRACKER cell: "..", "03", "3x4", "3/2"
// (lives here, not in the .ino, because the Arduino IDE puts auto-generated
//  prototypes above the #include of this file, where SongCell isn't known yet)
void song_cell_text(const SongCell& c, char* buf, int len) {
  if (c.pat < 0) { snprintf(buf, len, ".."); }
  else if (c.rep == 1) { snprintf(buf, len, "%02d", c.pat + 1); }
  else if (c.rep > 1) { snprintf(buf, len, "%dx%d", c.pat + 1, c.rep); }
  else { snprintf(buf, len, "%d/%d", c.pat + 1, -c.rep); }
}

// core0: called by the sequencer at the start of every bar
void song_bar() {
  if (!song_mode) { return; }
  if (song_rep_left > 0) {  // same cell again
    song_rep_left--;
    seqr.bar_pass++;
    return;
  }
  // find the next cell to play (skipping divided cells that sit out this loop)
  for (int tries = 0; tries < song_len * 60; tries++) {
    song_cell++;
    if (song_cell >= song_len || song[song_cell].pat < 0) {  // end of loop
      song_cell = 0;
      song_loop++;
    }
    SongCell c = song[song_cell];
    if (c.pat < 0) { break; }  // empty song
    if (c.rep < 0 && (song_loop % (-c.rep)) != 0) { continue; }  // "/N": not this time round
    for (int i = 0; i < numsteps; i++) {
      seqr.steps[i] = sequences[c.pat][i];
    }
    seqr.bar_len = constrain(c.len, 1, numsteps);
    seqr.bar_dir = c.dir;
    seqr.bar_pass = song_loop;  // ping-pong alternates per repeat and per loop
    seqr.song_transpose = c.trs;
    seqr.rest = false;
    synth_dly_mix_override = c.dmix;
    song_rep_left = (c.rep > 1) ? c.rep - 1 : 0;
    song_cell_playing = song_cell;
    return;
  }
  // nothing playable: rest
  seqr.rest = true;
  song_cell_playing = -1;
}

// start the song from the first cell (sequence steps are backed up by song_backup())
void song_start() {
  song_cell = -1;
  song_rep_left = 0;
  song_loop = 0;
  song_mode = true;
  seqr.play();
}

void song_backup() {
  for (int i = 0; i < numsteps; i++) { song_saved_steps[i] = seqr.steps[i]; }
}

// leave song mode: stop, restore pattern-mode steps and normal bar shape
void song_end() {
  if (!song_mode) { return; }
  seqr.stop();
  song_mode = false;
  delay(5);  // let core0 finish a bar_func that may be in flight
  seqr.reset_bar_shape();
  synth_dly_mix_override = -1;
  song_cell_playing = -1;
  for (int i = 0; i < numsteps; i++) { seqr.steps[i] = song_saved_steps[i]; }
}

//
// --- song load / save
//

void song_write() {
  if (!song_dirty) { return; }
  song_dirty = false;
  DynamicJsonDocument doc(4096);
  for (int i = 0; i < song_len; i++) {
    JsonArray a = doc.createNestedArray();
    a.add(song[i].pat);
    a.add(song[i].rep);
    a.add(song[i].trs);
    a.add(song[i].len);
    a.add(song[i].dir);
    a.add(song[i].dmix);
  }
  LittleFS.remove(song_file);
  File file = LittleFS.open(song_file, "w");
  if (!file) {
    Serial.println("song_write: Failed to create file");
    return;
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("song saved");
}

void song_read() {
  song_init();
  File file = LittleFS.open(song_file, "r");
  if (!file) {
    Serial.println("song_read: no song file, starting empty");
    return;
  }
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.print("song_read: deserialize failed: ");
    Serial.println(error.c_str());
    return;
  }
  for (int i = 0; i < song_len && i < (int)doc.size(); i++) {
    JsonArray a = doc[i];
    SongCell& c = song[i];
    c.pat = constrain((int)(a[0] | -1), -1, numseqs - 1);
    c.rep = a[1] | 1;
    bool rep_ok = false;
    for (int k = 0; k < song_reps_count; k++) { if (song_reps[k] == c.rep) { rep_ok = true; } }
    if (!rep_ok) { c.rep = 1; }
    c.trs = constrain((int)(a[2] | 0), -24, 24);
    c.len = constrain((int)(a[3] | numsteps), 1, numsteps);
    c.dir = constrain((int)(a[4] | 0), 0, DIR_COUNT - 1);
    c.dmix = constrain((int)(a[5] | -1), -1, 127);
  }
}
