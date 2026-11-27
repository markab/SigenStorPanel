// Screen, panel: grid events. Each live event in a card across the top (two can
// overlap: an Axle export inside a Saving Session), opened out with its figures.
// Booked events come next as rows, soonest first, saying how long until they
// start: they get a card when they go live. Then the finished ones, newest first,
// as many as fit. With nothing live or booked, the latest finished one takes the
// card, so the screen leads with a result rather than a list.
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
// Two cards are a little shorter, so two rows still fit under them.
constexpr lv_coord_t CARD_H_TWO = 150;
constexpr lv_coord_t CARD_GAP = 14;
constexpr lv_coord_t CARD_W = PUCK_LCD_WIDTH - 2 * PAD_X;
// Cards for at most two live events at once; a third, which has never been
// seen, goes at the top of the rows.
constexpr int CARDS = 2;
constexpr lv_coord_t ROWS_GAP = 18;  // from the last card to the first row
constexpr lv_coord_t ROW_H = 46;
// The rows stop here, clear of the page dots: six under one card.
constexpr lv_coord_t ROWS_BOTTOM = CARD_Y + CARD_H + ROWS_GAP + 6 * ROW_H;
// With no card at all, the rows start at the top and ten fit.
constexpr int ROWS = static_cast<int>((ROWS_BOTTOM - CARD_Y) / ROW_H);
// The card's figures: the kind's own (events_figures, at most two) and the
// battery's run.
constexpr int CARD_FIGURES = 3;

struct Figure {
  lv_obj_t* box;
  lv_obj_t* caption;
  lv_obj_t* value;
};

