// Screen, panel: grid events. The live event, else the next booked one, else the
// latest finished one, in a card across the top; the events before it in rows
// underneath, newest first, as many as fit. A new event pushes the oldest row off
// the bottom.
//
// The wording comes from events.h's helpers, so the panel describes an event in
// the same words as both Puck boards; this file only lays them out. The round and
// 2.41" layouts are SigenStorPuck's src/ui/ and src/ui_landscape/screen_events.cpp.

#include "screen_events.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "theme.h"

namespace {

constexpr lv_coord_t PAD_X = 56;
constexpr lv_coord_t CARD_Y = 58;
constexpr lv_coord_t CARD_H = 170;
constexpr lv_coord_t CARD_W = PUCK_LCD_WIDTH - 2 * PAD_X;
constexpr lv_coord_t ROWS_Y = 246;
constexpr lv_coord_t ROW_H = 46;  // six clear of the page dots
constexpr int ROWS = 6;
// The card's figures: the kind's own (events_figures, at most two) and the
// battery's run.
constexpr int CARD_FIGURES = 3;

struct Figure {
  lv_obj_t* box;
  lv_obj_t* caption;
  lv_obj_t* value;
};

struct Row {
  lv_obj_t* box;
  lv_obj_t* dot;
  lv_obj_t* name;
  lv_obj_t* when;
  lv_obj_t* figures;
};

lv_obj_t* s_root = nullptr;
lv_obj_t* s_empty = nullptr;
lv_obj_t* s_card = nullptr;
lv_obj_t* s_pill = nullptr;
lv_obj_t* s_pill_text = nullptr;
lv_obj_t* s_card_when = nullptr;
lv_obj_t* s_card_status = nullptr;
Figure s_figures[CARD_FIGURES] = {};
Row s_rows[ROWS] = {};

uint32_t kind_colour(EventKind kind) {
  switch (kind) {
    case EventKind::Saving:
      return PUCK_COLOUR_WARN;
    case EventKind::HappyHour:
    case EventKind::FreeImport:
      return PUCK_COLOUR_BATTERY;
    case EventKind::Axle:
      return PUCK_COLOUR_GRID;
    default:
      return PUCK_COLOUR_MUTED;
  }
}

// The row's name, in sentence case beside the card's capitals.
const char* row_name(const GridEvent& event) {
  switch (event.kind) {
    case EventKind::Saving:
      return "Saving session";
    case EventKind::HappyHour:
      return "Happy Hour";
    case EventKind::FreeImport:
      return "Free import";
    case EventKind::Axle:
      return event.direction == EventDirection::Import ? "Axle import" : "Axle export";
    default:
      return "";
  }
}

// Labels are rewritten only when their text changes: the screen is redrawn every
// 15 s for the countdown, and an unchanged label should not invalidate.
void set_text(lv_obj_t* label, const char* text) {
  if (strcmp(lv_label_get_text(label), text) != 0) {
    lv_label_set_text(label, text);
  }
}

lv_obj_t* plain(lv_obj_t* parent) {
  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour) {
  lv_obj_t* obj = lv_label_create(parent);
  lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(obj, lv_color_hex(colour), LV_PART_MAIN);
  lv_label_set_text(obj, "");
  return obj;
}

void build_card() {
  s_card = plain(s_root);
  lv_obj_set_pos(s_card, PAD_X, CARD_Y);
  lv_obj_set_size(s_card, CARD_W, CARD_H);
  lv_obj_set_style_radius(s_card, 18, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_card, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(s_card, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(s_card, 22, LV_PART_MAIN);

  s_pill = plain(s_card);
  lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(s_pill, 10, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(s_pill, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(s_pill, 5, LV_PART_MAIN);
  lv_obj_align(s_pill, LV_ALIGN_TOP_LEFT, 0, 0);
  s_pill_text = label(s_pill, &puck_font_18, PUCK_COLOUR_BG);

  s_card_when = label(s_card, &puck_font_24, PUCK_COLOUR_TEXT);
  lv_obj_align_to(s_card_when, s_pill, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
  s_card_status = label(s_card, &puck_font_20, PUCK_COLOUR_MUTED);
  lv_obj_align(s_card_status, LV_ALIGN_TOP_RIGHT, 0, 4);

  for (int i = 0; i < CARD_FIGURES; ++i) {
    Figure& figure = s_figures[i];
    figure.box = plain(s_card);
    lv_obj_set_size(figure.box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(figure.box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(figure.box, 2, LV_PART_MAIN);
    figure.caption = label(figure.box, &puck_font_14, PUCK_COLOUR_MUTED);
    figure.value = label(figure.box, &puck_font_48, PUCK_COLOUR_TEXT);
    lv_obj_align(figure.box, LV_ALIGN_BOTTOM_LEFT, i * 280, 0);
  }
}

void build_rows() {
  for (int i = 0; i < ROWS; ++i) {
    Row& row = s_rows[i];
    row.box = plain(s_root);
    lv_obj_set_pos(row.box, PAD_X, ROWS_Y + i * ROW_H);
    lv_obj_set_size(row.box, CARD_W, ROW_H);
    lv_obj_set_style_border_side(row.box, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row.box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row.box, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);

    row.dot = plain(row.box);
    lv_obj_set_size(row.dot, 10, 10);
    lv_obj_set_style_radius(row.dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row.dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(row.dot, LV_ALIGN_LEFT_MID, 0, 0);

    row.name = label(row.box, &puck_font_24, PUCK_COLOUR_TEXT);
    lv_obj_align(row.name, LV_ALIGN_LEFT_MID, 26, 0);
    row.when = label(row.box, &puck_font_24, PUCK_COLOUR_MUTED);
    lv_obj_align(row.when, LV_ALIGN_LEFT_MID, 250, 0);
    row.figures = label(row.box, &puck_font_24, PUCK_COLOUR_TEXT);
    lv_obj_align(row.figures, LV_ALIGN_RIGHT_MID, 0, 0);
  }
}

void show_card(const GridEvent& event, uint32_t now, int32_t tz_offset_min) {
  const uint32_t colour = kind_colour(event.kind);
  lv_obj_set_style_bg_color(s_card, lv_color_mix(lv_color_hex(colour), lv_color_black(), 36),
                            LV_PART_MAIN);
  lv_obj_set_style_border_color(s_card, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_pill, lv_color_hex(colour), LV_PART_MAIN);
  set_text(s_pill_text, events_title(event));

  char text[48];
  events_format_when(event, now, tz_offset_min, text, sizeof(text));
  set_text(s_card_when, text);
  lv_obj_align_to(s_card_when, s_pill, LV_ALIGN_OUT_RIGHT_MID, 16, 0);

  events_format_status(event, now, text, sizeof(text));
  set_text(s_card_status, text);
  const EventState state = events_state_at(event, now);
  lv_obj_set_style_text_color(
      s_card_status, lv_color_hex(state == EventState::Live ? PUCK_COLOUR_BATTERY : PUCK_COLOUR_MUTED),
      LV_PART_MAIN);

  EventFigure figures[CARD_FIGURES];
  size_t count = events_figures(event, figures, 2);
  // The battery's run across the event, once it has started: the panel has the
  // room the Pucks do not.
  if (event.soc_start.known && count < CARD_FIGURES) {
    EventFigure& battery = figures[count++];
    if (event.soc_end.known) {
      snprintf(battery.value, sizeof(battery.value), "%.0f%% to %.0f%%", event.soc_start.value,
               event.soc_end.value);
    } else {
      snprintf(battery.value, sizeof(battery.value), "%.0f%%", event.soc_start.value);
    }
    snprintf(battery.caption, sizeof(battery.caption), "BATTERY");
  }
  for (int i = 0; i < CARD_FIGURES; ++i) {
    const bool used = static_cast<size_t>(i) < count;
    if (used) {
      set_text(s_figures[i].caption, figures[i].caption);
      set_text(s_figures[i].value, figures[i].value);
    }
    if (used == lv_obj_has_flag(s_figures[i].box, LV_OBJ_FLAG_HIDDEN)) {
      used ? lv_obj_clear_flag(s_figures[i].box, LV_OBJ_FLAG_HIDDEN)
           : lv_obj_add_flag(s_figures[i].box, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void show_row(Row& row, const GridEvent& event, uint32_t now, int32_t tz_offset_min) {
  lv_obj_set_style_bg_color(row.dot, lv_color_hex(kind_colour(event.kind)), LV_PART_MAIN);
  set_text(row.name, row_name(event));

  char text[64];
  events_format_when(event, now, tz_offset_min, text, sizeof(text));
  set_text(row.when, text);

  // The figures on one line, " · " between them, the first named ("cut 3.8 kWh",
  // "exported 4.2 kWh"); a session sat out says so.
  char line[64] = {};
  if (events_state_at(event, now) == EventState::Deferred) {
    snprintf(line, sizeof(line), "Deferred");
  }
  EventFigure figures[2];
  const size_t count = events_figures(event, figures, 2);
  for (size_t i = 0; i < count; ++i) {
    const size_t used = strlen(line);
    char caption[sizeof(figures[i].caption)] = {};
    if (i == 0) {
      for (size_t c = 0; figures[i].caption[c] != '\0' && c + 1 < sizeof(caption); ++c) {
        caption[c] = static_cast<char>(tolower(static_cast<unsigned char>(figures[i].caption[c])));
      }
    }
    snprintf(line + used, sizeof(line) - used, "%s%s%s%s", used > 0 ? " \xC2\xB7 " : "",
             caption, caption[0] != '\0' ? " " : "", figures[i].value);
  }
  set_text(row.figures, line);
  lv_obj_set_style_text_color(
      row.figures,
      lv_color_hex(events_state_at(event, now) == EventState::Deferred ? PUCK_COLOUR_MUTED
                                                                        : PUCK_COLOUR_TEXT),
      LV_PART_MAIN);
  lv_obj_clear_flag(row.box, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

lv_obj_t* screen_events_create(lv_obj_t* parent) {
  s_root = plain(parent);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(s_root);

  lv_obj_t* caption = label(s_root, &puck_font_18, PUCK_COLOUR_MUTED);
  lv_label_set_text(caption, "EVENTS");
  lv_obj_set_pos(caption, PAD_X, 26);

  build_card();
  build_rows();

  s_empty = label(s_root, &puck_font_28, PUCK_COLOUR_MUTED);
  lv_label_set_text(s_empty, "No events");
  lv_obj_center(s_empty);

  const EventList none;
  screen_events_update(none, 0, 0);
  return s_root;
}

void screen_events_update(const EventList& events, uint32_t now, int32_t tz_offset_min) {
  if (s_root == nullptr) {
    return;
  }
  const GridEvent* featured = events.valid ? events_featured(events) : nullptr;
  if (featured == nullptr) {
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    for (Row& row : s_rows) {
      lv_obj_add_flag(row.box, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
  show_card(*featured, now, tz_offset_min);

  // Everything finished, newest first, but the one already in the card.
  const GridEvent* listed[EVENTS_MAX_PAST];
  int count = 0;
  for (uint8_t i = 0; i < events.past_count; ++i) {
    const GridEvent& event = events.past[i];
    if (&event != featured && events_renderable(event)) {
      listed[count++] = &event;
    }
  }
  for (int i = 0; i < ROWS; ++i) {
    if (i < count) {
      show_row(s_rows[i], *listed[i], now, tz_offset_min);
    } else {
      lv_obj_add_flag(s_rows[i].box, LV_OBJ_FLAG_HIDDEN);
    }
  }
}
