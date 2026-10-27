#include "presence_parse.h"

#include <stdlib.h>
#include <string.h>

namespace {

constexpr uint8_t FRAME_HEADER[4] = {0xF4, 0xF3, 0xF2, 0xF1};
constexpr uint8_t FRAME_TAIL[4] = {0xF8, 0xF7, 0xF6, 0xF5};

}  // namespace

bool PresenceParser::finish_line(PresenceReading* out) {
  line_[line_length_] = '\0';
  const bool overflowed = line_overflow_;
  line_length_ = 0;
  line_overflow_ = false;
  if (overflowed) {
    return false;
  }
  if (strcmp(line_, "ON") == 0) {
    present_ = true;
  } else if (strcmp(line_, "OFF") == 0) {
    present_ = false;
    has_distance_ = false;
  } else if (strncmp(line_, "Range ", 6) == 0) {
    char* end = nullptr;
    const long value = strtol(line_ + 6, &end, 10);
    if (end == line_ + 6 || value < 0 || value > 0xFFFF) {
      return false;
    }
    present_ = true;
    has_distance_ = true;
    distance_ = static_cast<uint16_t>(value);
  } else {
    return false;  // something else entirely: ignore it rather than guess
  }
  out->present = present_;
  out->has_distance = has_distance_;
  out->distance = distance_;
  return true;
}

bool PresenceParser::feed(uint8_t byte, PresenceReading* out) {
  // Report frames first: their bytes are never mistaken for text, because a
  // frame in progress takes every byte until its tail.
  switch (frame_) {
    case Frame::Hunting:
      if (byte == FRAME_HEADER[header_matched_]) {
        if (++header_matched_ == sizeof(FRAME_HEADER)) {
          header_matched_ = 0;
          length_received_ = 0;
          frame_ = Frame::Length;
          line_length_ = 0;  // whatever text came before is not a line
        }
        return false;
      }
      header_matched_ = byte == FRAME_HEADER[0] ? 1 : 0;
      break;
    case Frame::Length:
      length_bytes_[length_received_++] = byte;
      if (length_received_ == 2) {
        // The body, then the four-byte tail.
        frame_length_ = static_cast<size_t>(length_bytes_[0] | (length_bytes_[1] << 8)) +
                        sizeof(FRAME_TAIL);
        frame_received_ = 0;
        frame_ = frame_length_ <= sizeof(body_) && frame_length_ >= 3 + sizeof(FRAME_TAIL)
                     ? Frame::Body
                     : Frame::Hunting;
      }
      return false;
    case Frame::Body:
      body_[frame_received_++] = byte;
      if (frame_received_ < frame_length_) {
        return false;
      }
      frame_ = Frame::Hunting;
      if (memcmp(body_ + frame_length_ - sizeof(FRAME_TAIL), FRAME_TAIL, sizeof(FRAME_TAIL)) != 0) {
        return false;
      }
      present_ = body_[0] != 0;
      has_distance_ = present_;
      distance_ = static_cast<uint16_t>(body_[1] | (body_[2] << 8));
      out->present = present_;
      out->has_distance = has_distance_;
      out->distance = distance_;
      return true;
  }

  // Normal mode text.
  if (byte == '\n' || byte == '\r') {
    return line_length_ > 0 ? finish_line(out) : false;
  }
  if (byte < 0x20 || byte > 0x7E) {
    line_length_ = 0;  // binary noise: start the line again
    return false;
  }
  if (line_length_ + 1 < sizeof(line_)) {
    line_[line_length_++] = static_cast<char>(byte);
  } else {
    line_overflow_ = true;
  }
  return false;
}
