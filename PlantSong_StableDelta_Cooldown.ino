#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <math.h>

// =====================================================
// AUDIO
// =====================================================
// GUItool: begin automatically generated code
AudioPlaySdWav           playSdWav1;
AudioAmplifier           amp1;
AudioEffectFade          fade1;
AudioEffectFreeverb      freeverb1;
AudioMixer4              mixer1;
AudioOutputMQS           mqs1;
AudioConnection          patchCord1(playSdWav1, 0, amp1, 0);
AudioConnection          patchCord2(amp1, fade1);
AudioConnection          patchCord3(fade1, 0, mixer1, 0);
AudioConnection          patchCord4(fade1, freeverb1);
AudioConnection          patchCord5(freeverb1, 0, mixer1, 1);
AudioConnection          patchCord6(mixer1, 0, mqs1, 0);
AudioConnection          patchCord7(mixer1, 0, mqs1, 1);
// GUItool: end automatically generated code

// =====================================================
// SD / PINS
// =====================================================
#define SDCARD_CS_PIN BUILTIN_SDCARD
#define SDCARD_MOSI_PIN 11
#define SDCARD_SCK_PIN 13

const int interruptPin   = 5;
const int LED1           = 2;
const int LED2           = 3;
const int THRESH_POT_PIN = A10;
const int BANK_POT_PIN   = A11;

// =====================================================
// CONFIG
// =====================================================
// Middle C = MIDI note 60.
// setnote 1 = C4, setnote 2 = C#4, etc.
const int MIDI_ROOT_NOTE = 60;   // Middle C / C4

const uint8_t MIDI_CH_1 = 1;
const uint8_t MIDI_CH_2 = 2;
const uint8_t MIDI_CH_3 = 3;

// =====================================================
// TRUE MIDI POLYPHONY SETTINGS
// =====================================================
// This is NOT chord mode.
// Each trigger sends only one note per active MIDI channel.
// Older notes keep ringing until their own duration expires.
// Each MIDI channel can hold up to 5 overlapping notes.
const uint8_t POLYPHONY_PER_CHANNEL = 5;

// If all 5 voice slots are full, this decides what happens.
// true  = steal the oldest voice and replace it with the new note.
// false = ignore the new note until a voice slot is free.
const bool STEAL_OLDEST_VOICE = true;

// =====================================================
// DELTA SPIKE SETTINGS FOR MIDI CHANNELS 2 AND 3
// =====================================================
// Channel 1 uses the normal pot-controlled trigger.
// Channel 2 and 3 are opened by raw delta spike logic only.
// The sensitivity pot does NOT decide when CH2 or CH3 open.

const float CH2_DELTA_SPIKE_RATIO = 4.0f;
const float CH3_DELTA_SPIKE_RATIO = 9.0f;

const unsigned long CH2_MIN_RAW_DELTA = 180;
const unsigned long CH3_MIN_RAW_DELTA = 350;

// =====================================================
// CHANNEL 2 / CHANNEL 3 COOLDOWNS
// =====================================================
// These stop CH2 and CH3 from triggering too often,
// even if the delta spike condition is repeatedly true.
//
// Increase these values if CH2 / CH3 still come in too much.
// Lower these values if CH2 / CH3 feel too rare.
const unsigned long CH2_COOLDOWN_MS = 2500;
const unsigned long CH3_COOLDOWN_MS = 6000;

unsigned long lastCh2Trigger = 0;
unsigned long lastCh3Trigger = 0;

// Slow baseline learning.
// Lower values stop the delta baseline from rising too quickly.
const float DELTA_AVG_SMOOTHING = 0.008f;

// Big spikes should NOT be learned into the baseline.
// If a delta is more than this ratio above average, the average freezes for that cycle.
const float DELTA_BASELINE_FREEZE_RATIO = 1.7f;

float deltaAverage = 80.0f;

const byte samplesize = 11;
const byte analysize  = samplesize - 1;

float threshold = 0.1f;
const int fadeTime = 8;

