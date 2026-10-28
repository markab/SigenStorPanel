// Tidying a kiosk code before it is saved.
//
// From v0.38, SigenStor Display's Admin page shows a display unit its server and
// a code: 20 characters of lowercase Crockford base32 (no i, l, o or u) in dashed
// groups of four, k7mq-4xrp-9tbz-h2wd-c8nf. The server matches a code however it
// was typed: any case, dashes and spaces ignored, o, i and l read as 0, 1 and 1.
// But the code travels in a cookie, where a space does not belong, so the panel
// saves the one spelling the server hashes: lowercase, no dashes, no look-alikes.
//
// Tokens issued before v0.38 are 43 characters of base64url and are matched
// exactly as issued, case and all. One of those is saved as given, less any
// whitespace.
//
// This repo's own, beside the verbatim enrol_url.h (UPSTREAM.md). Free of Arduino
// headers for the same reason: the sim compiles it and the selftest checks it.

#pragma once

#include <stddef.h>

// Room for either form and its terminator.
static constexpr size_t KIOSK_CODE_SIZE = 44;

// Writes the tidied code to `out`. Returns false, with `out` empty, when `typed`
// is neither a code nor an older token, or `out` is too small for it.
bool kiosk_code_tidy(const char* typed, char* out, size_t size);
