// Screen, panel only: an EnViOT Display server's sensors.
//
// Up to four cards in a 2x2 grid, one or two side by side. Each card: the
// sensor's colour and name, how long since it last reported, a temperature gauge
// with the server's bands round it and the last 24 hours as a thin inner arc,
// the 24-hour range and two more readings in rows, and the battery as forty
// segments. A sensor with nothing for 45 minutes is greyed out; one with an
// alert gets a red outline and the alert's title.
//
// The panel renders, it does not judge: the bands, the band each reading is in,
// staleness and alerts are all the server's (enviot.h).

#include "screen_environment.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "theme.h"

namespace {

constexpr lv_coord_t PAD_X = 56;
constexpr lv_coord_t GRID_Y = 58;
constexpr lv_coord_t GRID_W = PUCK_LCD_WIDTH - 2 * PAD_X;
constexpr lv_coord_t GRID_H = 470;  // down to just above the page dots
constexpr lv_coord_t GAP = 16;
constexpr lv_coord_t CARD_W = (GRID_W - GAP) / 2;
constexpr lv_coord_t CARD_H = (GRID_H - GAP) / 2;

constexpr uint32_t COLOUR_CARD = 0x121214;
constexpr uint32_t COLOUR_LINE = 0x2C2C2E;
constexpr uint32_t COLOUR_RANGE = 0xAEAEB2;

// The gauge: a 270-degree arc open at the bottom, centred left of the rows.
constexpr lv_coord_t GAUGE_X = 100;
constexpr lv_coord_t GAUGE_Y = 116;
constexpr lv_coord_t GAUGE_R = 52;       // the centre line of the value arc
constexpr lv_coord_t GAUGE_W = 11;
constexpr lv_coord_t BANDS_R = 64;
constexpr lv_coord_t BANDS_W = 3;
constexpr lv_coord_t RANGE_R = 40;
constexpr lv_coord_t RANGE_W = 4;
constexpr int GAUGE_START = 135;  // degrees clockwise from 3 o'clock
constexpr int GAUGE_SWEEP = 270;

constexpr lv_coord_t ROWS_X = 200;
constexpr lv_coord_t ROWS_Y = 68;
constexpr lv_coord_t ROW_PITCH = 38;
constexpr int ROWS = 1 + ENV_MAX_ROWS;  // "24 h": the range, then the server's
constexpr lv_coord_t UNIT_W = 36;
constexpr lv_coord_t VALUE_W = 130;

constexpr int BATTERY_SEGMENTS = 40;
constexpr lv_coord_t BATTERY_BAR_X = 110;
constexpr lv_coord_t BATTERY_BAR_H = 16;
constexpr lv_coord_t BATTERY_VALUE_W = 78;

struct Row {
  lv_obj_t* name;
  lv_obj_t* value;
  lv_obj_t* unit;
};

struct Card {
  lv_obj_t* box;
  lv_obj_t* swatch;
  lv_obj_t* name;
  lv_obj_t* when;
  lv_obj_t* gauge;  // the bands, the track, the value and the range, drawn as one
  lv_obj_t* reading;
  lv_obj_t* reading_unit;
  lv_obj_t* scale_min;
  lv_obj_t* scale_max;
  Row rows[ROWS];
  lv_obj_t* battery_bar;
  lv_obj_t* battery_value;
  // What the gauge's and battery bar's draw callbacks paint from.
  EnvScale scale;
  MaybeFloat temperature;
  uint32_t temperature_colour;
  MaybeFloat day_min;
  MaybeFloat day_max;
  MaybeFloat battery;
  EnvScale battery_scale;
  bool grey;
};

lv_obj_t* s_root = nullptr;
lv_obj_t* s_message = nullptr;
lv_obj_t* s_note = nullptr;
Card s_cards[ENV_MAX_DEVICES] = {};

uint32_t band_colour(EnvBand band) {
  switch (band) {
    case EnvBand::Cold:
      return PUCK_COLOUR_GRID;
    case EnvBand::Ok:
      return PUCK_COLOUR_BATTERY;
    case EnvBand::Hot:
    case EnvBand::Low:
      return PUCK_COLOUR_ALARM;
    case EnvBand::Fair:
      return PUCK_COLOUR_WARN;
    default:
      return PUCK_COLOUR_MUTED;
  }
}

// Labels are rewritten only when their text changes: the screen is redrawn every
// 15 s for the "min ago", and an unchanged label should not invalidate.
void set_text(lv_obj_t* label, const char* text) {
  if (strcmp(lv_label_get_text(label), text) != 0) {
    lv_label_set_text(label, text);
  }
}

void set_colour(lv_obj_t* obj, uint32_t colour, lv_part_t part = LV_PART_MAIN) {
  lv_obj_set_style_text_color(obj, lv_color_hex(colour), part);
}

void set_hidden(lv_obj_t* obj, bool hidden) {
  if (hidden) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
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
  set_colour(obj, colour);
  lv_label_set_text(obj, "");
  return obj;
}

// The angle of a value on the gauge, clamped to its ends.
int gauge_angle(const EnvScale& scale, float value) {
  float t = (value - scale.min) / (scale.max - scale.min);
  t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
  return GAUGE_START + static_cast<int>(t * GAUGE_SWEEP + 0.5f);
}

// One arc on the gauge, from one angle to another, clockwise. `radius` is the
// centre of the stroke, as the constants give it; LVGL wants the outer edge.
void gauge_arc(lv_draw_ctx_t* ctx, const lv_point_t& centre, lv_coord_t radius,
               lv_coord_t width, int from, int to, uint32_t colour) {
  if (to <= from) {
    return;
  }
  lv_draw_arc_dsc_t dsc;
  lv_draw_arc_dsc_init(&dsc);
  dsc.color = lv_color_hex(colour);
  dsc.width = width;
  dsc.rounded = 1;
  lv_draw_arc(ctx, &dsc, &centre, static_cast<uint16_t>(radius + width / 2),
              static_cast<uint16_t>(from % 360), static_cast<uint16_t>(to % 360));
}

// Drawn rather than built from lv_arc widgets: eight arcs a card, four cards, as
// widgets with their own styles overran the simulator's LVGL pool.
void draw_gauge(lv_event_t* event) {
  const Card* card = static_cast<const Card*>(lv_event_get_user_data(event));
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
  lv_area_t area;
  lv_obj_get_coords(lv_event_get_target(event), &area);
  const lv_point_t centre = {static_cast<lv_coord_t>(area.x1 + lv_area_get_width(&area) / 2),
                             static_cast<lv_coord_t>(area.y1 + lv_area_get_height(&area) / 2)};
  const EnvScale& scale = card->scale;

  // The bands, thin, outside the track. Each is pulled in two degrees at an inner
  // join, so the round caps of neighbours do not run into each other.
  for (uint8_t i = 0; i < scale.band_count; ++i) {
    const bool last = i + 1 >= scale.band_count;
    const float to = last ? scale.max : scale.bands[i + 1].start;
    gauge_arc(ctx, centre, BANDS_R, BANDS_W,
              gauge_angle(scale, scale.bands[i].start) + (i > 0 ? 2 : 0),
              gauge_angle(scale, to) - (last ? 0 : 2),
              card->grey ? COLOUR_LINE : band_colour(scale.bands[i].band));
  }

  gauge_arc(ctx, centre, GAUGE_R, GAUGE_W, GAUGE_START, GAUGE_START + GAUGE_SWEEP, COLOUR_LINE);
  if (card->temperature.known) {
    const int to = gauge_angle(scale, card->temperature.value);
    // At the very bottom of the scale the arc is still a dot, not nothing.
    gauge_arc(ctx, centre, GAUGE_R, GAUGE_W, GAUGE_START, to > GAUGE_START ? to : GAUGE_START + 1,
              card->grey ? PUCK_COLOUR_MUTED : card->temperature_colour);
  }

  // The last 24 hours, inside the track. A range too narrow to see as an arc
  // still shows, as a dot.
  if (card->day_min.known && card->day_max.known && card->day_max.value >= card->day_min.value) {
    const int from = gauge_angle(scale, card->day_min.value);
    const int to = gauge_angle(scale, card->day_max.value);
    gauge_arc(ctx, centre, RANGE_R, RANGE_W, from, to > from ? to : from + 1, COLOUR_RANGE);
  }
}

void draw_battery(lv_event_t* event) {
  const Card* card = static_cast<const Card*>(lv_event_get_user_data(event));
  lv_obj_t* bar = lv_event_get_target(event);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
  lv_area_t area;
  lv_obj_get_coords(bar, &area);
  const lv_coord_t width = lv_area_get_width(&area);
  const EnvScale& scale = card->battery_scale;
  const float span = scale.max - scale.min;
  int lit = 0;
  if (card->battery.known && span > 0.0f) {
    lit = static_cast<int>((card->battery.value - scale.min) / span * BATTERY_SEGMENTS + 0.5f);
    lit = lit < 0 ? 0 : lit > BATTERY_SEGMENTS ? BATTERY_SEGMENTS : lit;
  }
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.radius = 1;
  for (int i = 0; i < BATTERY_SEGMENTS; ++i) {
    // Each lit segment in the colour of the band its own voltage is in, so the
    // bar reads red, then amber, then green, as the web card does.
    uint32_t colour = COLOUR_LINE;
    if (i < lit) {
      const float at = scale.min + span * (i + 0.5f) / BATTERY_SEGMENTS;
      colour = card->grey ? PUCK_COLOUR_MUTED : band_colour(enviot_band_at(scale, at));
    }
    dsc.bg_color = lv_color_hex(colour);
    lv_area_t seg;
    seg.x1 = area.x1 + static_cast<lv_coord_t>(i * width / BATTERY_SEGMENTS);
    seg.x2 = area.x1 + static_cast<lv_coord_t>((i + 1) * width / BATTERY_SEGMENTS) - 3;
    seg.y1 = area.y1;
    seg.y2 = area.y2;
    lv_draw_rect(ctx, &dsc, &seg);
  }
}

void build_card(Card& card) {
  card.box = plain(s_root);
  lv_obj_set_size(card.box, CARD_W, CARD_H);
  lv_obj_set_style_radius(card.box, 16, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card.box, lv_color_hex(COLOUR_CARD), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card.box, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card.box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(card.box, lv_color_hex(COLOUR_LINE), LV_PART_MAIN);

  card.swatch = plain(card.box);
  lv_obj_set_size(card.swatch, 14, 14);
  lv_obj_set_pos(card.swatch, 22, 24);
  lv_obj_set_style_radius(card.swatch, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card.swatch, LV_OPA_COVER, LV_PART_MAIN);

  card.name = label(card.box, &puck_font_24, PUCK_COLOUR_TEXT);
  lv_label_set_long_mode(card.name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(card.name, 190);
  lv_obj_set_pos(card.name, 46, 17);

  card.when = label(card.box, &puck_font_18, PUCK_COLOUR_MUTED);
  lv_label_set_long_mode(card.when, LV_LABEL_LONG_DOT);
  lv_obj_set_width(card.when, 190);
  lv_obj_set_style_text_align(card.when, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_align(card.when, LV_ALIGN_TOP_RIGHT, -22, 22);

  const lv_coord_t gauge_size = 2 * BANDS_R + 2 * BANDS_W + 4;
  card.gauge = plain(card.box);
  lv_obj_set_size(card.gauge, gauge_size, gauge_size);
  lv_obj_set_pos(card.gauge, GAUGE_X - gauge_size / 2, GAUGE_Y - gauge_size / 2);
  lv_obj_add_event_cb(card.gauge, draw_gauge, LV_EVENT_DRAW_MAIN, &card);

  card.reading = label(card.box, &puck_font_32, PUCK_COLOUR_TEXT);
  card.reading_unit = label(card.box, &puck_font_14, PUCK_COLOUR_MUTED);
  card.scale_min = label(card.box, &puck_font_14, PUCK_COLOUR_MUTED);
  card.scale_max = label(card.box, &puck_font_14, PUCK_COLOUR_MUTED);

  const lv_coord_t rows_w = CARD_W - ROWS_X - 22;
  for (int i = 0; i < ROWS; ++i) {
    Row& row = card.rows[i];
    const lv_coord_t y = ROWS_Y + i * ROW_PITCH;
    if (i > 0) {
      lv_obj_t* line = plain(card.box);
      lv_obj_set_size(line, rows_w, 1);
      lv_obj_set_pos(line, ROWS_X, y - 8);
      lv_obj_set_style_bg_color(line, lv_color_hex(COLOUR_LINE), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    }
    row.name = label(card.box, &puck_font_18, PUCK_COLOUR_MUTED);
    lv_obj_set_pos(row.name, ROWS_X, y + 2);
    // The units in a column of their own at the right edge, each value right-
    // aligned against it. Fixed widths: a content-sized pair aligned to the right
    // grows off the card's edge as its text lengthens.
    row.unit = label(card.box, &puck_font_14, PUCK_COLOUR_MUTED);
    lv_obj_set_width(row.unit, UNIT_W);
    lv_obj_set_pos(row.unit, CARD_W - 22 - UNIT_W, y + 6);
    row.value = label(card.box, &puck_font_20, PUCK_COLOUR_TEXT);
    lv_obj_set_width(row.value, VALUE_W);
    lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_pos(row.value, CARD_W - 22 - UNIT_W - 6 - VALUE_W, y);
  }

  lv_obj_t* line = plain(card.box);
  lv_obj_set_size(line, CARD_W - 44, 1);
  lv_obj_set_pos(line, 22, CARD_H - 50);
  lv_obj_set_style_bg_color(line, lv_color_hex(COLOUR_LINE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t* battery = label(card.box, &puck_font_18, PUCK_COLOUR_MUTED);
  lv_label_set_text(battery, "Battery");
  lv_obj_set_pos(battery, 22, CARD_H - 37);
  card.battery_bar = plain(card.box);
  lv_obj_set_size(card.battery_bar, CARD_W - 22 - BATTERY_BAR_X - BATTERY_VALUE_W, BATTERY_BAR_H);
  lv_obj_set_pos(card.battery_bar, BATTERY_BAR_X, CARD_H - 26 - BATTERY_BAR_H / 2);
  lv_obj_add_event_cb(card.battery_bar, draw_battery, LV_EVENT_DRAW_MAIN, &card);
  card.battery_value = label(card.box, &puck_font_20, PUCK_COLOUR_TEXT);
  lv_obj_align(card.battery_value, LV_ALIGN_BOTTOM_RIGHT, -22, -15);
}

void place_cards(uint8_t count) {
  for (uint8_t i = 0; i < ENV_MAX_DEVICES; ++i) {
    lv_obj_t* box = s_cards[i].box;
    set_hidden(box, i >= count);
    if (count <= 2) {
      // One or two: a row across the middle, centred.
      const lv_coord_t row_w = count * CARD_W + (count - 1) * GAP;
      lv_obj_set_pos(box, (PUCK_LCD_WIDTH - row_w) / 2 + i * (CARD_W + GAP),
                     GRID_Y + (GRID_H - CARD_H) / 2);
    } else {
      lv_obj_set_pos(box, PAD_X + (i % 2) * (CARD_W + GAP), GRID_Y + (i / 2) * (CARD_H + GAP));
    }
  }
}

void update_gauge(Card& card, const EnvReadings& r, const EnvDevice& d) {
  const EnvScale& scale = r.temperature;
  card.scale = scale;
  EnvBand band = d.temperature_band;
  if (band == EnvBand::None && d.temperature.known) {
    band = enviot_band_at(scale, d.temperature.value);
  }
  card.temperature = d.temperature;
  card.temperature_colour = band_colour(band);
  card.day_min = d.day_min;
  card.day_max = d.day_max;
  lv_obj_invalidate(card.gauge);

  char text[16];
  enviot_format_value(d.temperature, r.temperature_decimals, text, sizeof(text));
  set_text(card.reading, text);
  set_colour(card.reading, card.grey ? PUCK_COLOUR_MUTED : PUCK_COLOUR_TEXT);
  lv_obj_set_pos(card.reading, GAUGE_X - lv_obj_get_width(card.reading) / 2, GAUGE_Y - 22);
  set_text(card.reading_unit, r.temperature_unit);
  lv_obj_set_pos(card.reading_unit, GAUGE_X - lv_obj_get_width(card.reading_unit) / 2,
                 GAUGE_Y + 16);

  // The scale's ends, outside the arc's open ends.
  MaybeFloat end;
  end.known = true;
  end.value = scale.min;
  enviot_format_value(end, 0, text, sizeof(text));
  set_text(card.scale_min, text);
  end.value = scale.max;
  enviot_format_value(end, 0, text, sizeof(text));
  set_text(card.scale_max, text);
  // cos/sin of 135 and 45 degrees: the arc's open ends sit at +-0.707 of the radius.
  const lv_coord_t end_dx = static_cast<lv_coord_t>(BANDS_R * 0.707f);
  const lv_coord_t end_y = GAUGE_Y + end_dx - 2;
  lv_obj_set_pos(card.scale_min, GAUGE_X - end_dx - 10 - lv_obj_get_width(card.scale_min) / 2,
                 end_y);
  lv_obj_set_pos(card.scale_max, GAUGE_X + end_dx + 10 - lv_obj_get_width(card.scale_max) / 2,
                 end_y);
}

void update_card(Card& card, const EnvReadings& r, const EnvDevice& d, int64_t now) {
  card.grey = d.stale;
  const uint32_t text_colour = card.grey ? PUCK_COLOUR_MUTED : PUCK_COLOUR_TEXT;
  const bool alert = d.alert[0] != '\0';
  // Two pixels of red for an alert: the border's one and an outline's one just
  // outside it. A wider border would move everything inside the card.
  lv_obj_set_style_border_color(card.box, lv_color_hex(alert ? PUCK_COLOUR_ALARM : COLOUR_LINE),
                                LV_PART_MAIN);
  lv_obj_set_style_outline_width(card.box, alert ? 1 : 0, LV_PART_MAIN);
  lv_obj_set_style_outline_color(card.box, lv_color_hex(PUCK_COLOUR_ALARM), LV_PART_MAIN);

  lv_obj_set_style_bg_color(card.swatch, lv_color_hex(card.grey ? PUCK_COLOUR_MUTED : d.colour),
                            LV_PART_MAIN);
  set_text(card.name, d.name);
  set_colour(card.name, text_colour);

  char text[48];
  const int64_t age = d.last_seen > 0 && now > d.last_seen ? now - d.last_seen : 0;
  if (alert) {
    snprintf(text, sizeof(text), "%s", d.alert);
  } else if (d.last_seen == 0) {
    snprintf(text, sizeof(text), "No data yet");
  } else if (d.stale) {
    enviot_format_stale(age, text, sizeof(text));
  } else {
    enviot_format_ago(age, text, sizeof(text));
  }
  set_text(card.when, text);
  set_colour(card.when, alert ? PUCK_COLOUR_ALARM : PUCK_COLOUR_MUTED);

  update_gauge(card, r, d);

  for (int i = 0; i < ROWS; ++i) {
    Row& row = card.rows[i];
    const bool shown = i == 0 || i - 1 < d.row_count;
    set_hidden(row.name, !shown);
    set_hidden(row.value, !shown);
    set_hidden(row.unit, !shown);
    if (!shown) {
      continue;
    }
    if (i == 0) {
      set_text(row.name, "24 h");
      char low[16];
      char high[16];
      enviot_format_value(d.day_min, r.temperature_decimals, low, sizeof(low));
      enviot_format_value(d.day_max, r.temperature_decimals, high, sizeof(high));
      if (d.day_min.known && d.day_max.known) {
        snprintf(text, sizeof(text), "%s \xE2\x80\x93 %s", low, high);
      } else {
        snprintf(text, sizeof(text), "--");
      }
      set_text(row.value, text);
      set_text(row.unit, r.temperature_unit);
    } else {
      const EnvRow& source = d.rows[i - 1];
      set_text(row.name, source.label);
      enviot_format_value(source.value, source.decimals, text, sizeof(text));
      set_text(row.value, text);
      set_text(row.unit, source.unit);
    }
    set_colour(row.value, text_colour);
  }

  card.battery = d.battery;
  card.battery_scale = r.battery;
  if (d.battery.known) {
    enviot_format_value(d.battery, 2, text, sizeof(text));
    strncat(text, " V", sizeof(text) - strlen(text) - 1);
  } else {
    snprintf(text, sizeof(text), "--");
  }
  set_text(card.battery_value, text);
  set_colour(card.battery_value, text_colour);
  lv_obj_invalidate(card.battery_bar);
}

}  // namespace

lv_obj_t* screen_environment_create(lv_obj_t* parent) {
  s_root = plain(parent);
  lv_obj_set_size(s_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t* caption = label(s_root, &puck_font_18, PUCK_COLOUR_MUTED);
  lv_label_set_text(caption, "ENVIRONMENT");
  lv_obj_set_pos(caption, PAD_X, 26);

  // Beside the caption: why the readings have stopped updating, when they have.
  s_note = label(s_root, &puck_font_18, PUCK_COLOUR_MUTED);
  lv_obj_align(s_note, LV_ALIGN_TOP_RIGHT, -PAD_X, 26);

  for (Card& card : s_cards) {
    build_card(card);
  }
  place_cards(0);

  s_message = label(s_root, &puck_font_28, PUCK_COLOUR_MUTED);
  lv_obj_set_width(s_message, 720);
  lv_obj_set_style_text_align(s_message, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(s_message, LV_LABEL_LONG_WRAP);
  lv_label_set_text(s_message, "Waiting for EnViOT Display");
  lv_obj_center(s_message);
  return s_root;
}

void screen_environment_update(const EnvReadings& r, uint32_t elapsed_s) {
  if (s_root == nullptr) {
    return;
  }
  const char* message = nullptr;
  if (!r.has_readings) {
    message = r.status == EnvStatus::Refused  ? "EnViOT refused this panel's code. Set it again "
                                                "on the Data page in settings."
              : r.status == EnvStatus::Failed ? "Can't reach EnViOT Display"
                                              : "Waiting for EnViOT Display";
  } else if (r.count == 0) {
    message = "EnViOT Display has no sensors to show";
  }
  set_hidden(s_message, message == nullptr);
  if (message != nullptr) {
    set_text(s_message, message);
    place_cards(0);
    set_text(s_note, "");
    return;
  }

  // Old readings stay up when a read fails, marked as old rather than replaced by
  // a message: the last temperature is still worth seeing.
  char note[48] = {};
  if (r.status == EnvStatus::Refused) {
    snprintf(note, sizeof(note), "Code refused");
  } else if (r.status == EnvStatus::Failed && elapsed_s >= 120) {
    snprintf(note, sizeof(note), "Not updated for %u min", static_cast<unsigned>(elapsed_s / 60));
  }
  set_text(s_note, note);
  set_colour(s_note, r.status == EnvStatus::Refused ? PUCK_COLOUR_ALARM : PUCK_COLOUR_MUTED);
  lv_obj_align(s_note, LV_ALIGN_TOP_RIGHT, -PAD_X, 26);

  place_cards(r.count);
  const int64_t now = r.now + elapsed_s;
  for (uint8_t i = 0; i < r.count; ++i) {
    update_card(s_cards[i], r, r.devices[i], now);
  }
}