// Bank select zones
const int BANK_LEFT_MAX  = 350;
const int BANK_RIGHT_MIN = 700;

// Debug / serial
byte rawSerial = 1;
unsigned long rawSerialTime = 0;
int rawSerialDelay = 0;
unsigned long currentMillis = 0;

// =====================================================
// SAMPLE CAPTURE
// =====================================================
volatile unsigned long microseconds = 0;
volatile byte sampleIndex = 0;
volatile unsigned long samples[samplesize];

// =====================================================
// BANKS
// =====================================================
volatile uint8_t sampleBank = 1;  // 1, 2, 3

const char* bank1[17] = {
  nullptr,
  "a1.wav","a2.wav","a3.wav","a4.wav",
  "a5.wav","a6.wav","a7.wav","a8.wav",
  "a9.wav","a10.wav","a11.wav","a12.wav",
  "a13.wav","a14.wav","a15.wav","a16.wav"
};

const char* bank2[17] = {
  nullptr,
  "b1.wav","b2.wav","b3.wav","b4.wav",
  "b5.wav","b6.wav","b7.wav","b8.wav",
  "b9.wav","b10.wav","b11.wav","b12.wav",
  "b13.wav","b14.wav","b15.wav","b16.wav"
};

const char* bank3[17] = {
  nullptr,
  "c1.wav","c2.wav","c3.wav","c4.wav",
  "c5.wav","c6.wav","c7.wav","c8.wav",
  "c9.wav","c10.wav","c11.wav","c12.wav",
  "c13.wav","c14.wav","c15.wav","c16.wav"
};

const char* getSampleName(uint8_t bank, int setnote) {
  if (setnote < 1 || setnote > 16) return nullptr;

  switch (bank) {
    case 1: return bank1[setnote];
    case 2: return bank2[setnote];
    case 3: return bank3[setnote];
    default: return bank1[setnote];
  }
}

// =====================================================
// CHROMATIC MIDI MAPPING FROM MIDDLE C
// =====================================================
int chromaticFromMiddleC(int setnote) {
  setnote = constrain(setnote, 1, 16);
  return MIDI_ROOT_NOTE + (setnote - 1);
}

// =====================================================
// MIDI NOTE STATE: 3 MIDI CHANNELS x 5 POLY VOICES
// =====================================================
struct PolyVoice {
  bool active = false;
  int note = -1;
  uint8_t channel = 1;
  unsigned long offAt = 0;
  unsigned long startedAt = 0;
};

PolyVoice voices[3][POLYPHONY_PER_CHANNEL];

uint8_t channelToIndex(uint8_t channel) {
  if (channel == MIDI_CH_1) return 0;
  if (channel == MIDI_CH_2) return 1;
  if (channel == MIDI_CH_3) return 2;
  return 0;
}

void noteOffVoice(uint8_t chIndex, uint8_t voiceIndex) {
  if (voices[chIndex][voiceIndex].active && voices[chIndex][voiceIndex].note >= 0) {
    usbMIDI.sendNoteOff(
      voices[chIndex][voiceIndex].note,
      0,
      voices[chIndex][voiceIndex].channel
    );

    voices[chIndex][voiceIndex].active = false;
    voices[chIndex][voiceIndex].note = -1;
    voices[chIndex][voiceIndex].offAt = 0;
    voices[chIndex][voiceIndex].startedAt = 0;
  }
}

void checkPolyNotes() {
  unsigned long now = millis();

  for (uint8_t ch = 0; ch < 3; ch++) {
    for (uint8_t i = 0; i < POLYPHONY_PER_CHANNEL; i++) {
      if (voices[ch][i].active && (long)(now - voices[ch][i].offAt) >= 0) {
        noteOffVoice(ch, i);
      }
    }
  }

  usbMIDI.send_now();
}

void stopAllMidiNotes() {
  for (uint8_t ch = 0; ch < 3; ch++) {
    for (uint8_t i = 0; i < POLYPHONY_PER_CHANNEL; i++) {
      noteOffVoice(ch, i);
    }
  }

  usbMIDI.send_now();
}

