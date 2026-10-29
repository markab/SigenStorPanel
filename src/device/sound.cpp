#include "sound.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>

#include "board_config.h"
#include "settings.h"

namespace {

struct Note {
  uint16_t freq;   // Hz, 0 for a rest
  uint16_t ms;     // length, decay included
  uint16_t gap_ms; // silence after
};

enum : uint16_t {
  N_C5 = 523,
  N_E5 = 659,
  N_G5 = 784,
  N_C6 = 1047,
  N_E6 = 1319,
};

// All in C major and all short, so none of them is alarming. Up for good news,
// down for a warning or an ending.
constexpr Note BOOT[] = {{N_C5, 140, 10}, {N_E5, 140, 10}, {N_G5, 140, 10}, {N_C6, 420, 0}};
constexpr Note TAP[] = {{N_E6, 45, 0}};
constexpr Note BATTERY_FULL[] = {{N_G5, 160, 20}, {N_C6, 160, 20}, {N_E6, 520, 0}};
constexpr Note BATTERY_RESERVE[] = {{N_G5, 200, 30}, {N_E5, 200, 30}, {N_C5, 600, 0}};
constexpr Note EV_START[] = {{N_C5, 150, 20}, {N_G5, 380, 0}};
constexpr Note EV_STOP[] = {{N_G5, 150, 20}, {N_C5, 380, 0}};
constexpr Note PREVIEW[] = {{N_E6, 120, 0}};

// A tap is a tick under the other sounds, not a note of its own.
constexpr float TAP_LEVEL = 0.35f;
// Full scale at 100% volume would clip the sum of two partials.
constexpr float PEAK = 0.7f;
// Lets the amp settle before the first note, and the DMA drain after the last,
// so neither end pops.
constexpr uint32_t SETTLE_MS = 30;
constexpr uint32_t DRAIN_MS = 40;
constexpr size_t FRAMES_PER_WRITE = 256;

I2SClass s_i2s;
QueueHandle_t s_queue = nullptr;
volatile bool s_quiet = false;
bool s_amp_on = false;

void amp(bool on) {
  const bool high = on ? PUCK_SPEAKER_AMP_ON_LEVEL : !PUCK_SPEAKER_AMP_ON_LEVEL;
  digitalWrite(PUCK_SPEAKER_AMP_EN, high ? HIGH : LOW);
  s_amp_on = on;
}

void write_silence(uint32_t ms) {
  static int16_t zeros[FRAMES_PER_WRITE * 2] = {};
  uint32_t frames = PUCK_SPEAKER_RATE * ms / 1000;
  while (frames > 0) {
    const uint32_t n = frames < FRAMES_PER_WRITE ? frames : FRAMES_PER_WRITE;
    s_i2s.write(reinterpret_cast<uint8_t*>(zeros), n * 4);
    frames -= n;
  }
}

void play_note(const Note& note, float level) {
  static int16_t frames[FRAMES_PER_WRITE * 2];
  const uint32_t total = PUCK_SPEAKER_RATE * note.ms / 1000;
  const uint32_t attack = PUCK_SPEAKER_RATE * 6 / 1000;
  const float step = 2.0f * static_cast<float>(M_PI) * note.freq / PUCK_SPEAKER_RATE;
  const float decay = expf(-5.0f / static_cast<float>(total));  // about -43 dB by the end
  float envelope = 1.0f;
  uint32_t i = 0;
  while (i < total) {
    const uint32_t chunk = total - i < FRAMES_PER_WRITE ? total - i : FRAMES_PER_WRITE;
    for (uint32_t k = 0; k < chunk; ++k, ++i) {
      const float shape = i < attack ? static_cast<float>(i) / attack : envelope;
      if (i >= attack) {
        envelope *= decay;
      }
      const float wave =
          note.freq != 0 ? (sinf(step * i) + 0.3f * sinf(2.0f * step * i)) / 1.3f : 0.0f;
      const int16_t sample = static_cast<int16_t>(wave * shape * level * 32767.0f);
      frames[2 * k] = sample;      // both speakers
      frames[2 * k + 1] = sample;
    }
    s_i2s.write(reinterpret_cast<uint8_t*>(frames), chunk * 4);
  }
  if (note.gap_ms != 0) {
    write_silence(note.gap_ms);
  }
}

template <size_t N>
void play_notes(const Note (&notes)[N], float level) {
  for (const Note& note : notes) {
    play_note(note, level);
  }
}

void play(SoundCue cue) {
  // Perceptual: the square of the 0-100 setting.
  const float volume = settings_get().sound_volume / 100.0f;
  const float level = volume * volume * PEAK;
  switch (cue) {
    case SoundCue::Boot:
      play_notes(BOOT, level);
      break;
    case SoundCue::Tap:
      play_notes(TAP, level * TAP_LEVEL);
      break;
    case SoundCue::BatteryFull:
      play_notes(BATTERY_FULL, level);
      break;
    case SoundCue::BatteryReserve:
      play_notes(BATTERY_RESERVE, level);
      break;
    case SoundCue::EvStart:
      play_notes(EV_START, level);
      break;
    case SoundCue::EvStop:
      play_notes(EV_STOP, level);
      break;
    case SoundCue::Preview:
      play_notes(PREVIEW, level);
      break;
  }
}

void sound_task(void* /*arg*/) {
  for (;;) {
    SoundCue cue;
    // Held on for a while after a sound, so a run of taps is one switch-on.
    const TickType_t wait = s_amp_on ? pdMS_TO_TICKS(PUCK_SPEAKER_HOLD_MS) : portMAX_DELAY;
    if (xQueueReceive(s_queue, &cue, wait) != pdTRUE) {
      amp(false);
      continue;
    }
    if (!s_amp_on) {
      amp(true);
      write_silence(SETTLE_MS);
    }
    play(cue);
    write_silence(DRAIN_MS);
  }
}

void enqueue(SoundCue cue) {
  if (s_queue == nullptr || settings_get().sound_volume == 0) {
    return;
  }
  if (cue == SoundCue::Tap && uxQueueMessagesWaiting(s_queue) > 0) {
    return;
  }
  xQueueSend(s_queue, &cue, 0);
}

}  // namespace

void sound_begin() {
  pinMode(PUCK_SPEAKER_AMP_EN, OUTPUT);
  amp(false);
  s_i2s.setPins(PUCK_SPEAKER_BCLK, PUCK_SPEAKER_LRCLK, PUCK_SPEAKER_DOUT);
  if (!s_i2s.begin(I2S_MODE_STD, PUCK_SPEAKER_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[sound] I2S would not start; the panel stays silent");
    return;
  }
  s_queue = xQueueCreate(4, sizeof(SoundCue));
  // Above the LVGL loop on its core, so a busy frame cannot starve the DMA and
  // break a note up; it spends nearly all its time blocked on the queue or on
  // I2S anyway.
  xTaskCreatePinnedToCore(sound_task, "sound", 4096, nullptr, 4, nullptr, 1);
  Serial.println("[sound] speaker ready");
}

void sound_play(SoundCue cue) {
  if (s_quiet || (settings_get().sound_mask & sound_switch_bit(cue)) == 0) {
    return;
  }
  enqueue(cue);
}

void sound_preview(SoundCue cue) {
  enqueue(cue);
}

void sound_set_quiet(bool quiet) {
  s_quiet = quiet;
}
