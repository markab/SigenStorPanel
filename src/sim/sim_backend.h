// Desktop stand-in for src/display.cpp and src/touch.cpp: an SDL window the
// same size as the panel, plus the mouse as LVGL's pointer device.
//
// Only the sim environment compiles this. Everything above it — screens, the
// snapshot model — is the same code that runs on the board.

#pragma once

#include <lvgl.h>

// Opens the window and registers LVGL's display and input drivers. lv_init()
// must have been called first. False if SDL or the draw buffer failed.
bool sim_backend_begin();

// Pumps SDL's event queue. Returns false once the window has been closed, which
// is the sim's cue to exit.
bool sim_backend_pump();

// Next queued key press as its ASCII code (so ' ' and 'n' work as literals), or
// 0 when nothing is waiting. Lets the harness drive the sim from the keyboard
// without leaking SDL's headers upwards.
int sim_backend_take_key();

void sim_backend_set_title(const char* title);

// Sleeps, so the sim's main loop does not spin a core flat out.
void sim_backend_delay(uint32_t milliseconds);

// Writes the current frame to a BMP. Lets a screen be checked against every
// fixture in one non-interactive run, which is far quicker than clicking
// through them and works with no one watching the window.
bool sim_backend_save_bmp(const char* path);

// Puts the pointer somewhere, pressed or not, as if the mouse had. Lets the
// screenshot pass drive a real swipe through LVGL's input handling rather than
// calling the function a swipe would end up in.
void sim_backend_set_pointer(int x, int y, bool down);

void sim_backend_end();