int findFreeVoice(uint8_t chIndex) {
  for (uint8_t i = 0; i < POLYPHONY_PER_CHANNEL; i++) {
    if (!voices[chIndex][i].active) {
      return i;
    }
  }

  return -1;
}

int findOldestVoice(uint8_t chIndex) {
  uint8_t oldest = 0;
  unsigned long oldestStart = voices[chIndex][0].startedAt;

  for (uint8_t i = 1; i < POLYPHONY_PER_CHANNEL; i++) {
    if (voices[chIndex][i].startedAt < oldestStart) {
      oldestStart = voices[chIndex][i].startedAt;
      oldest = i;
    }
  }

  return oldest;
}

void setPolyNote(uint8_t channel, int note, uint8_t velocity, unsigned long durationMs) {
  uint8_t chIndex = channelToIndex(channel);

  int slot = findFreeVoice(chIndex);

  if (slot < 0) {
    if (!STEAL_OLDEST_VOICE) {
      return;
    }

    slot = findOldestVoice(chIndex);
    noteOffVoice(chIndex, slot);
  }

  note = constrain(note, 0, 127);

  usbMIDI.sendNoteOn(note, velocity, channel);

  voices[chIndex][slot].active = true;
  voices[chIndex][slot].note = note;
  voices[chIndex][slot].channel = channel;
  voices[chIndex][slot].startedAt = millis();
  voices[chIndex][slot].offAt = millis() + durationMs;

  usbMIDI.send_now();

  Serial.print("Poly Note On - CH ");
  Serial.print(channel);
  Serial.print(" Voice ");
  Serial.print(slot + 1);
  Serial.print(" Note ");
  Serial.print(note);
  Serial.print(" Dur ");
  Serial.println(durationMs);
}

void triggerPolyBiodataNotes(
  int note1,
  int note2,
  int note3,
  uint8_t velocity,
  unsigned long durationMs,
  bool playCh2,
  bool playCh3
) {
  // No stopAllMidiNotes() here.
  // That is the key difference from chord/retrigger mode.
  // Existing notes keep playing until their own offAt time expires.

  setPolyNote(MIDI_CH_1, note1, velocity, durationMs);

  if (playCh2) {
    setPolyNote(MIDI_CH_2, note2, velocity, durationMs);
  }

  if (playCh3) {
    setPolyNote(MIDI_CH_3, note3, velocity, durationMs);
  }
}

// =====================================================
// HELPERS
// =====================================================
void updateThresholdFromPot() {
  float raw = analogRead(THRESH_POT_PIN) / 1023.0f;
  float sensitivity = 1.0f - raw;
  sensitivity = powf(sensitivity, 1.8f);
  threshold = 0.02f + sensitivity * 4.0f;
}

void updateBankFromPot() {
  static uint8_t lastBank = 1;

  int bankRaw = analogRead(BANK_POT_PIN);
  uint8_t newBank;

  if (bankRaw <= BANK_LEFT_MAX) {
    newBank = 1;
  } else if (bankRaw >= BANK_RIGHT_MIN) {
    newBank = 3;
  } else {
    newBank = 2;
  }

  if (newBank != lastBank) {
    lastBank = newBank;
    sampleBank = newBank;

    Serial.print("Sample Bank = ");
    Serial.println(sampleBank);
  }
}

void playSelectedSample(uint8_t bank, int setnote) {
  const char* fname = getSampleName(bank, setnote);
  if (!fname) return;

  fade1.fadeOut(fadeTime);
  delay(fadeTime);
  playSdWav1.stop();
  fade1.fadeIn(fadeTime);

  playSdWav1.play(fname);

  Serial.print("Bank ");
  Serial.print(bank);
  Serial.print(" Sample ");
  Serial.print(setnote);
  Serial.print(" -> ");
  Serial.println(fname);
}

