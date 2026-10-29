// The speaker: short, gentle tones for the panel's events.
//
// This repo's own, after claude-gadget's sound.cpp: tones synthesised on the fly
// (a sine with a little second harmonic, fast attack, exponential decay), so
// there are no audio files, played on a task of their own so nothing waits on
// them. The board has an I2S amplifier and no codec, so there is nothing to set
// up over I2C.
//
// Which readings call for which sound is src/sound_events.cpp.

#pragma once

#include "sound_events.h"

void sound_begin();

// Plays `cue` if its switch on the Sound page is on and the panel is not being
// quiet (sound_set_quiet). Never blocks; a tap that arrives while something is
// already playing is dropped rather than queued.
void sound_play(SoundCue cue);

// Plays `cue` whatever the switches and quiet say, at the set volume: what the
// Sound page does when you turn a sound on or move the volume, so you hear it.
void sound_preview(SoundCue cue);

// Quiet: the overnight hours, an empty room or an install. main.cpp decides and
// sets this every loop.
void sound_set_quiet(bool quiet);