struct Card {
  lv_obj_t* box;
  lv_obj_t* pill;
  lv_obj_t* pill_text;
  lv_obj_t* when;
  lv_obj_t* status;
  Figure figures[CARD_FIGURES];
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
Card s_cards[CARDS] = {};
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

void build_card(Card& card) {
  card.box = plain(s_root);
  lv_obj_set_pos(card.box, PAD_X, CARD_Y);
  lv_obj_set_size(card.box, CARD_W, CARD_H);
  lv_obj_set_style_radius(card.box, 18, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card.box, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card.box, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(card.box, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(card.box, 22, LV_PART_MAIN);

  card.pill = plain(card.box);
  lv_obj_set_size(card.pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(card.pill, 10, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card.pill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(card.pill, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(card.pill, 5, LV_PART_MAIN);
  lv_obj_align(card.pill, LV_ALIGN_TOP_LEFT, 0, 0);
  card.pill_text = label(card.pill, &puck_font_18, PUCK_COLOUR_BG);

  card.when = label(card.box, &puck_font_24, PUCK_COLOUR_TEXT);
  lv_obj_align_to(card.when, card.pill, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
  card.status = label(card.box, &puck_font_20, PUCK_COLOUR_MUTED);
  lv_obj_align(card.status, LV_ALIGN_TOP_RIGHT, 0, 4);

  for (int i = 0; i < CARD_FIGURES; ++i) {
    Figure& figure = card.figures[i];
    figure.box = plain(card.box);
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
    lv_obj_set_pos(row.box, PAD_X, CARD_Y + CARD_H + ROWS_GAP + i * ROW_H);
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

void show_card(Card& card, const GridEvent& event, uint32_t now, int32_t tz_offset_min) {
  const uint32_t colour = kind_colour(event.kind);
  lv_obj_set_style_bg_color(card.box, lv_color_mix(lv_color_hex(colour), lv_color_black(), 36),
                            LV_PART_MAIN);
  lv_obj_set_style_border_color(card.box, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_set_style_bg_color(card.pill, lv_color_hex(colour), LV_PART_MAIN);
  set_text(card.pill_text, events_title(event));

  char text[48];
  events_format_when(event, now, tz_offset_min, text, sizeof(text));
  set_text(card.when, text);
  lv_obj_align_to(card.when, card.pill, LV_ALIGN_OUT_RIGHT_MID, 16, 0);

  events_format_status(event, now, text, sizeof(text));
  set_text(card.status, text);
  const EventState state = events_state_at(event, now);
  lv_obj_set_style_text_color(
      card.status, lv_color_hex(state == EventState::Live ? PUCK_COLOUR_BATTERY : PUCK_COLOUR_MUTED),
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
    Figure& figure = card.figures[i];
    if (used) {
      set_text(figure.caption, figures[i].caption);
      set_text(figure.value, figures[i].value);
    }
    if (used == lv_obj_has_flag(figure.box, LV_OBJ_FLAG_HIDDEN)) {
      used ? lv_obj_clear_flag(figure.box, LV_OBJ_FLAG_HIDDEN)
           : lv_obj_add_flag(figure.box, LV_OBJ_FLAG_HIDDEN);
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
  // "exported 4.2 kWh"); a session sat out says so. An event not over yet leads
  // with how it stands: "In 44 min", or "Live · 12 min left" for a live event
  // there was no card for.
  char line[96] = {};
  const EventState state = events_state_at(event, now);
  if (state == EventState::Upcoming || state == EventState::Live ||
      state == EventState::Deferred) {
    events_format_status(event, now, line, sizeof(line));
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
      lv_color_hex(state == EventState::Deferred ? PUCK_COLOUR_MUTED
                   : state == EventState::Live   ? PUCK_COLOUR_BATTERY
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

  for (Card& card : s_cards) {
    build_card(card);
  }
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
  // The cards: every live event the server sent (two can overlap), else the one
  // event a small screen would feature: the next booked, or the latest finished.
  const GridEvent* carded[EVENTS_MAX_LIVE];
  int live = 0;
  if (events.valid) {
    for (uint8_t i = 0; i < events.live_count && i < EVENTS_MAX_LIVE; ++i) {
      if (events_renderable(events.live[i])) {
        carded[live++] = &events.live[i];
      }
    }
  }
  // Booked events, soonest first: rows, until they go live.
  const GridEvent* booked[EVENTS_MAX_UPCOMING];
  int booked_count = 0;
  if (events.valid) {
    for (uint8_t i = 0; i < events.upcoming_count && i < EVENTS_MAX_UPCOMING; ++i) {
      if (events_renderable(events.upcoming[i])) {
        booked[booked_count++] = &events.upcoming[i];
      }
    }
  }
  // Nothing live or booked: the latest finished event takes the card.
  const GridEvent* featured = nullptr;
  if (live == 0 && booked_count == 0) {
    featured = events.valid ? events_featured(events) : nullptr;
    if (featured != nullptr) {
      carded[live++] = featured;
    }
  }
  if (live == 0 && booked_count == 0) {
    for (Card& card : s_cards) {
      lv_obj_add_flag(card.box, LV_OBJ_FLAG_HIDDEN);
    }
    for (Row& row : s_rows) {
      lv_obj_add_flag(row.box, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

  const int cards = live < CARDS ? live : CARDS;
  const lv_coord_t card_h = cards > 1 ? CARD_H_TWO : CARD_H;
  for (int i = 0; i < CARDS; ++i) {
    Card& card = s_cards[i];
    if (i < cards) {
      lv_obj_set_pos(card.box, PAD_X, CARD_Y + i * (card_h + CARD_GAP));
      lv_obj_set_height(card.box, card_h);
      lv_obj_clear_flag(card.box, LV_OBJ_FLAG_HIDDEN);
      show_card(card, *carded[i], now, tz_offset_min);
    } else {
      lv_obj_add_flag(card.box, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // The rows: a live event there was no card for, the booked ones, then
  // everything finished, newest first, but the one already in a card.
  const GridEvent* listed[EVENTS_MAX_LIVE + EVENTS_MAX_UPCOMING + EVENTS_MAX_PAST];
  int count = 0;
  for (int i = cards; i < live; ++i) {
    listed[count++] = carded[i];
  }
  for (int i = 0; i < booked_count; ++i) {
    listed[count++] = booked[i];
  }
  for (uint8_t i = 0; i < events.past_count && i < EVENTS_MAX_PAST; ++i) {
    const GridEvent& event = events.past[i];
    if (&event != featured && events_renderable(event)) {
      listed[count++] = &event;
    }
  }
  const lv_coord_t rows_y =
      cards > 0 ? CARD_Y + cards * card_h + (cards - 1) * CARD_GAP + ROWS_GAP : CARD_Y;
  const int fit = static_cast<int>((ROWS_BOTTOM - rows_y) / ROW_H);
  for (int i = 0; i < ROWS; ++i) {
    if (i < count && i < fit) {
      lv_obj_set_y(s_rows[i].box, rows_y + i * ROW_H);
      show_row(s_rows[i], *listed[i], now, tz_offset_min);
    } else {
      lv_obj_add_flag(s_rows[i].box, LV_OBJ_FLAG_HIDDEN);
    }
  }
}