// =====================================================
// ISR
// =====================================================
void sample() {
  if (sampleIndex < samplesize) {
    samples[sampleIndex] = micros() - microseconds;
    microseconds = samples[sampleIndex] + microseconds;
    sampleIndex += 1;
  }
}

// =====================================================
// ANALYSIS
// =====================================================
void analyzeSample() {
  if (sampleIndex < samplesize) return;

  unsigned long averg = 0;
  unsigned long maxim = 0;
  unsigned long minim = 100000;
  float stdevi = 0.0f;
  unsigned long delta = 0;
  byte change = 0;
  unsigned long dur = 0;
  byte vel = 0;
  int setnote = 0;

  unsigned long sampanalysis[analysize];

  for (byte i = 0; i < analysize; i++) {
    sampanalysis[i] = samples[i + 1];

    if (sampanalysis[i] > maxim) maxim = sampanalysis[i];
    if (sampanalysis[i] < minim) minim = sampanalysis[i];

    averg += sampanalysis[i];
    stdevi += (float)sampanalysis[i] * (float)sampanalysis[i];
  }

  averg = averg / analysize;
  stdevi = sqrtf(stdevi / analysize - (float)averg * (float)averg);
  if (stdevi < 1.0f) stdevi = 1.0f;

  delta = maxim - minim;

  // Main trigger still uses the sensitivity pot.
  // This controls whether Channel 1 and the sample trigger.
  if (delta > (stdevi * threshold)) {
    change = 1;
  }

  // =====================================================
  // POT-INDEPENDENT DELTA SPIKE TRACKING - STABLE VERSION
  // =====================================================
  // Calculate spike ratio BEFORE updating the average.
  // This stops the current spike from immediately raising the baseline.
  //
  // The sensitivity pot does NOT affect this section.
  // CH2 and CH3 are opened from raw delta spike behaviour only.

  if (deltaAverage < 1.0f) deltaAverage = 1.0f;

  float deltaSpikeRatio = (float)delta / deltaAverage;

  bool ch2Ready = (millis() - lastCh2Trigger) > CH2_COOLDOWN_MS;
  bool ch3Ready = (millis() - lastCh3Trigger) > CH3_COOLDOWN_MS;

  bool playCh2 = (delta >= CH2_MIN_RAW_DELTA) &&
                 (deltaSpikeRatio >= CH2_DELTA_SPIKE_RATIO) &&
                 ch2Ready;

  bool playCh3 = (delta >= CH3_MIN_RAW_DELTA) &&
                 (deltaSpikeRatio >= CH3_DELTA_SPIKE_RATIO) &&
                 ch3Ready;

  if (playCh2) lastCh2Trigger = millis();
  if (playCh3) lastCh3Trigger = millis();

  // Update the baseline only when the current delta is not a large spike.
  // This prevents the patch from "getting used to" strong spikes after a minute.
  if (deltaSpikeRatio < DELTA_BASELINE_FREEZE_RATIO) {
    deltaAverage = (deltaAverage * (1.0f - DELTA_AVG_SMOOTHING)) + ((float)delta * DELTA_AVG_SMOOTHING);
  }

  if (change) {
    dur = 150 + map(delta % 127, 0, 127, 100, 2200);
    vel = map(delta % 127, 0, 127, 80, 110);

    // Keep sample selection logic as-is.
    // This selects one of 16 samples in the currently selected bank.
    setnote = map((averg + delta) % 127, 0, 127, 1, 16);

    static uint8_t last_bank_for_audio = 1;
    static int lastTriggeredSetnote = -1;

    if (setnote != lastTriggeredSetnote || sampleBank != last_bank_for_audio) {
      playSelectedSample(sampleBank, setnote);
      lastTriggeredSetnote = setnote;
      last_bank_for_audio = sampleBank;
    }

    // =====================================================
    // TRUE POLYPHONIC MIDI NOTES FROM MIDDLE C
    // =====================================================
    // This sends only ONE note per active MIDI channel per trigger.
    // It does NOT send a chord.
    // Previous notes keep ringing until their duration expires,
    // so each channel can build up to 5 overlapping notes.
    // =====================================================

    int baseNote = chromaticFromMiddleC(setnote);

    int note1 = baseNote;
    int note2 = constrain(baseNote + (delta % 12), 0, 127);
    int note3 = constrain(baseNote + ((delta / 3) % 12), 0, 127);

    triggerPolyBiodataNotes(note1, note2, note3, vel, dur, playCh2, playCh3);

    Serial.print("Pot Threshold: "); Serial.println(threshold);
    Serial.print("Setnote: "); Serial.println(setnote);
    Serial.print("Delta: "); Serial.println(delta);
    Serial.print("Delta Average: "); Serial.println(deltaAverage);
    Serial.print("Delta Spike Ratio: "); Serial.println(deltaSpikeRatio);
    Serial.print("CH2 Cooldown Ready: "); Serial.println(ch2Ready ? "YES" : "NO");
    Serial.print("CH3 Cooldown Ready: "); Serial.println(ch3Ready ? "YES" : "NO");
    Serial.print("CH1 Note: "); Serial.println(note1);

    if (playCh2) {
      Serial.print("CH2 Note: "); Serial.println(note2);
    } else {
      Serial.println("CH2: OFF - no large delta spike");
    }

    if (playCh3) {
      Serial.print("CH3 Note: "); Serial.println(note3);
    } else {
      Serial.println("CH3: OFF - no extreme delta spike");
    }

    Serial.print("Velocity: "); Serial.println(vel);
    Serial.print("Duration: "); Serial.println(dur);
  }

  if (rawSerial && (change || (currentMillis > rawSerialTime + rawSerialDelay))) {
    rawSerialTime = currentMillis;
  }

  sampleIndex = 0;
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  AudioMemory(128);
  Serial.begin(57600);

  pinMode(interruptPin, INPUT_PULLUP);
  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);

  digitalWrite(LED1, HIGH);
  digitalWrite(LED2, HIGH);
  delay(250);
  digitalWrite(LED1, LOW);
  delay(250);
  digitalWrite(LED2, LOW);

  attachInterrupt(interruptPin, sample, RISING);

  SPI.setMOSI(SDCARD_MOSI_PIN);
  SPI.setSCK(SDCARD_SCK_PIN);

  if (!SD.begin(SDCARD_CS_PIN)) {
    while (1) {
      Serial.println("Unable to access the SD card");
      delay(500);
    }
  }

  mixer1.gain(0, 0.5f);
  mixer1.gain(1, 0.5f);
  amp1.gain(0.5f);

  freeverb1.roomsize(0.9f);
  freeverb1.damping(0.3f);

  delay(500);

  playSdWav1.stop();
  playSdWav1.play("Sample_01.wav");

  Serial.println("Playing startup sample");
  Serial.println("MIDI chromatic from Middle C");
  Serial.println("TRUE polyphonic MIDI mode - NOT chord mode");
  Serial.println("Each channel can hold up to 5 overlapping notes");
  Serial.println("Channel 1 = one note per normal pot-controlled trigger");
  Serial.println("Channel 2 = one note per delta spike");
  Serial.println("Channel 3 = one note per stronger delta spike");
  Serial.println("Existing notes continue until their own duration expires");
  Serial.println("Stable delta spike mode: baseline learns slowly and ignores large spikes");
  Serial.println("CH2 / CH3 cooldown enabled: CH2 = 2500 ms, CH3 = 6000 ms");
  Serial.println("USB Type must be MIDI or Serial + MIDI in Teensy Tools menu");

  delay(500);
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  currentMillis = millis();

  while (usbMIDI.read()) { }

  updateThresholdFromPot();
  updateBankFromPot();

  if (sampleIndex >= samplesize) {
    analyzeSample();
  }

  // This replaces the old single global noteOffAt system.
  // It checks every voice slot and turns notes off individually.
  checkPolyNotes();
}
