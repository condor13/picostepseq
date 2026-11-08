

//
// --- sequence load / save functions
//

uint32_t last_sequence_write_millis = 0;

// write all sequences to "disk"
void sequences_write() {
  Serial.println("sequences_write");
  // save wear & tear on flash, only allow writes every 10 seconds
  if (millis() - last_sequence_write_millis < 10 * 1000) {  // only allow writes every 10 secs
    Serial.println("sequences_write: too soon, wait a bit more");
  }
  last_sequence_write_millis = millis();

  DynamicJsonDocument doc(8192);  // assistant said 6144
  for (int j = 0; j < numseqs; j++) {
    JsonArray seq_array = doc.createNestedArray();
    for (int i = 0; i < numsteps; i++) {
      Step s = sequences[j][i];
      JsonArray step_array = seq_array.createNestedArray();
      step_array.add(s.note);
      step_array.add(s.vel);
      step_array.add(s.gate);
      step_array.add(s.on);
      step_array.add(s.wave);
    }
  }

  LittleFS.remove(save_file);
  File file = LittleFS.open(save_file, "w");
  if (!file) {
    Serial.println("sequences_write: Failed to create file");
    return;
  }
  if (serializeJson(doc, file) == 0) {
    Serial.println(F("sequences_write: Failed to write to file"));
  }
  file.close();
  Serial.print("saved_sequences_json = \"");
  serializeJson(doc, Serial);
  Serial.println("\"\nsequences saved");
}

// read all sequences from "disk"
void sequences_read() {
  Serial.println("sequences_read");
  DynamicJsonDocument doc(8192);  // assistant said 6144

  File file = LittleFS.open(save_file, "r");
  if (!file) {
    Serial.println("sequences_read: no sequences file. Using ROM default...");
    DeserializationError error = deserializeJson(doc, default_saved_sequences_json);
    if (error) {
      Serial.print("sequences_read: deserialize default failed: ");
      Serial.println(error.c_str());
      return;
    }
  } else {
    DeserializationError error = deserializeJson(doc, file);  // inputLength);
    if (error) {
      Serial.print("sequences_read: deserialize failed: ");
      Serial.println(error.c_str());
      return;
    }
  }

  for (int j = 0; j < numseqs; j++) {
    JsonArray seq_array = doc[j];
    for (int i = 0; i < numsteps; i++) {
      JsonArray step_array = seq_array[i];
      Step s;
      s.note = step_array[0];
      s.vel = step_array[1];
      s.gate = step_array[2];
      s.on = step_array[3];
      s.wave = step_array[4] | -1;  // older files have no wave: follow global
      if (s.wave < -1 || s.wave >= WAVE_COUNT) { s.wave = -1; }
      sequences[j][i] = s;
    }
  }
  file.close();
}

// Load a single sequence from into the sequencer from RAM storage
void sequence_load(int seq_num) {
  Serial.printf("sequence_load:%d\n", seq_num);
  for (int i = 0; i < numsteps; i++) {
    seqr.steps[i] = sequences[seq_num][i];
  }
  seqr.seqno = seq_num;
}

// Store current sequence in sequencer to RAM storage"""
void sequence_save(int seq_num) {
  Serial.printf("sequence_save:%d\n", seq_num);
  for (int i = 0; i < numsteps; i++) {
    sequences[seq_num][i] = seqr.steps[i];
    ;
  }
}

//
// --- synth patch load / save (one global patch)
//

const char* synth_save_file = "/synth_patch.json";

// write synth patch to "disk", but only if it changed
void synth_patch_write() {
  if (!synth_patch_dirty) { return; }
  synth_patch_dirty = false;
  StaticJsonDocument<256> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < SYN_COUNT; i++) {
    arr.add(synth_params[i]);
  }
  LittleFS.remove(synth_save_file);
  File file = LittleFS.open(synth_save_file, "w");
  if (!file) {
    Serial.println("synth_patch_write: Failed to create file");
    return;
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("synth patch saved");
}

// read synth patch from "disk", keeps defaults if no file
void synth_patch_read() {
  File file = LittleFS.open(synth_save_file, "r");
  if (!file) {
    Serial.println("synth_patch_read: no patch file, using defaults");
    return;
  }
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.print("synth_patch_read: deserialize failed: ");
    Serial.println(error.c_str());
    return;
  }
  JsonArray arr = doc.as<JsonArray>();
  for (int i = 0; i < SYN_COUNT && i < (int)arr.size(); i++) {
    int v = arr[i];
    if (v < 0) { v = 0; }
    if (v > synth_param_max[i]) { v = synth_param_max[i]; }
    synth_params[i] = v;
  }
}
