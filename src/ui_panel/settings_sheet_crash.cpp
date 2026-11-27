// The settings sheet's crash view: why the panel last started, and the crash
// the core dump saved in flash, laid out to be photographed and decoded on a
// computer. Opened from the Device page.
//
// The decoding is not done here. RISC-V keeps no backtrace on the device, and
// the names of functions are not in the image, so the screen shows addresses
// and the build they belong to; tools/decode_crash.sh turns them into
// functions and lines against that build's firmware.elf.

#include <stdio.h>
#include <string.h>

#include "settings_sheet_internal.h"

namespace sheet_kit {

namespace {

SheetCrash s_crash;
lv_obj_t* s_modal = nullptr;
lv_obj_t* s_body = nullptr;

void refresh() {
  if (s_body == nullptr) {
    return;
  }
  char text[640];
  size_t used = 0;
  auto add = [&](const char* format, auto... args) {
    if (used < sizeof(text)) {
      const int n = snprintf(text + used, sizeof(text) - used, format, args...);
      if (n > 0) {
        used += static_cast<size_t>(n);
      }
    }
  };

  add("Last restart: %s\n", restart_text(s_crash.restart));
  if (!s_crash.saved) {
    add("No crash is saved in flash.");
    lv_label_set_text(s_body, text);
    return;
  }
  add("Saved crash: %s, build %s\n", s_crash.this_build ? "this firmware" : "an earlier firmware",
      s_crash.build);
  char cause[48];
  add("Task: %s\n", s_crash.task[0] != '\0' ? s_crash.task : "--");
  add("Cause: %s (mcause %08lx, mtval %08lx)\n", crash_cause_text(s_crash.mcause, s_crash.mtval, cause, sizeof(cause)),
      static_cast<unsigned long>(s_crash.mcause), static_cast<unsigned long>(s_crash.mtval));
  add("PC %08lx   RA %08lx   SP %08lx\n", static_cast<unsigned long>(s_crash.pc),
      static_cast<unsigned long>(s_crash.ra), static_cast<unsigned long>(s_crash.sp));
  add("Stack:");
  if (s_crash.stack_count == 0) {
    add(" none found");
  }
  for (uint8_t i = 0; i < s_crash.stack_count && i < SHEET_CRASH_STACK; ++i) {
    add("%s%08lx", i == 4 ? "\n            " : "  ", static_cast<unsigned long>(s_crash.stack[i]));
  }
  lv_label_set_text(s_body, text);
}

}  // namespace

const char* restart_text(SheetRestart restart) {
  switch (restart) {
    case SheetRestart::PowerOn:
      return "power on";
    case SheetRestart::Software:
      return "restarted on purpose";
    case SheetRestart::Crash:
      return "crash";
    case SheetRestart::TaskWatchdog:
      return "task watchdog";
    case SheetRestart::IntWatchdog:
      return "interrupt watchdog";
    case SheetRestart::Watchdog:
      return "watchdog";
    case SheetRestart::PowerDip:
      return "power dip";
    case SheetRestart::ResetPin:
      return "reset pin";
    case SheetRestart::Other:
      return "other";
    case SheetRestart::Unknown:
      break;
  }
  return "--";
}

bool restart_unexpected(SheetRestart restart) {
  switch (restart) {
    case SheetRestart::Crash:
    case SheetRestart::TaskWatchdog:
    case SheetRestart::IntWatchdog:
    case SheetRestart::Watchdog:
    case SheetRestart::PowerDip:
      return true;
    default:
      return false;
  }
}

const char* crash_cause_text(uint32_t mcause, uint32_t mtval, char* out, size_t size) {
  // abort() and a failed assert end in panic_abort, which stops the CPU with an
  // all-zero instruction: an illegal instruction whose bits (mtval) are 0.
  if (mcause == 2 && mtval == 0) {
    snprintf(out, size, "abort: a failed assert, or abort()");
    return out;
  }
  // The top bit marks an interrupt rather than an exception: on this chip, a
  // watchdog's or a cross-core panic's.
  if ((mcause & 0x80000000u) != 0) {
    snprintf(out, size, "interrupt %lu", static_cast<unsigned long>(mcause & 0x7FFFFFFFu));
    return out;
  }
  static const char* const EXCEPTION[] = {
      "instruction misaligned", "instruction access fault", "illegal instruction",
      "breakpoint", "load misaligned", "load access fault",
      "store misaligned", "store access fault", "environment call", "environment call",
      "exception 10", "environment call",
  };
  if (mcause < sizeof(EXCEPTION) / sizeof(EXCEPTION[0])) {
    snprintf(out, size, "%s", EXCEPTION[mcause]);
  } else {
    snprintf(out, size, "exception %lu", static_cast<unsigned long>(mcause));
  }
  return out;
}

void crash_build_modal(lv_obj_t* root) {
  s_modal = modal_backdrop(root);
  lv_obj_t* box = card(s_modal, 820, LV_SIZE_CONTENT);
  lv_obj_center(box);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  lv_obj_set_style_pad_row(box, 14, LV_PART_MAIN);

  caption(box, "LAST CRASH");
  s_body = text(box, FONT_TEXT, PUCK_COLOUR_TEXT, "");
  lv_obj_set_style_text_line_space(s_body, 6, LV_PART_MAIN);
  hint(box, "Take a photo of this screen: the addresses are looked up against this build "
            "on a computer.");

  lv_obj_t* buttons = row(box);
  lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t* close = button(buttons, "Close", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(close, [](lv_event_t*) { crash_close(); }, LV_EVENT_CLICKED, nullptr);
}

void crash_set(const SheetCrash& crash) {
  s_crash = crash;
  refresh();
}

const SheetCrash& crash_get() {
  return s_crash;
}

void crash_open() {
  if (s_modal == nullptr) {
    return;
  }
  refresh();
  lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_modal);
}

bool crash_modal_open() {
  return s_modal != nullptr && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

void crash_close() {
  if (s_modal != nullptr) {
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace sheet_kit
